#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mcdk/plugin/abi/entry.h>
#include <mcdk/plugin/abi/events.h>
#include <mcdk/plugin/abi/iface/console.h>
#include <mcdk/plugin/abi/iface/core.h>
#include <mcdk/plugin/abi/iface/events.h>
#include <mcdk/plugin/abi/iface/info.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

mcdk_str View(std::string_view text) {
    return {text.data(), text.size()};
}

std::string Text(mcdk_str text) {
    return text.len == 0 ? std::string() : std::string(text.ptr, text.len);
}

class Library {
public:
    explicit Library(const std::filesystem::path& path) {
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Require(module_ != nullptr, "Cannot load the game-speed host plugin DLL.");
        entry = reinterpret_cast<mcdk_plugin_entry_fn>(GetProcAddress(module_, MCDK_PLUGIN_ENTRY_SYMBOL));
        if (entry == nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw std::runtime_error("The host plugin does not export mcdk_plugin_entry.");
        }
    }
    ~Library() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    mcdk_plugin_entry_fn entry = nullptr;

private:
    HMODULE module_ = nullptr;
};

// QueryInterface has no instance argument. Only one fake host is alive at a
// time. Readiness tests use this test process's PID, which the injector rejects
// before any remote process access. No Minecraft process is launched.
class FakeHost {
public:
    explicit FakeHost(std::string config) : config_(std::move(config)) {
        Require(active_ == nullptr, "Only one fake host may be active.");
        active_ = this;
        core_.struct_size = sizeof(core_);
        core_.get_config = GetConfig;
        events_.struct_size = sizeof(events_);
        events_.resolve = Resolve;
        events_.subscribe = Subscribe;
        events_.unsubscribe = Unsubscribe;
        console_.struct_size = sizeof(console_);
        console_.log = Log;
        sessionInfo_.struct_size = sizeof(sessionInfo_);
        sessionInfo_.get_session = GetSession;
        sessionInfo_.get_ipc_clients = GetClients;
        session.struct_size = sizeof(session);
        session.game_ipc_port = 24444;
        session.game_state = MCDK_GAME_LOADING;
        info.struct_size = sizeof(info);
        info.abi_major = MCDK_ABI_VERSION_MAJOR;
        info.abi_minor = MCDK_ABI_VERSION_MINOR;
        info.host_version = View("host-plugin-test");
        info.self = 1;
        info.get_interface = GetInterface;
    }
    ~FakeHost() { active_ = nullptr; }
    FakeHost(const FakeHost&) = delete;
    FakeHost& operator=(const FakeHost&) = delete;

    void GameExit(std::uint32_t pid) {
        const auto subscription = subscriptions_.find(ExitEvent);
        Require(subscription != subscriptions_.end(), "Plugin did not subscribe to game exit.");
        mcdk_ev_game_exit payload{};
        payload.struct_size = sizeof(payload);
        payload.pid = pid;
        const mcdk_event event{sizeof(mcdk_event), ExitEvent, 1, sizeof(payload), &payload};
        Require(subscription->second.handler(&event, subscription->second.user) == MCDK_EVENT_CONTINUE,
                "Game exit event returned an unexpected result.");
    }

    void Connected(std::uint32_t clientCount) {
        const auto subscription = subscriptions_.find(ConnectedEvent);
        Require(subscription != subscriptions_.end(), "Plugin did not subscribe to game debug IPC connections.");
        mcdk_ev_ipc_client payload{};
        payload.struct_size = sizeof(payload);
        payload.client_count = clientCount;
        payload.port = 50123;
        const mcdk_event event{sizeof(mcdk_event), ConnectedEvent, 1, sizeof(payload), &payload};
        Require(subscription->second.handler(&event, subscription->second.user) == MCDK_EVENT_CONTINUE,
                "IPC connection event returned an unexpected result.");
    }

    std::size_t CountLog(mcdk_log_level level, std::string_view fragment) const {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        return CountLogLocked(level, fragment);
    }

    bool HasLog(mcdk_log_level level, std::string_view fragment) const {
        return CountLog(level, fragment) != 0;
    }

    bool WaitForLog(mcdk_log_level level, std::string_view fragment) {
        std::unique_lock<std::mutex> lock(messagesMutex_);
        return messagesChanged_.wait_for(lock, std::chrono::seconds(2), [&] {
            return CountLogLocked(level, fragment) != 0;
        });
    }

    mcdk_host_info info{};
    mcdk_session_info session{};
    std::vector<std::uint16_t> ipcPorts;
    bool mcpUnavailableQueried = false;
    std::vector<std::string> resolvedEvents;

private:
    std::size_t CountLogLocked(mcdk_log_level level, std::string_view fragment) const {
        std::size_t count = 0;
        for (const auto& [messageLevel, message] : messages_) {
            if (messageLevel == level && message.find(fragment) != std::string::npos) {
                ++count;
            }
        }
        return count;
    }

    struct Subscription {
        mcdk_event_handler handler = nullptr;
        void* user = nullptr;
    };
    static constexpr std::uint32_t ExitEvent = 1;
    static constexpr std::uint32_t ConnectedEvent = 2;
    static FakeHost* active_;

    static const void* MCDK_CALL GetInterface(const char* name, std::uint32_t version) {
        if (active_ == nullptr || name == nullptr || version != 1) {
            return nullptr;
        }
        const std::string_view key(name);
        if (key == MCDK_IFACE_CORE_NAME) {
            return &active_->core_;
        }
        if (key == MCDK_IFACE_EVENTS_NAME) {
            return &active_->events_;
        }
        if (key == MCDK_IFACE_CONSOLE_NAME) {
            return &active_->console_;
        }
        if (key == MCDK_IFACE_INFO_NAME) {
            return &active_->sessionInfo_;
        }
        // The SDK queries optional interfaces at entry. Run every lifecycle
        // test with MCP unavailable, as in a session with its server disabled.
        if (key == "mcdk.mcp") {
            active_->mcpUnavailableQueried = true;
            return nullptr;
        }
        return nullptr;
    }

    static void MCDK_CALL GetConfig(mcdk_handle, mcdk_str* output) {
        if (active_ != nullptr && output != nullptr) {
            *output = View(active_->config_);
        }
    }

    static std::uint32_t MCDK_CALL Resolve(mcdk_handle, mcdk_str name) {
        const auto text = Text(name);
        if (active_ != nullptr) {
            active_->resolvedEvents.push_back(text);
        }
        if (text == MCDK_EVENT_GAME_EXIT) {
            return ExitEvent;
        }
        if (text == MCDK_EVENT_IPC_CLIENT_CONNECTED) {
            return ConnectedEvent;
        }
        return 0;
    }

    static mcdk_status MCDK_CALL GetSession(mcdk_handle, mcdk_session_info* output) {
        if (active_ == nullptr || output == nullptr || output->struct_size < sizeof(*output)) {
            return MCDK_ERR_INVALID_ARGUMENT;
        }
        *output = active_->session;
        return MCDK_OK;
    }

    static mcdk_status MCDK_CALL GetClients(mcdk_handle, std::uint16_t* output,
                                          std::size_t capacity, std::size_t* count) {
        if (active_ == nullptr || count == nullptr) {
            return MCDK_ERR_INVALID_ARGUMENT;
        }
        *count = active_->ipcPorts.size();
        if (capacity < *count) {
            return MCDK_ERR_BUFFER_TOO_SMALL;
        }
        for (std::size_t index = 0; index < *count; ++index) {
            output[index] = active_->ipcPorts[index];
        }
        return MCDK_OK;
    }

    static mcdk_handle MCDK_CALL Subscribe(mcdk_handle, std::uint32_t eventId,
                                         mcdk_dispatch_mode, std::int32_t,
                                         mcdk_event_handler handler, void* user) {
        if (active_ == nullptr || handler == nullptr) {
            return 0;
        }
        active_->subscriptions_[eventId] = {handler, user};
        return eventId;
    }

    static void MCDK_CALL Unsubscribe(mcdk_handle, mcdk_handle token) {
        if (active_ != nullptr) {
            active_->subscriptions_.erase(static_cast<std::uint32_t>(token));
        }
    }

    static void MCDK_CALL Log(mcdk_handle, mcdk_log_level level, mcdk_str message) {
        if (active_ != nullptr) {
            {
                std::lock_guard<std::mutex> lock(active_->messagesMutex_);
                active_->messages_.emplace_back(level, Text(message));
            }
            active_->messagesChanged_.notify_all();
        }
    }

    std::string config_;
    mcdk_iface_core core_{};
    mcdk_iface_events events_{};
    mcdk_iface_console console_{};
    mcdk_iface_info sessionInfo_{};
    std::unordered_map<std::uint32_t, Subscription> subscriptions_;
    std::vector<std::pair<mcdk_log_level, std::string>> messages_;
    mutable std::mutex messagesMutex_;
    std::condition_variable messagesChanged_;
};

FakeHost* FakeHost::active_ = nullptr;

class Instance {
public:
    Instance(const Library& library, FakeHost& host) {
        Require(library.entry(&host.info, &descriptor_) == MCDK_TRUE,
                "The plugin rejected a compatible host ABI.");
        Require(descriptor_.struct_size == sizeof(descriptor_) && descriptor_.abi_major == 1 &&
                    descriptor_.abi_minor == 0,
                "Plugin descriptor has an unexpected ABI.");
        Require(Text(descriptor_.id) == "com.github-zero123.game-speed", "Unexpected plugin identity.");
        Require(descriptor_.on_stage != nullptr && descriptor_.on_unload != nullptr,
                "Plugin descriptor is missing lifecycle callbacks.");
    }
    ~Instance() {
        if (!shutdown_) {
            descriptor_.on_stage(descriptor_.user, MCDK_STAGE_SHUTDOWN);
        }
        descriptor_.on_unload(descriptor_.user);
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    mcdk_status Stage(mcdk_stage stage) {
        const auto status = descriptor_.on_stage(descriptor_.user, stage);
        if (stage == MCDK_STAGE_SHUTDOWN) {
            shutdown_ = true;
        }
        return status;
    }

private:
    mcdk_plugin_desc descriptor_{};
    bool shutdown_ = false;
};

void CheckConfig(const Library& library) {
    const std::vector<std::pair<std::string, bool>> cases{
        {"null", true},
        {"{}", true},
        {R"({"autoInject":false,"initialSpeed":0,"showUi":true})", true},
        {R"({"initialSpeed":0.01,"showUi":false})", true},
        {R"({"initialSpeed":16})", true},
        {R"({"initialSpeed":0.005})", false},
        {R"({"initialSpeed":17})", false},
        {R"({"autoInject":"false"})", false},
        {R"({"showUi":0})", false},
        {R"({"bogus":true})", false},
        {"[]", false},
        {"{", false},
        {R"({"initialSpeed":true})", false},
        {R"({"initialSpeed":-1})", false},
    };
    for (const auto& [config, valid] : cases) {
        FakeHost host(config);
        Instance plugin(library, host);
        const auto status = plugin.Stage(MCDK_STAGE_CONFIG);
        Require((status == MCDK_OK) == valid, "Config validation failed for " + config);
        Require(plugin.Stage(MCDK_STAGE_SHUTDOWN) == MCDK_OK, "Config instance shutdown failed.");
    }
}

void CheckAbiRejection(const Library& library) {
    FakeHost host("{}");
    mcdk_plugin_desc descriptor{};
    Require(library.entry(nullptr, &descriptor) == MCDK_FALSE, "A null host was accepted.");
    Require(library.entry(&host.info, nullptr) == MCDK_FALSE, "A null descriptor was accepted.");
    auto incompatible = host.info;
    incompatible.abi_major = MCDK_ABI_VERSION_MAJOR + 1;
    Require(library.entry(&incompatible, &descriptor) == MCDK_FALSE,
            "An incompatible host ABI major version was accepted.");
    incompatible = host.info;
    incompatible.struct_size = sizeof(std::uint32_t);
    Require(library.entry(&incompatible, &descriptor) == MCDK_FALSE,
            "A truncated host descriptor was accepted.");
}

void CheckLifecycle(const Library& library) {
    for (const auto config : {R"({"autoInject":false})", R"({"autoInject":true})"}) {
        FakeHost host(config);
        Instance plugin(library, host);
        Require(host.mcpUnavailableQueried, "The test host did not exercise unavailable MCP.");
        Require(plugin.Stage(MCDK_STAGE_REGISTER) == MCDK_OK, "Registration stage failed without MCP.");
        Require(host.resolvedEvents == std::vector<std::string>{MCDK_EVENT_GAME_EXIT, MCDK_EVENT_IPC_CLIENT_CONNECTED},
                "The plugin subscribed to an unexpected event.");
        Require(plugin.Stage(MCDK_STAGE_CONFIG) == MCDK_OK, "Configuration stage failed.");
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "No-process runtime stage failed.");
        Require(host.HasLog(MCDK_LOG_ERROR, "did not provide a game process ID"),
                "Runtime without a game PID did not report the injection failure.");
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Runtime stage re-entry failed.");
        Require(host.HasLog(MCDK_LOG_WARN, "runtime stage was already handled"),
                "Repeated runtime did not report that it had already run.");
        host.GameExit(0);
        host.GameExit(123);
        Require(plugin.Stage(MCDK_STAGE_SHUTDOWN) == MCDK_OK, "Shutdown stage failed.");
        Require(plugin.Stage(MCDK_STAGE_SHUTDOWN) == MCDK_OK, "Repeated shutdown stage failed.");
    }

    // Exercise unload's fallback shutdown with a fresh instance.
    FakeHost host("{}");
    Instance plugin(library, host);
    Require(plugin.Stage(MCDK_STAGE_REGISTER) == MCDK_OK, "Fallback lifecycle registration failed.");
    Require(plugin.Stage(MCDK_STAGE_CONFIG) == MCDK_OK, "Fallback lifecycle configuration failed.");
    Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Fallback lifecycle runtime failed.");
}

void Configure(Instance& plugin) {
    Require(plugin.Stage(MCDK_STAGE_REGISTER) == MCDK_OK, "Readiness registration failed.");
    Require(plugin.Stage(MCDK_STAGE_CONFIG) == MCDK_OK, "Readiness configuration failed.");
}

constexpr std::string_view InjectionStarted = "IPC ready; starting DLL injection";

void RequireOneRejectedAttempt(FakeHost& host) {
    // This process is deliberately rejected by Inject before reading its
    // modules. The error proves that the asynchronous worker actually ran.
    Require(host.WaitForLog(MCDK_LOG_ERROR, "Provide another Windows x64 process PID"),
            "Readiness did not start the expected safe injection attempt.");
    Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 1,
            "Readiness started more than one injection worker.");
}

void CheckReadinessGate(const Library& library) {
    {
        FakeHost host("{}");
        host.session.game_pid = GetCurrentProcessId();
        Instance plugin(library, host);
        Configure(plugin);
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Waiting runtime failed.");
        Require(host.HasLog(MCDK_LOG_INFO, "waiting for the first game debug IPC connection"),
                "Runtime did not explain why injection was deferred.");
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 0,
                "Runtime injected before any game debug IPC connection.");
        host.Connected(0);
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 0,
                "An empty IPC event started injection.");
        host.Connected(1);
        RequireOneRejectedAttempt(host);
        host.Connected(2);
        host.Connected(1);
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Repeated ready runtime failed.");
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 1,
                "A reconnect or repeated runtime started another injection.");
        host.GameExit(GetCurrentProcessId());
        host.Connected(1);
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 1,
                "An IPC event after game exit restarted injection.");
        Require(plugin.Stage(MCDK_STAGE_SHUTDOWN) == MCDK_OK, "Ready shutdown failed.");
    }

    // The accept thread can deliver the first connection before runtime has
    // supplied the game PID. Remember it without injecting prematurely.
    {
        FakeHost host("{}");
        host.session.game_pid = GetCurrentProcessId();
        Instance plugin(library, host);
        Configure(plugin);
        host.Connected(1);
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 0,
                "An IPC event before runtime started injection without an armed PID.");
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Early IPC runtime failed.");
        RequireOneRejectedAttempt(host);
    }

    // A snapshot closes the other race: the first connection may already be
    // established even if its event was not observed by this plugin instance.
    for (const auto readySource : {0, 1, 2}) {
        FakeHost host("{}");
        host.session.game_pid = GetCurrentProcessId();
        if (readySource == 0) {
            host.session.game_debug_ready = MCDK_TRUE;
        } else if (readySource == 1) {
            host.session.game_state = MCDK_GAME_IN_WORLD;
        } else {
            host.ipcPorts = {50123};
        }
        Instance plugin(library, host);
        Configure(plugin);
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Already-connected runtime failed.");
        RequireOneRejectedAttempt(host);
    }

    {
        FakeHost host(R"({"autoInject":false})");
        host.session.game_pid = GetCurrentProcessId();
        host.session.game_debug_ready = MCDK_TRUE;
        Instance plugin(library, host);
        Configure(plugin);
        host.Connected(1);
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Disabled injection runtime failed.");
        host.Connected(2);
        Require(plugin.Stage(MCDK_STAGE_SHUTDOWN) == MCDK_OK, "Disabled injection shutdown failed.");
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 0,
                "autoInject=false still started injection.");
    }

    // Late events after shutdown or game exit must not publish a worker that
    // could outlive the plugin or use a stale PID.
    for (const auto stopReason : {0, 1, 2}) {
        FakeHost host("{}");
        host.session.game_pid = GetCurrentProcessId();
        Instance plugin(library, host);
        Configure(plugin);
        if (stopReason == 2) {
            host.GameExit(GetCurrentProcessId());
        }
        Require(plugin.Stage(MCDK_STAGE_RUNTIME) == MCDK_OK, "Late-event runtime failed.");
        if (stopReason == 0) {
            Require(plugin.Stage(MCDK_STAGE_SHUTDOWN) == MCDK_OK, "Late-event shutdown failed.");
        } else if (stopReason == 1) {
            host.GameExit(GetCurrentProcessId());
        }
        host.Connected(1);
        Require(host.CountLog(MCDK_LOG_INFO, InjectionStarted) == 0,
                "A connection after shutdown or game exit started injection.");
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "Usage: gamespeed_host_plugin_tests <game_speed_plugin.dll>");
        const Library library(std::filesystem::absolute(argv[1]));
        CheckAbiRejection(library);
        CheckConfig(library);
        CheckLifecycle(library);
        CheckReadinessGate(library);
        std::cout << "PASS: host plugin ABI, 14 configuration cases, lifecycle and IPC injection readiness without MCP.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
