#include <gamespeed/Runtime.hpp>
#include "PipeServer.hpp"
#include <MinHook.h>
#include <exception>

namespace {
DWORD WINAPI Initialize(void* parameter) {
    const auto module = static_cast<HMODULE>(parameter);
    // Keeping the DLL resident preserves both the QPC offset and detour callbacks.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Initialize), &pinned);
    std::string error;
    bool clockReady = false;
    try {
        const auto status = MH_Initialize();
        if (status != MH_OK) error = std::string("MinHook initialization: ") + MH_StatusToString(status);
        else if ((clockReady = gamespeed::runtime::InitializeClock(module, error))) {
            std::string overlayError;
            if (!gamespeed::runtime::InitializeOverlay(module, overlayError)) {
                // Timer controls remain usable when no supported renderer is available.
                OutputDebugStringA(("GameSpeed overlay: " + overlayError + "\n").c_str());
                if (!error.empty()) error += "; ";
                error += "Overlay: " + overlayError;
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "Unexpected DLL initialization failure";
    }
    if (!error.empty()) OutputDebugStringA(("GameSpeed: " + error + "\n").c_str());
    gamespeed::runtime::RunPipeServer(clockReady, error);
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Static CRT uses thread notifications; do not disable them here.
        // The worker only starts after the loader lock is released. Never wait in DllMain.
        const HANDLE worker = CreateThread(nullptr, 0, Initialize, instance, 0, nullptr);
        if (!worker) return FALSE;
        CloseHandle(worker);
    }
    return TRUE;
}
