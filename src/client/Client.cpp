#include <gamespeed/Client.hpp>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <memory>

namespace gamespeed {
namespace {
struct CloseHandleDeleter {
    void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

std::string WinError(const char* action, DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string result = std::string(action) + " (Windows error " + std::to_string(code) + ")";
    if (buffer) {
        const int count = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
        if (count > 1) {
            std::string detail(static_cast<std::size_t>(count), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, detail.data(), count, nullptr, nullptr);
            while (!detail.empty() && (detail.back() == '\0' || detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
            result += ": " + detail;
        }
        LocalFree(buffer);
    }
    return result;
}

Result FinishIo(HANDLE pipe, OVERLAPPED& operation, BOOL immediate, DWORD initialError,
                ULONGLONG deadline, DWORD& bytes) {
    if (!immediate && initialError != ERROR_IO_PENDING) return {false, WinError("Pipe I/O failed", initialError)};
    const ULONGLONG now = GetTickCount64();
    const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    const DWORD wait = WaitForSingleObject(operation.hEvent, remaining);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        // Keep OVERLAPPED/buffers alive until the cancellation completes.
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        return {false, wait == WAIT_TIMEOUT ? "Pipe command timed out" : WinError("Pipe wait failed")};
    }
    if (!GetOverlappedResult(pipe, &operation, &bytes, FALSE)) return {false, WinError("Pipe I/O failed")};
    return {true, {}};
}

struct ModuleInfo { std::uintptr_t base = 0; std::wstring path; std::uint32_t imageSize = 0; };
ModuleInfo FindModule(DWORD pid, const std::wstring& filename, DWORD* lookupError = nullptr) {
    if (lookupError) *lookupError = ERROR_MOD_NOT_FOUND;
    Handle snapshot;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        snapshot.reset(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (snapshot.get() != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        if (lookupError) *lookupError = GetLastError();
        return {};
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.get(), &entry)) {
        do {
            if (_wcsicmp(entry.szModule, filename.c_str()) == 0) {
                if (lookupError) *lookupError = ERROR_SUCCESS;
                return {reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.szExePath, entry.modBaseSize};
            }
        } while (Module32NextW(snapshot.get(), &entry));
    }
    const DWORD lastError = GetLastError();
    if (lookupError && lastError != ERROR_NO_MORE_FILES) *lookupError = lastError;
    return {};
}

DWORD Remaining(ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}

bool StartupModuleError(DWORD error) {
    return error == ERROR_MOD_NOT_FOUND || error == ERROR_PARTIAL_COPY ||
        error == ERROR_BAD_LENGTH || error == ERROR_NO_MORE_FILES;
}

Result WaitForLoader(DWORD pid, HANDLE process, const std::wstring& filename,
                     ULONGLONG deadline, ModuleInfo& module) {
    for (;;) {
        DWORD lookupError = ERROR_SUCCESS;
        module = FindModule(pid, filename, &lookupError);
        if (module.base) return {true, {}};
        if (!StartupModuleError(lookupError))
            return {false, WinError("Cannot enumerate target loader module", lookupError)};
        const DWORD remaining = Remaining(deadline);
        const DWORD waited = WaitForSingleObject(process, std::min<DWORD>(remaining, 25));
        if (waited == WAIT_OBJECT_0) return {false, "Target exited while waiting for its loader module"};
        if (waited == WAIT_FAILED) return {false, WinError("Cannot wait for target loader readiness")};
        if (remaining == 0)
            return {false, WinError("Target loader readiness timed out (module not initialized or module enumeration incomplete)", lookupError)};
    }
}

bool IsX64(HANDLE process) {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto query = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (query) {
        USHORT machine = 0, native = 0;
        return query(process, &machine, &native) && machine == IMAGE_FILE_MACHINE_UNKNOWN && native == IMAGE_FILE_MACHINE_AMD64;
    }
    BOOL wow64 = TRUE;
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return IsWow64Process(process, &wow64) && !wow64 && info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

bool DllIsX64(const std::filesystem::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!ReadFile(file.get(), &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return false;
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) return false;
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!ReadFile(file.get(), &signature, sizeof(signature), &read, nullptr) || read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE) return false;
    return ReadFile(file.get(), &header, sizeof(header), &read, nullptr) && read == sizeof(header)
        && header.Machine == IMAGE_FILE_MACHINE_AMD64 && (header.Characteristics & IMAGE_FILE_DLL);
}
}

std::wstring PipeName(std::uint32_t pid) { return L"\\\\.\\pipe\\MCDK.GameSpeed." + std::to_wstring(pid); }

Result SendCommand(std::uint32_t pid, std::string_view command, std::uint32_t timeoutMs) {
    if (pid == 0 || command.empty() || command.size() > 512 || command.find('\n') != std::string_view::npos || command.find('\0') != std::string_view::npos)
        return {false, "Invalid PID or command (maximum 512 bytes, one line)"};
    const auto name = PipeName(pid);
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    Handle pipe;
    do {
        pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe.get() != INVALID_HANDLE_VALUE) break;
        const auto code = GetLastError();
        if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) return {false, WinError("Cannot open game control pipe", code)};
        if (GetTickCount64() >= deadline) return {false, "Game control pipe is unavailable or startup timed out"};
        Sleep(20);
    } while (true);
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &serverPid) || serverPid != pid) return {false, "Control pipe belongs to a different process"};
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return {false, WinError("Cannot set pipe message mode")};
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return {false, WinError("Cannot create pipe event")};
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD bytes = 0;
    BOOL written = WriteFile(pipe.get(), command.data(), static_cast<DWORD>(command.size()), nullptr, &operation);
    const DWORD writeError = written ? ERROR_SUCCESS : GetLastError();
    auto result = FinishIo(pipe.get(), operation, written, writeError, deadline, bytes);
    if (!result.ok) return result;
    if (bytes != command.size()) return {false, "Incomplete pipe command write"};
    ResetEvent(event.get());
    operation = {};
    operation.hEvent = event.get();
    std::array<char, 8192> response{};
    BOOL received = ReadFile(pipe.get(), response.data(), static_cast<DWORD>(response.size()), nullptr, &operation);
    const DWORD readError = received ? ERROR_SUCCESS : GetLastError();
    result = FinishIo(pipe.get(), operation, received, readError, deadline, bytes);
    if (!result.ok) return result;
    std::string message(response.data(), bytes);
    const bool ok = message.starts_with("{\"ok\":true,") || message.starts_with("{\"ok\":true}");
    return {ok, std::move(message)};
}

Result Inject(std::uint32_t pid, const std::filesystem::path& dll, std::uint32_t timeoutMs) {
    if (pid == 0 || pid == GetCurrentProcessId()) return {false, "Provide another Windows x64 process PID"};
    std::error_code error;
    const auto absolute = std::filesystem::absolute(dll, error).lexically_normal();
    if (error || !DllIsX64(absolute)) return {false, "Injection DLL is missing or is not a Windows x64 DLL"};
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!process) return {false, WinError("Cannot open target process")};
    if (!IsX64(process.get())) return {false, "Target must be a native Windows x64 process (injector and DLL architecture must match)"};
    const auto localProc = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!localProc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localProc), &owner)) return {false, WinError("Cannot resolve LoadLibraryW")};
    std::array<wchar_t, 32768> ownerPath{};
    if (!GetModuleFileNameW(owner, ownerPath.data(), static_cast<DWORD>(ownerPath.size()))) return {false, WinError("Cannot resolve loader module")};
    ModuleInfo remoteOwner;
    const auto loaderReady = WaitForLoader(pid, process.get(), std::filesystem::path(ownerPath.data()).filename().wstring(), deadline, remoteOwner);
    if (!loaderReady.ok) return loaderReady;
    const auto existing = FindModule(pid, absolute.filename().wstring());
    if (existing.base) {
        if (_wcsicmp(std::filesystem::path(existing.path).lexically_normal().c_str(), absolute.c_str()) != 0)
            return {false, "A DLL with the same filename is already loaded from another directory; restart the target before switching builds"};
        return SendCommand(pid, "status", Remaining(deadline));
    }
    const auto rva = reinterpret_cast<std::uintptr_t>(localProc) - reinterpret_cast<std::uintptr_t>(owner);
    if (rva >= remoteOwner.imageSize) return {false, "Resolved LoadLibraryW address is outside the target loader module"};
    const auto remoteProc = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteOwner.base + rva);
    const auto pathText = absolute.wstring();
    const auto size = (pathText.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process.get(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) return {false, WinError("Cannot allocate target DLL path")};
    SIZE_T bytes = 0;
    if (!WriteProcessMemory(process.get(), remotePath, pathText.c_str(), size, &bytes) || bytes != size) {
        const auto message = WinError("Cannot write target DLL path");
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return {false, message};
    }
    Handle thread(CreateRemoteThread(process.get(), nullptr, 0, remoteProc, remotePath, 0, nullptr));
    if (!thread) {
        const auto message = WinError("Cannot create DLL loading thread");
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return {false, message};
    }
    const auto wait = WaitForSingleObject(thread.get(), Remaining(deadline));
    if (wait != WAIT_OBJECT_0) {
        // Loader may still be reading this memory. Leave the small buffer until process exit.
        return {false, "DLL loading timed out; the target loader may still be running. Do not retry until status is available or the process restarts"};
    }
    VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
    // A remote thread's 32-bit exit code cannot represent an x64 HMODULE.
    if (!FindModule(pid, absolute.filename().wstring()).base) return {false, "LoadLibraryW did not load the DLL; check its dependencies and target permissions"};
    return SendCommand(pid, "status", Remaining(deadline));
}
}
