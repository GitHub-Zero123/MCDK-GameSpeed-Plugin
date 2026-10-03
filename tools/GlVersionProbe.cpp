#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <gl/GL.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>

namespace {
using SwapFunction = BOOL(WINAPI*)(HDC);
SwapFunction original = nullptr;
std::atomic<bool> captured{false};
wchar_t outputPath[32768]{};

BOOL WINAPI ObserveSwap(HDC device) {
    if (wglGetCurrentContext() && !captured.exchange(true)) {
        const auto text = [](GLenum name) {
            const auto* value = glGetString(name);
            return value ? reinterpret_cast<const char*>(value) : "unavailable";
        };
        char output[8192]{};
        const int size = std::snprintf(output, sizeof(output),
            "GL_VERSION=%s\nGL_SHADING_LANGUAGE_VERSION=%s\nGL_VENDOR=%s\nGL_RENDERER=%s\n",
            text(GL_VERSION), text(0x8b8c), text(GL_VENDOR), text(GL_RENDERER));
        const HANDLE file = CreateFileW(outputPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, output, size > 0 && size < sizeof(output) ? static_cast<DWORD>(size) : sizeof(output) - 1, &written, nullptr);
            CloseHandle(file);
        }
    }
    return original(device);
}

DWORD WINAPI Start(void* argument) {
    const auto module = static_cast<HMODULE>(argument);
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Start), &pinned);
    wchar_t modulePath[32768]{};
    GetModuleFileNameW(module, modulePath, 32768);
    wchar_t* slash = wcsrchr(modulePath, L'\\');
    if (!slash) return 1;
    *slash = 0;
    _snwprintf_s(outputPath, _countof(outputPath), _TRUNCATE, L"%s\\opengl-%lu.log", modulePath, GetCurrentProcessId());
    const auto target = GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "SwapBuffers");
    if (!target || MH_Initialize() != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&ObserveSwap), reinterpret_cast<void**>(&original)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) return 2;
    // All calls only read context strings; the trampoline preserves rendering.
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        const HANDLE worker = CreateThread(nullptr, 0, Start, module, 0, nullptr);
        if (!worker) return FALSE;
        CloseHandle(worker);
    }
    return TRUE;
}
