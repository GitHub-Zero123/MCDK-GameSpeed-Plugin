#include <gamespeed/Client.hpp>
#include <mcdk/plugin/plugin.hpp>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr double MinSpeed = 0.01;
constexpr double MaxSpeed = 16.0;

std::filesystem::path PluginDirectory() {
    // An address in this DLL identifies the plugin even when the host's working
    // directory and executable are elsewhere. Do not resolve against mcdk.exe.
    static const int moduleAnchor = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) {
        throw std::runtime_error("Cannot locate the game-speed host plugin module.");
    }
    constexpr DWORD capacity = 32768;
    std::vector<wchar_t> buffer(capacity);
    const DWORD length = GetModuleFileNameW(module, buffer.data(), capacity);
    if (length == 0) {
        throw std::runtime_error("Cannot read the game-speed host plugin path.");
    }
    if (length < capacity) {
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    }
    throw std::runtime_error("The game-speed host plugin path exceeds the Windows path limit.");
}

std::string SpeedCommand(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "set " << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return text.str();
}

bool ValidSpeed(double value) {
    return std::isfinite(value) && (value == 0.0 || (value >= MinSpeed && value <= MaxSpeed));
}

class GameSpeedPlugin final : public mcdk::Plugin {
public:
    ~GameSpeedPlugin() override {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stopping_ = true;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void onRegister(mcdk::Context& context) override {
        context.events().on<mcdk::ev::GameExit>(mcdk::Dispatch::Sync,
            [this](const auto& event) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (pid_ == event.pid || !runtimeStarted_) {
                    pid_ = 0;
                    injected_ = false;
                    ipcReady_ = false;
                    gameExited_ = true;
                    ++generation_;
                }
            });
        context.events().on<mcdk::ev::IpcClientConnected>(mcdk::Dispatch::Sync,
            [this](const auto& event) {
                if (event.clientCount == 0) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    if (stopping_ || gameExited_) {
                        return;
                    }
                    // The accept callback can run before the runtime stage,
                    // and the host updates its game state after this event.
                    ipcReady_ = true;
                }
                StartInjection();
            });
    }

    void onConfig(mcdk::Context& context) override {
        const auto raw = context.configJson();
        const auto config = Json::parse(raw.begin(), raw.end(), nullptr, false);
        if (config.is_discarded() || (!config.is_null() && !config.is_object())) {
            throw mcdk::Error(MCDK_ERR_INVALID_ARGUMENT,
                             "Game-speed config must be a JSON object or null.");
        }
        if (config.is_object()) {
            for (const auto& item : config.items()) {
                if (item.key() != "autoInject" && item.key() != "initialSpeed" && item.key() != "showUi") {
                    throw mcdk::Error(MCDK_ERR_INVALID_ARGUMENT,
                                     "Unknown game-speed config field: " + item.key());
                }
            }
            autoInject_ = ReadBool(config, "autoInject", true);
            showUi_ = ReadBool(config, "showUi", false);
            if (const auto value = config.find("initialSpeed"); value != config.end()) {
                if (!value->is_number()) {
                    throw mcdk::Error(MCDK_ERR_INVALID_ARGUMENT, "initialSpeed must be a number.");
                }
                initialSpeed_ = value->get<double>();
                if (!ValidSpeed(initialSpeed_)) {
                    throw mcdk::Error(MCDK_ERR_INVALID_ARGUMENT,
                                     "initialSpeed must be 0 (pause) or a finite multiplier between 0.01 and 16.");
                }
            }
        }
        hookDll_ = PluginDirectory() / L"gamespeed_hook.dll";
    }

    void onRuntime(mcdk::Context& context) override {
        const auto session = context.info().session();
        const bool connected = session.gameDebugReady || session.state == mcdk::GameState::InWorld ||
                               !context.info().ipcClientPorts().empty();
        bool waiting = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (runtimeStarted_) {
                context.console().warn("Game-speed runtime stage was already handled.");
                return;
            }
            runtimeStarted_ = true;
            if (stopping_ || gameExited_) {
                return;
            }
            pid_ = session.gamePid;
            if (pid_ == 0) {
                lastError_ = "MCDevTool did not provide a game process ID at the runtime stage.";
                context.console().error(lastError_);
                return;
            }
            if (!autoInject_) {
                return;
            }
            ipcReady_ = ipcReady_ || connected;
            waiting = !ipcReady_;
        }
        if (waiting) {
            context.console().info("Game-speed is waiting for the first game debug IPC connection before injection.");
        }
        StartInjection();
    }

    void onShutdown(mcdk::Context& context) override {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stopping_ = true;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        // The worker has finished. Restore a continuous 1x clock and hide UI.
        std::uint32_t pid;
        bool injected;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            pid = pid_;
            injected = injected_;
        }
        if (pid != 0 && injected) {
            const auto result = gamespeed::SendCommand(pid, "shutdown", 3000);
            if (!result.ok) {
                RecordError(result.message);
                context.console().warn("Cannot deactivate game-speed DLL: " + result.message);
            }
        }
    }

private:
    static bool ReadBool(const Json& object, const char* key, bool fallback) {
        const auto value = object.find(key);
        if (value == object.end()) {
            return fallback;
        }
        if (!value->is_boolean()) {
            throw mcdk::Error(MCDK_ERR_INVALID_ARGUMENT, std::string(key) + " must be a boolean.");
        }
        return value->get<bool>();
    }

    void RecordError(std::string_view error) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastError_ = error;
    }

    void StartInjection() {
        // Assignment of worker_ is protected by the same mutex as stopping_,
        // so shutdown cannot inspect or join a partially published worker.
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (stopping_ || gameExited_ || !runtimeStarted_ || !autoInject_ ||
            !ipcReady_ || pid_ == 0 || attempted_) {
            return;
        }
        attempted_ = true;
        injecting_ = true;
        lastError_.clear();
        const auto pid = pid_;
        const auto generation = generation_;
        // Each MCDevTool session owns one game PID. Reconnecting clients must
        // never inject it twice, including after an unsuccessful attempt.
        try {
            worker_ = std::thread([this, pid, generation] {
                gamespeed::Result result;
                try {
                    result = PerformInjection(pid, generation);
                } catch (const std::exception& error) {
                    result = {false, error.what()};
                } catch (...) {
                    result = {false, "Unknown exception in the game-speed injection worker."};
                }
                bool report = false;
                {
                    std::lock_guard<std::mutex> workerLock(stateMutex_);
                    injecting_ = false;
                    report = !stopping_ && pid_ == pid && generation_ == generation;
                }
                if (report) {
                    if (result.ok) {
                        this->context().console().info("Game-speed DLL attached; press F8 / Ctrl+Shift+G to toggle the panel.");
                    } else {
                        this->context().console().error("Game-speed injection failed: " + result.message);
                    }
                }
            });
        } catch (...) {
            attempted_ = false;
            injecting_ = false;
            throw;
        }
        context().console().info("Game-speed IPC ready; starting DLL injection.");
    }

    bool CanContinue(std::uint32_t pid, std::uint64_t generation) const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return !stopping_ && !gameExited_ && pid_ == pid && generation_ == generation;
    }

    gamespeed::Result PerformInjection(std::uint32_t pid, std::uint64_t generation) {
        gamespeed::Result result;
        bool loaded = false;
        try {
            if (!CanContinue(pid, generation)) {
                return {false, "Game-speed injection was cancelled before it started."};
            }
            result = gamespeed::Inject(pid, hookDll_);
            loaded = result.ok;
            if (loaded && CanContinue(pid, generation)) {
                result = gamespeed::SendCommand(pid, SpeedCommand(initialSpeed_));
                if (result.ok && CanContinue(pid, generation)) {
                    result = gamespeed::SendCommand(pid, showUi_ ? "show" : "hide");
                }
            }
        } catch (const std::exception& error) {
            result = {false, error.what()};
        } catch (...) {
            result = {false, "Unknown exception while injecting the game-speed DLL."};
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (pid_ == pid && generation_ == generation) {
                injected_ = loaded;
                if (!result.ok) {
                    lastError_ = result.message;
                }
            } else {
                result = {false, "The game exited while the game-speed DLL was being injected."};
            }
            injecting_ = false;
        }
        return result;
    }

    mutable std::mutex stateMutex_;
    std::thread worker_;
    std::filesystem::path hookDll_;
    std::uint32_t pid_ = 0;
    bool autoInject_ = true;
    // The injected overlay presents its temporary shortcut hint on startup.
    // Opening the full panel immediately remains an explicit opt-in.
    bool showUi_ = false;
    double initialSpeed_ = 1.0;
    bool injecting_ = false;
    bool injected_ = false;
    bool runtimeStarted_ = false;
    bool ipcReady_ = false;
    bool attempted_ = false;
    bool gameExited_ = false;
    bool stopping_ = false;
    std::uint64_t generation_ = 0;
    std::string lastError_;
};
} // namespace

MCDK_PLUGIN(GameSpeedPlugin, "com.github-zero123.game-speed", "0.1.0");
