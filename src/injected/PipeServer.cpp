#include <gamespeed/Runtime.hpp>
#include "PipeServer.hpp"
#include <sddl.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>

namespace gamespeed::runtime {
namespace {
std::string Escape(const std::string& value) {
    std::string result;
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c >= 32) result += static_cast<char>(c);
    }
    return result;
}
std::string Failure(const std::string& reason) { return "{\"ok\":false,\"error\":\"" + Escape(reason) + "\"}"; }

std::string Snapshot(bool clockReady, const std::string& warning) {
    const auto state = Status();
    std::ostringstream response;
    response.imbue(std::locale::classic());
    response << std::setprecision(17) << "{\"ok\":" << (clockReady ? "true" : "false")
        << ",\"pid\":" << GetCurrentProcessId() << ",\"clockReady\":" << (clockReady ? "true" : "false")
        << ",\"speed\":" << state.speed << ",\"resumeSpeed\":" << state.resumeSpeed
        << ",\"paused\":" << (state.paused ? "true" : "false")
        << ",\"scaledCalls\":" << state.scaledCalls << ",\"virtualCounter\":" << state.virtualCounter
        << ",\"realCounter\":" << state.realCounter << ",\"frequency\":" << CounterFrequency()
        << ",\"overlayReady\":" << (OverlayReady() ? "true" : "false")
        << ",\"uiVisible\":" << (UiVisible() ? "true" : "false")
        << ",\"overlayError\":\"" << Escape(OverlayError()) << "\""
        << ",\"" << (clockReady ? "warning" : "error") << "\":\"" << Escape(warning) << "\"}";
    return response.str();
}

std::string Dispatch(std::string command, bool clockReady, const std::string& warning) {
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (command == "status") return Snapshot(clockReady, warning);
    if (!clockReady) return Failure("Clock hook is unavailable: " + warning);
    if (command.starts_with("set ")) {
        double speed = -1;
        const auto number = std::string_view(command).substr(4);
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), speed);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !SetSpeed(speed))
            return Failure("Speed must be 0 (pause) or a finite value from 0.01 to 16");
    } else if (command == "pause") {
        SetSpeed(0.0);
    } else if (command == "resume") {
        SetSpeed(Status().resumeSpeed);
    } else if (command == "reset") Reset();
    else if (command == "show") SetUiVisible(true);
    else if (command == "hide") SetUiVisible(false);
    else if (command == "shutdown") { Reset(); DeactivateOverlay(); }
    else return Failure("Unknown command; use status/set/pause/resume/reset/show/hide/shutdown");
    return Snapshot(clockReady, warning);
}

PSECURITY_DESCRIPTOR UserSecurity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    const bool queried = GetTokenInformation(token, TokenUser, storage.data(), size, &size) != FALSE;
    CloseHandle(token);
    if (!queried) return nullptr;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) return nullptr;
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    return descriptor;
}

bool Complete(HANDLE pipe, OVERLAPPED& operation, BOOL immediate, DWORD code, DWORD timeout, DWORD& bytes) {
    if (!immediate && code != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}
}

[[noreturn]] void RunPipeServer(bool clockReady, const std::string& startupError) {
    const std::wstring name = L"\\\\.\\pipe\\MCDK.GameSpeed." + std::to_wstring(GetCurrentProcessId());
    const auto descriptor = UserSecurity();
    if (!descriptor) {
        OutputDebugStringA("GameSpeed: cannot construct user-only control pipe security\n");
        // Clock remains at 1x if there is no control channel.
        Reset(); DeactivateOverlay();
        ExitThread(1);
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 8192, 1024, 3000, &security);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        OutputDebugStringA("GameSpeed: cannot create control pipe\n");
        Reset(); DeactivateOverlay();
        ExitThread(1);
    }
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) { CloseHandle(pipe); Reset(); DeactivateOverlay(); ExitThread(1); }
    for (;;) {
        ResetEvent(event);
        OVERLAPPED operation{};
        operation.hEvent = event;
        const BOOL connected = ConnectNamedPipe(pipe, &operation);
        const DWORD code = connected ? ERROR_SUCCESS : GetLastError();
        DWORD bytes = 0;
        if (code != ERROR_PIPE_CONNECTED && !Complete(pipe, operation, connected, code, INFINITE, bytes)) {
            DisconnectNamedPipe(pipe);
            continue;
        }
        ResetEvent(event);
        operation = {}; operation.hEvent = event;
        std::array<char, 513> command{};
        const BOOL read = ReadFile(pipe, command.data(), static_cast<DWORD>(command.size()), nullptr, &operation);
        const DWORD readCode = read ? ERROR_SUCCESS : GetLastError();
        if (Complete(pipe, operation, read, readCode, 3000, bytes)) {
            std::string response;
            try {
                if (bytes == 0 || bytes > 512 || std::find(command.begin(), command.begin() + bytes, '\0') != command.begin() + bytes)
                    response = Failure("Invalid command");
                else response = Dispatch(std::string(command.data(), bytes), clockReady, startupError);
            } catch (const std::exception& exception) { response = Failure(exception.what()); }
            catch (...) { response = Failure("Command failed"); }
            ResetEvent(event);
            operation = {}; operation.hEvent = event;
            const BOOL written = WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), nullptr, &operation);
            const DWORD writeCode = written ? ERROR_SUCCESS : GetLastError();
            if (Complete(pipe, operation, written, writeCode, 3000, bytes)) {
                // The client closes after reading. Waiting for that close keeps
                // DisconnectNamedPipe from discarding a queued response. Bounded
                // I/O also prevents an idle connection from locking out controls.
                ResetEvent(event);
                operation = {}; operation.hEvent = event;
                char discard = 0;
                const BOOL ack = ReadFile(pipe, &discard, 1, nullptr, &operation);
                const DWORD ackCode = ack ? ERROR_SUCCESS : GetLastError();
                Complete(pipe, operation, ack, ackCode, 1000, bytes);
            }
        }
        DisconnectNamedPipe(pipe);
    }
}
}
