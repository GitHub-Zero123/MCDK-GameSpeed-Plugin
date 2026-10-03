#include <gamespeed/Client.hpp>
#include <windows.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct CloseHandleDeleter {
    void operator()(void* handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int Child(const wchar_t* eventName) {
    Handle quit(OpenEventW(SYNCHRONIZE, FALSE, eventName));
    if (!quit) return 2;
    return WaitForSingleObject(quit.get(), 15000) == WAIT_OBJECT_0 ? 0 : 3;
}

// Only this executable's owned child is resumed or terminated. No game process,
// window, graphics context or desktop focus is involved in this regression test.
struct OwnedChild {
    Handle quit;
    Handle process;
    Handle thread;
    DWORD pid = 0;
    bool suspended = false;

    void Start() {
        const auto eventName = L"Local\\GameSpeed.LoaderTest.Quit." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        quit.reset(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
        Require(bool(quit), "Cannot create isolated child's quit event");
        std::array<wchar_t, 32768> self{};
        const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        Require(length != 0 && length < self.size(), "Cannot resolve isolated test executable");
        std::wstring command = L"\"" + std::wstring(self.data()) + L"\" --child \"" + eventName + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        const BOOL created = CreateProcessW(self.data(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
        const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
        Require(created != FALSE,
            "Cannot start suspended isolated child (Windows error " + std::to_string(createError) + ")");
        process.reset(child.hProcess);
        thread.reset(child.hThread);
        pid = child.dwProcessId;
        suspended = true;
    }

    void Resume() {
        Require(ResumeThread(thread.get()) != DWORD(-1), "Cannot resume isolated child's loader");
        suspended = false;
    }

    bool Stop() noexcept {
        if (!process) return true;
        SetEvent(quit.get());
        if (suspended) {
            ResumeThread(thread.get());
            suspended = false;
        }
        bool clean = WaitForSingleObject(process.get(), 3000) == WAIT_OBJECT_0;
        if (!clean) {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), 1000);
        }
        DWORD code = 0;
        clean = clean && GetExitCodeProcess(process.get(), &code) && code == 0;
        thread.reset();
        process.reset();
        return clean;
    }

    ~OwnedChild() { Stop(); }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--child") return Child(argv[2]);
    if (argc != 2) {
        std::cerr << "Provide the injected x64 DLL path\n";
        return 2;
    }

    const std::filesystem::path dll = argv[1];
    OwnedChild child;
    std::thread injector;
    gamespeed::Result attached;
    std::atomic<bool> finished{false};
    int exitCode = 0;
    try {
        child.Start();
        // CREATE_SUSPENDED holds the primary thread before user-mode loader
        // initialization, reproducing a process-start event arriving too early.
        const ULONGLONG started = GetTickCount64();
        const auto early = gamespeed::Inject(child.pid, dll, 150);
        const ULONGLONG elapsed = GetTickCount64() - started;
        Require(!early.ok && early.message.find("Target loader readiness timed out") != std::string::npos,
            "Suspended loader did not produce the expected readiness timeout: " + early.message);
        Require(elapsed >= 125 && elapsed < 2000,
            "Loader readiness timeout ignored its deadline: " + std::to_string(elapsed) + "ms");
        Require(WaitForSingleObject(child.process.get(), 0) == WAIT_TIMEOUT,
            "Loader readiness timeout terminated the isolated child");

        injector = std::thread([&] {
            try { attached = gamespeed::Inject(child.pid, dll, 5000); }
            catch (const std::exception& error) { attached = {false, error.what()}; }
            catch (...) { attached = {false, "Unexpected injection exception"}; }
            finished.store(true, std::memory_order_release);
        });
        Sleep(100);
        Require(!finished.load(std::memory_order_acquire),
            "Injection failed immediately while the loader was still suspended");
        child.Resume();
        const ULONGLONG completionDeadline = GetTickCount64() + 6000;
        while (!finished.load(std::memory_order_acquire) && GetTickCount64() < completionDeadline) Sleep(10);
        Require(finished.load(std::memory_order_acquire), "Injection exceeded its bounded readiness deadline");
        injector.join();
        Require(attached.ok && attached.message.find("\"clockReady\":true") != std::string::npos,
            "Injection did not recover when the child loader initialized: " + attached.message);

        const auto duplicate = gamespeed::Inject(child.pid, dll, 2000);
        Require(duplicate.ok && duplicate.message.find("\"clockReady\":true") != std::string::npos,
            "Duplicate injection did not reuse the resident DLL and control pipe: " + duplicate.message);
        // This child verifies loader readiness, and deliberately has no game
        // timer adapter. An unrecognized build must never fall back to freezing
        // every system timer merely because its loader is now available.
        Require(duplicate.message.find("\"nativeTickReady\":false") != std::string::npos,
            "The generic loader child was incorrectly recognized as a supported game");
        const auto unsupportedPause = gamespeed::SendCommand(child.pid, "pause", 1000);
        Require(!unsupportedPause.ok && unsupportedPause.message.find("only 1x is supported") != std::string::npos,
            "An unrecognized timer profile did not reject pause: " + unsupportedPause.message);
        const auto unsupportedSpeed = gamespeed::SendCommand(child.pid, "set 2", 1000);
        Require(!unsupportedSpeed.ok, "An unrecognized timer profile accepted game speed control");
        const auto normalSpeed = gamespeed::SendCommand(child.pid, "set 1", 1000);
        Require(normalSpeed.ok && normalSpeed.message.find("\"speed\":1,") != std::string::npos &&
            normalSpeed.message.find("\"paused\":false") != std::string::npos,
            "Unknown-profile rejection did not leave a responsive 1x clock: " + normalSpeed.message);
        Require(gamespeed::SendCommand(child.pid, "reset", 1000).ok,
            "Cannot reset the isolated child's timer");
        std::cout << "Suspended-loader timeout, delayed initialization, resident DLL reuse and safe unknown-profile rejection passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        exitCode = 1;
    }
    // On every failure, stop our child before joining the injection attempt;
    // target exit wakes its loader wait, and the injection deadline bounds I/O.
    if (!child.Stop()) {
        std::cerr << "Isolated loader test child did not exit cleanly\n";
        exitCode = 1;
    }
    if (injector.joinable()) injector.join();
    return exitCode;
}
