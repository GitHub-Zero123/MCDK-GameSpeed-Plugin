#include <gamespeed/Runtime.hpp>
#include <gamespeed/CursorVisibility.hpp>

#include <MinHook.h>
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi_Platform_Win32.h>
#include <RmlUi_Renderer_GL3.h>
#include <RmlUi_Include_GL3.h>

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gamespeed::runtime {
namespace {
constexpr UINT_PTR kSubclassId = 0x4d434753;
constexpr std::size_t kInputCapacity = 512;
using SwapFunction = BOOL(WINAPI*)(HDC);
using ClipFunction = BOOL(WINAPI*)(const RECT*);
using PositionFunction = BOOL(WINAPI*)(int, int);
using CursorFunction = HCURSOR(WINAPI*)(HCURSOR);
using ShowCursorFunction = int(WINAPI*)(BOOL);
using CaptureFunction = HWND(WINAPI*)(HWND);
SwapFunction g_swap = nullptr;
SwapFunction g_wglSwap = nullptr;
ClipFunction g_clip = nullptr;
PositionFunction g_position = nullptr;
CursorFunction g_cursor = nullptr;
ShowCursorFunction g_showCursor = nullptr;
CaptureFunction g_capture = nullptr;
HMODULE g_module = nullptr;
std::filesystem::path g_assetDirectory;
std::atomic<bool> g_visible{false};
std::atomic<bool> g_hintVisible{false};
std::atomic<bool> g_hintDismissRequested{false};
std::atomic<bool> g_ready{false};
std::atomic<HWND> g_window{nullptr};
std::atomic<HWND> g_subclassWindow{nullptr};
std::atomic<DWORD> g_renderThread{0};
// Foreground ownership can change after a focus notification is dispatched.
// Keep a separate gate so WM_KILLFOCUS cannot leave the UI capturing input
// until the next posted cursor update happens to run.
std::atomic<bool> g_windowInputInactive{false};
UINT g_installMessage = 0;
UINT g_cursorMessage = 0;
std::mutex g_bootstrapMutex;
HHOOK g_bootstrapHook = nullptr;
HWND g_bootstrapWindow = nullptr;
std::mutex g_clipMutex;
RECT g_requestedClip{};
bool g_hasRequestedClip = false;
bool g_requestedClipEmpty = true;
// These belong to the window's thread, never the worker or render thread.
CursorVisibility g_cursorVisibility;
bool g_cursorCaptured = false;
bool g_toggleChordHeld = false;
UINT g_uiMouseButtons = 0;
UINT g_activationMouseButtons = 0;
bool g_uiOwnsMouseCapture = false;
bool g_releasingUiMouseCapture = false;
std::atomic<HCURSOR> g_uiCursor{nullptr};
std::mutex g_errorMutex;
std::string g_overlayError;

struct InputTraceEntry {
    ULONGLONG time;
    DWORD thread;
    const char* event;
    std::uintptr_t value;
    std::intptr_t result;
    std::uintptr_t caller;
    HWND foreground;
    bool visible;
    bool inactive;
};
std::atomic<bool> g_inputTraceEnabled{false};
std::mutex g_inputTraceMutex;
std::array<InputTraceEntry, 1024> g_inputTrace;
std::size_t g_inputTraceNext = 0, g_inputTraceCount = 0;

void TraceInput(const char* event, std::uintptr_t value = 0, std::intptr_t result = 0, std::uintptr_t caller = 0) {
    if (!g_inputTraceEnabled.load(std::memory_order_relaxed)) return;
    std::unique_lock lock(g_inputTraceMutex, std::try_to_lock);
    if (!lock || !g_inputTraceEnabled.load(std::memory_order_relaxed)) return;
    g_inputTrace[g_inputTraceNext] = {GetTickCount64(), GetCurrentThreadId(), event, value, result, caller,
        GetForegroundWindow(), g_visible.load(), g_windowInputInactive.load()};
    g_inputTraceNext = (g_inputTraceNext + 1) % g_inputTrace.size();
    g_inputTraceCount = std::min(g_inputTraceCount + 1, g_inputTrace.size());
}

void Log(const std::string& message) {
    const std::string line = "[MCDK GameSpeed UI] " + message + "\n";
    OutputDebugStringA(line.c_str());
}

void ReportError(const std::string& message) {
    {
        std::lock_guard lock(g_errorMutex);
        g_overlayError = message;
    }
    Log(message);
}

bool PanelOwnsWindowInput() {
    return g_window.load(std::memory_order_acquire) && g_ready.load(std::memory_order_acquire) &&
        g_visible.load(std::memory_order_acquire);
}

bool GameIsForeground() {
    const HWND window = g_window.load(std::memory_order_acquire);
    return window && GetForegroundWindow() == GetAncestor(window, GA_ROOT);
}

bool CapturesInput() {
    // Global cursor APIs need foreground ownership. Window-message ownership
    // instead lasts for the whole modal panel, including activation transitions.
    return PanelOwnsWindowInput() && !g_windowInputInactive.load(std::memory_order_acquire) && GameIsForeground();
}

UINT MouseButtonMask(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 2;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 4;
    default: return 0;
    }
}

void RequestCursorUpdate() {
    if (const HWND window = g_subclassWindow.load(std::memory_order_acquire))
        PostMessageW(window, g_cursorMessage, 0, 0);
}

void ReleaseUiMouseCapture(HWND window) {
    g_uiMouseButtons = 0;
    const bool release = g_uiOwnsMouseCapture && window && GetCapture() == window;
    g_uiOwnsMouseCapture = false;
    if (release) {
        // The normal mouse-up is already queued. Its expected capture-change
        // notification must not discard the gesture's final move/up events.
        g_releasingUiMouseCapture = true;
        ReleaseCapture();
        g_releasingUiMouseCapture = false;
    }
}

void EnsureCursorVisibleOnWindowThread() {
    g_cursorVisibility.Acquire(g_showCursor);
}

void ApplyUiCursorOnWindowThread() {
    const HCURSOR cursor = g_uiCursor.load(std::memory_order_relaxed);
    g_cursor(cursor ? cursor : LoadCursorW(nullptr, IDC_ARROW));
}

void UpdateCursorOnWindowThread(bool refreshVisibility = false) {
    if (!PanelOwnsWindowInput()) g_activationMouseButtons = 0;
    const bool capture = CapturesInput();
    if (!capture)
        ReleaseUiMouseCapture(g_subclassWindow.load(std::memory_order_acquire));
    if (capture == g_cursorCaptured) {
        if (capture && refreshVisibility) {
            // A game's WM_SETFOCUS handler may hide the pointer again even
            // though the panel was already open. Repair on this event only.
            EnsureCursorVisibleOnWindowThread();
            g_clip(nullptr);
            ApplyUiCursorOnWindowThread();
        }
        return;
    }
    g_cursorCaptured = capture;
    if (capture) {
        RECT current{};
        if (GetClipCursor(&current)) {
            const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            std::lock_guard lock(g_clipMutex);
            g_requestedClip = current;
            g_requestedClipEmpty = EqualRect(&current, &desktop) != FALSE;
            g_hasRequestedClip = true;
        }
        g_clip(nullptr);
        EnsureCursorVisibleOnWindowThread();
        ApplyUiCursorOnWindowThread();
        const HWND inputWindow = g_subclassWindow.load(std::memory_order_acquire);
        // A key released while the panel is open is consumed by the UI. Release
        // keys already held by the game at capture start so GLFW cannot keep W,
        // Shift, or a mouse button stuck after the panel closes. This helper is
        // only called inside our subclass on the HWND owner's thread.
        for (int key = VK_BACK; key <= 0xff; ++key) {
            if (key == VK_F8 || key == VK_INSERT || (key == 'G' && g_toggleChordHeld) ||
                key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || !(GetKeyState(key) & 0x8000))
                continue;
            const UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX);
            const LPARAM extended = (scan & 0xff00) == 0xe000 ? (LPARAM(1) << 24) : 0;
            const LPARAM released = LPARAM(1) | (LPARAM(scan & 0xff) << 16) | extended | (LPARAM(1) << 30) | (LPARAM(1) << 31);
            const int originalKey = (key == VK_LSHIFT || key == VK_RSHIFT) ? VK_SHIFT :
                (key == VK_LCONTROL || key == VK_RCONTROL) ? VK_CONTROL : (key == VK_LMENU || key == VK_RMENU) ? VK_MENU : key;
            DefSubclassProc(inputWindow, WM_KEYUP, static_cast<WPARAM>(originalKey), released);
        }
        POINT position{};
        GetCursorPos(&position);
        ScreenToClient(inputWindow, &position);
        const LPARAM mousePosition = MAKELPARAM(position.x, position.y);
        if (GetKeyState(VK_LBUTTON) & 0x8000) DefSubclassProc(inputWindow, WM_LBUTTONUP, 0, mousePosition);
        if (GetKeyState(VK_RBUTTON) & 0x8000) DefSubclassProc(inputWindow, WM_RBUTTONUP, 0, mousePosition);
        if (GetKeyState(VK_MBUTTON) & 0x8000) DefSubclassProc(inputWindow, WM_MBUTTONUP, 0, mousePosition);
        ReleaseCapture();
    } else {
        g_cursorVisibility.Release(g_showCursor);
        std::lock_guard lock(g_clipMutex);
        // Never re-clip the desktop while another application has focus.
        if (g_hasRequestedClip && !g_windowInputInactive.load(std::memory_order_acquire) &&
            GetForegroundWindow() == GetAncestor(g_window.load(), GA_ROOT))
            g_clip(g_requestedClipEmpty ? nullptr : &g_requestedClip);
    }
}

BOOL WINAPI ClipCursorHook(const RECT* rect) {
    TraceInput("clip", reinterpret_cast<std::uintptr_t>(rect), CapturesInput(), reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    if (CapturesInput()) {
        std::lock_guard lock(g_clipMutex);
        if (rect)
            g_requestedClip = *rect;
        g_requestedClipEmpty = rect == nullptr;
        g_hasRequestedClip = true;
        return g_clip(nullptr);
    }
    return g_clip(rect);
}

BOOL WINAPI SetCursorPositionHook(int x, int y) {
    TraceInput("warp", static_cast<std::uintptr_t>(static_cast<unsigned int>(x)), y, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    // GLFW's disabled cursor mode continually warps the cursor to the centre.
    return CapturesInput() ? TRUE : g_position(x, y);
}

HCURSOR WINAPI SetCursorHook(HCURSOR cursor) {
    TraceInput("cursor", reinterpret_cast<std::uintptr_t>(cursor), CapturesInput(), reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    if (CapturesInput()) {
        // Cursor mode can be re-applied in a later gameplay frame, after the
        // focus messages have already returned. Keep its request from hiding
        // or replacing the UI pointer for the whole input-capture interval.
        const HCURSOR uiCursor = g_uiCursor.load(std::memory_order_relaxed);
        cursor = uiCursor ? uiCursor : LoadCursorW(nullptr, IDC_ARROW);
    }
    // Preserve Win32's return value: the previous installed cursor handle.
    return g_cursor(cursor);
}

int WINAPI ShowCursorHook(BOOL show) {
    // The game can enter hidden/relative mode in a later frame, well after
    // WM_SETFOCUS and WM_MOUSEACTIVATE. Return its logical display count so
    // while (ShowCursor(FALSE) >= 0) terminates, while keeping ours visible.
    const HWND window = g_subclassWindow.load(std::memory_order_acquire);
    const bool owner = window && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId();
    const auto result = owner && CapturesInput() ?
        g_cursorVisibility.Change(show != FALSE, g_showCursor) : g_showCursor(show);
    TraceInput("show", show, result, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    return result;
}

HWND WINAPI ObserveCapture(HWND window) {
    const auto result = g_capture(window);
    TraceInput("capture", reinterpret_cast<std::uintptr_t>(window), reinterpret_cast<std::intptr_t>(result),
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    return result;
}

struct InputEvent {
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    int modifiers = 0;
};
std::mutex g_inputMutex;
std::array<InputEvent, kInputCapacity> g_input{};
std::size_t g_inputBegin = 0;
std::size_t g_inputCount = 0;
bool g_resetInput = false;

void QueueInput(UINT message, WPARAM wParam, LPARAM lParam) {
    const InputEvent input{message, wParam, lParam, RmlWin32::GetKeyModifierState()};
    std::lock_guard lock(g_inputMutex);
    if (message == WM_MOUSEMOVE && g_inputCount != 0) {
        const std::size_t previous = (g_inputBegin + g_inputCount - 1) % kInputCapacity;
        if (g_input[previous].message == WM_MOUSEMOVE) {
            g_input[previous] = input;
            return;
        }
    }
    if (g_inputCount == kInputCapacity) {
        // Drop the batch and release all held UI keys next frame. Missing a mouse-up
        // must never leave a slider or text input permanently dragging.
        g_inputBegin = g_inputCount = 0;
        g_resetInput = true;
    }
    g_input[(g_inputBegin + g_inputCount++) % kInputCapacity] = input;
}

void ResetQueuedInput() {
    std::lock_guard lock(g_inputMutex);
    g_inputBegin = g_inputCount = 0;
    g_resetInput = true;
}

LRESULT CALLBACK WindowSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEACTIVATE || message == WM_ACTIVATE || message == WM_ACTIVATEAPP ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_CAPTURECHANGED ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
        TraceInput("message", message, static_cast<std::intptr_t>(wParam));
    if (message == g_cursorMessage) {
        UpdateCursorOnWindowThread();
        if (CapturesInput()) ApplyUiCursorOnWindowThread();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        g_visible.store(false, std::memory_order_release);
        g_hintDismissRequested.store(true, std::memory_order_release);
        g_hintVisible.store(false, std::memory_order_release);
        UpdateCursorOnWindowThread();
        RemoveWindowSubclass(window, WindowSubclass, kSubclassId);
        g_subclassWindow.store(nullptr, std::memory_order_release);
        g_window.store(nullptr, std::memory_order_release);
        ResetQueuedInput();
        return DefSubclassProc(window, message, wParam, lParam);
    }
    if (message == WM_MOUSEACTIVATE && PanelOwnsWindowInput() && LOWORD(lParam) == HTCLIENT) {
        // This arrives before Windows changes foreground/focus. Let Windows
        // activate the window, but never run the game's click-to-grab-HUD
        // handler or deliver this activation gesture to a UI control.
        g_activationMouseButtons |= MouseButtonMask(HIWORD(lParam));
        PostMessageW(window, g_cursorMessage, 0, 0);
        return MA_ACTIVATEANDEAT;
    }
    if (message == WM_ACTIVATEAPP || message == WM_ACTIVATE || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        const bool gainingFocus = message == WM_SETFOCUS ||
            (message == WM_ACTIVATEAPP && wParam != FALSE) ||
            (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE);
        g_windowInputInactive.store(!gainingFocus, std::memory_order_release);
        g_toggleChordHeld = false;
        if (!gainingFocus) {
            g_activationMouseButtons = 0;
            ReleaseUiMouseCapture(window);
        }
        ResetQueuedInput();
        // Balance our ShowCursor changes before the game handles focus loss.
        // On focus gain, start interception first so its ClipCursor/warp calls
        // are remembered rather than locking the pointer back into gameplay.
        UpdateCursorOnWindowThread();
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        // GLFW re-applies its disabled cursor in WM_SETFOCUS. Override that
        // after the original handler has finished, on this same owner thread.
        if (gainingFocus)
            UpdateCursorOnWindowThread(true);
        PostMessageW(window, g_cursorMessage, 0, 0);
        return result;
    }
    if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lParam) != window) {
        g_uiMouseButtons = 0;
        g_uiOwnsMouseCapture = false;
        if (!g_releasingUiMouseCapture)
            ResetQueuedInput();
        // Capture changes caused by the UI's own mouse gestures must not
        // trigger the game's attempt to return to relative/HUD mouse mode.
        if (PanelOwnsWindowInput()) return 0;
    }
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    const bool chord = wParam == 'G' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000);
    if ((keyDown || keyUp) && (wParam == VK_F8 || wParam == VK_INSERT || chord || (keyUp && wParam == 'G' && g_toggleChordHeld))) {
        if (wParam == 'G') g_toggleChordHeld = keyDown;
        if (keyDown && !(lParam & (LPARAM(1) << 30)))
            SetUiVisible(!UiVisible());
        return 0;
    }
    if (keyDown && wParam == VK_ESCAPE && UiVisible() && OverlayReady()) {
        SetUiVisible(false);
        return 0;
    }
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wParam == VK_F4)
        return DefSubclassProc(window, message, wParam, lParam);
    if (PanelOwnsWindowInput()) {
        switch (message) {
        case WM_SETCURSOR:
            if (GameIsForeground()) {
                g_windowInputInactive.store(false, std::memory_order_release);
                UpdateCursorOnWindowThread();
            }
            ApplyUiCursorOnWindowThread();
            return TRUE;
        case WM_INPUT:
            // DefWindowProc performs the documented foreground raw-input cleanup.
            return DefWindowProcW(window, message, wParam, lParam);
        case WM_MOUSEMOVE: case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_UNICHAR: {
            if (message == WM_UNICHAR && wParam == UNICODE_NOCHAR)
                return TRUE;
            // Do not hand a queued click/raw input back to gameplay merely
            // because focus notifications and foreground ownership disagree.
            const UINT activationButton = MouseButtonMask(message);
            if (activationButton && (g_activationMouseButtons & activationButton)) {
                if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP)
                    g_activationMouseButtons &= ~activationButton;
                return 0;
            }
            if (!GameIsForeground()) return 0;
            g_windowInputInactive.store(false, std::memory_order_release);
            UpdateCursorOnWindowThread();
            UINT mouseButton = 0;
            bool mouseDown = false;
            switch (message) {
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: mouseButton = 1; mouseDown = true; break;
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: mouseButton = 2; mouseDown = true; break;
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: mouseButton = 4; mouseDown = true; break;
            case WM_LBUTTONUP: mouseButton = 1; break;
            case WM_RBUTTONUP: mouseButton = 2; break;
            case WM_MBUTTONUP: mouseButton = 4; break;
            }
            if (mouseButton) {
                if (mouseDown) {
                    g_uiMouseButtons |= mouseButton;
                    SetCapture(window);
                    g_uiOwnsMouseCapture = GetCapture() == window;
                } else {
                    g_uiMouseButtons &= ~mouseButton;
                }
            }
            if (message == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
            }
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(window, &point);
                lParam = MAKELPARAM(point.x, point.y);
            }
            if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) && window != g_window.load()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                MapWindowPoints(window, g_window.load(), &point, 1);
                lParam = MAKELPARAM(point.x, point.y);
            }
            QueueInput(message, wParam, lParam);
            if (mouseButton && !mouseDown && !g_uiMouseButtons)
                ReleaseUiMouseCapture(window);
            return 0;
        }
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            return TRUE;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK BootstrapWindowHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) {
        const auto& call = *reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (call.message == g_installMessage) {
            std::lock_guard lock(g_bootstrapMutex);
            if (call.hwnd == g_bootstrapWindow && SetWindowSubclass(call.hwnd, WindowSubclass, kSubclassId, 0)) {
                g_subclassWindow.store(call.hwnd, std::memory_order_release);
                PostMessageW(call.hwnd, g_cursorMessage, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void EnsureWindowSubclass(HWND window) {
    std::lock_guard lock(g_bootstrapMutex);
    if (g_subclassWindow.load(std::memory_order_acquire) == window) {
        if (g_bootstrapHook) {
            UnhookWindowsHookEx(g_bootstrapHook);
            g_bootstrapHook = nullptr;
            g_bootstrapWindow = nullptr;
        }
        return;
    }
    DWORD process = 0;
    const DWORD windowThread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId())
        return;
    if (g_bootstrapHook && g_bootstrapWindow != window) {
        UnhookWindowsHookEx(g_bootstrapHook);
        g_bootstrapHook = nullptr;
        g_bootstrapWindow = nullptr;
    }
    if (windowThread == GetCurrentThreadId()) {
        if (SetWindowSubclass(window, WindowSubclass, kSubclassId, 0)) {
            g_subclassWindow.store(window, std::memory_order_release);
            PostMessageW(window, g_cursorMessage, 0, 0);
        }
    } else if (!g_bootstrapHook) {
        // SetWindowSubclass cannot be used across threads. This bootstrap runs
        // inside the HWND's owner thread; it performs no RmlUi operations.
        g_bootstrapWindow = window;
        g_bootstrapHook = SetWindowsHookExW(WH_CALLWNDPROC, BootstrapWindowHook, g_module, windowThread);
        if (g_bootstrapHook) {
            // WH_CALLWNDPROC observes sent messages, not messages retrieved
            // from the posted queue. Cross-thread SendNotifyMessage schedules
            // this without blocking while g_bootstrapMutex is held.
            if (!SendNotifyMessageW(window, g_installMessage, 0, 0)) {
                UnhookWindowsHookEx(g_bootstrapHook);
                g_bootstrapHook = nullptr;
                g_bootstrapWindow = nullptr;
                ReportError("Cannot notify the window input thread (Win32 error " + std::to_string(GetLastError()) + ").");
            }
        }
        else
            ReportError("Unable to attach window input hook (Win32 error " + std::to_string(GetLastError()) + ").");
    }
}

template <typename T>
T GlProcedure(const char* name) {
    PROC address = wglGetProcAddress(name);
    if (!address || address == reinterpret_cast<PROC>(1) || address == reinterpret_cast<PROC>(2) ||
        address == reinterpret_cast<PROC>(3) || address == reinterpret_cast<PROC>(-1))
        address = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
    return reinterpret_cast<T>(address);
}

struct GlFunctions {
    using BlendEquationIndexed = void(APIENTRY*)(GLuint, GLenum, GLenum);
    using BlendFunctionIndexed = void(APIENTRY*)(GLuint, GLenum, GLenum, GLenum, GLenum);
    using ClipControl = void(APIENTRY*)(GLenum, GLenum);
    BlendEquationIndexed blendEquationIndexed = nullptr;
    BlendFunctionIndexed blendFunctionIndexed = nullptr;
    ClipControl clipControl = nullptr;
    PFNGLBINDSAMPLERPROC bindSampler = nullptr;
    int major = 0;
    int minor = 0;
    int drawBuffers = 1;
    int clipDistances = 0;
    int viewports = 1;
    bool compatibility = false;
    bool primitiveRestartFixed = false;
    std::string driverVersion;

    bool AtLeast(int requestedMajor, int requestedMinor) const {
        return major > requestedMajor || (major == requestedMajor && minor >= requestedMinor);
    }

    bool Initialize() {
        // GLAD must be initialized here, with the game's current context, never
        // in DllMain or the worker. Its pointers may differ between contexts.
        Rml::String message;
        if (!RmlGL3::Initialize(&message)) {
            ReportError("OpenGL loader failed: " + message);
            return false;
        }
        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const auto* shaderVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        driverVersion = std::string("GL_VERSION=") + (version ? version : "<unavailable>") +
            "; GLSL=" + (shaderVersion ? shaderVersion : "<unavailable>");
        if (!version || std::sscanf(version, "%d.%d", &major, &minor) != 2 || !AtLeast(3, 2)) {
            ReportError("The overlay requires desktop OpenGL 3.2+ (core or compatibility profile). " + driverVersion);
            return false;
        }
        std::string extensions;
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint index = 0; index < count; ++index) {
            if (const auto* extension = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index))) {
                extensions += reinterpret_cast<const char*>(extension);
                extensions += ' ';
            }
        }
        const auto has = [&extensions](const char* extension) {
            return (" " + extensions).find(" " + std::string(extension) + " ") != std::string::npos;
        };
        GLint profile = 0;
        glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
        compatibility = (profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0;
        primitiveRestartFixed = AtLeast(4, 3) || has("GL_ARB_ES3_compatibility");
        // The client's Intel driver exposes a 3.2 context. Sampler objects are
        // optional there; the bundled GLAD loader only loads core entry points,
        // so resolve this explicitly when ARB_sampler_objects is available.
        if (AtLeast(3, 3) || has("GL_ARB_sampler_objects")) {
            bindSampler = GlProcedure<PFNGLBINDSAMPLERPROC>("glBindSampler");
            if (!bindSampler) {
                ReportError("Advertised sampler object entry point is missing. " + driverVersion);
                return false;
            }
        }
        if (AtLeast(4, 0) || has("GL_ARB_draw_buffers_blend")) {
            blendEquationIndexed = GlProcedure<BlendEquationIndexed>("glBlendEquationSeparatei");
            blendFunctionIndexed = GlProcedure<BlendFunctionIndexed>("glBlendFuncSeparatei");
            if (!blendEquationIndexed)
                blendEquationIndexed = GlProcedure<BlendEquationIndexed>("glBlendEquationSeparateiARB");
            if (!blendFunctionIndexed)
                blendFunctionIndexed = GlProcedure<BlendFunctionIndexed>("glBlendFuncSeparateiARB");
        }
        if (AtLeast(4, 1) || has("GL_ARB_viewport_array"))
            glGetIntegerv(0x825b /* GL_MAX_VIEWPORTS */, &viewports);
        if (AtLeast(4, 5) || has("GL_ARB_clip_control"))
            clipControl = GlProcedure<ClipControl>("glClipControl");
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &drawBuffers);
        glGetIntegerv(GL_MAX_CLIP_DISTANCES, &clipDistances);
        drawBuffers = std::clamp(drawBuffers, 1, 32);
        clipDistances = std::clamp(clipDistances, 0, 32);
        viewports = std::clamp(viewports, 1, 32);
        if (!glBindVertexArray || !glBindFramebuffer || !glBindBuffer || !glUseProgram) {
            ReportError("Required OpenGL 3.2 entry points are missing. " + driverVersion);
            return false;
        }
        Log(std::string("Rendering with desktop OpenGL ") + version + (compatibility ? " (compatibility)." : " (core)."));
        return true;
    }
};

GlFunctions g_gl;

// The official GL3 backend backs up a subset of state. Injection also has to
// preserve object bindings and state inherited from an arbitrary game frame,
// including first-frame shader/font/FBO creation. No attribute stacks or fixed
// function matrix calls are valid in the client's core profile.
class GlStateGuard {
public:
    explicit GlStateGuard(GlFunctions& functions) : gl(functions) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBuffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        for (int index = 0; index < gl.drawBuffers; ++index) {
            glGetIntegerv(GL_DRAW_BUFFER0 + index, &drawBuffer[index]);
            blend[index].enabled = glIsEnabledi(GL_BLEND, index);
            glGetBooleani_v(GL_COLOR_WRITEMASK, index, blend[index].colorMask.data());
            if (gl.blendEquationIndexed && gl.blendFunctionIndexed) {
                glGetIntegeri_v(GL_BLEND_EQUATION_RGB, index, &blend[index].equationRgb);
                glGetIntegeri_v(GL_BLEND_EQUATION_ALPHA, index, &blend[index].equationAlpha);
                glGetIntegeri_v(GL_BLEND_SRC_RGB, index, &blend[index].sourceRgb);
                glGetIntegeri_v(GL_BLEND_DST_RGB, index, &blend[index].destinationRgb);
                glGetIntegeri_v(GL_BLEND_SRC_ALPHA, index, &blend[index].sourceAlpha);
                glGetIntegeri_v(GL_BLEND_DST_ALPHA, index, &blend[index].destinationAlpha);
            }
        }
        for (int index = 0; index < 2; ++index) {
            glActiveTexture(GL_TEXTURE0 + index);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[index]);
            if (gl.bindSampler) {
                glGetIntegerv(GL_SAMPLER_BINDING, &samplers[index]);
                gl.bindSampler(index, 0);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
        for (int index = 0; index < gl.viewports; ++index)
            scissorEnabled[index] = gl.viewports > 1 ? glIsEnabledi(GL_SCISSOR_TEST, index) : glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetIntegerv(GL_POLYGON_MODE, polygonMode.data());
        glGetFloatv(GL_BLEND_COLOR, blendColor.data());
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data());
        glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clearStencil);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSourceRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDestinationRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSourceAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDestinationAlpha);
        SaveStencil(stencilFront, false);
        SaveStencil(stencilBack, true);
        for (std::size_t index = 0; index < unpackNames.size(); ++index)
            glGetIntegerv(unpackNames[index], &unpack[index]);
        for (const GLenum capability : {GL_CULL_FACE, GL_BLEND, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST,
                 GL_COLOR_LOGIC_OP, GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_ALPHA_TO_ONE,
                 GL_SAMPLE_COVERAGE, GL_SAMPLE_MASK, GL_RASTERIZER_DISCARD, GL_PRIMITIVE_RESTART, GL_FRAMEBUFFER_SRGB})
            Disable(capability);
        if (gl.primitiveRestartFixed)
            Disable(0x8d69 /* GL_PRIMITIVE_RESTART_FIXED_INDEX */);
        for (int index = 0; index < gl.clipDistances; ++index)
            Disable(GL_CLIP_DISTANCE0 + index);
        // Legacy alpha test exists only in a compatibility profile and can
        // discard programmable fragment output there.
        if (gl.compatibility)
            Disable(0x0bc0 /* GL_ALPHA_TEST */);
        if (gl.clipControl) {
            glGetIntegerv(0x935c /* GL_CLIP_ORIGIN */, &clipOrigin);
            glGetIntegerv(0x935d /* GL_CLIP_DEPTH_MODE */, &clipDepthMode);
            gl.clipControl(0x8ca1 /* GL_LOWER_LEFT */, 0x935e /* GL_NEGATIVE_ONE_TO_ONE */);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGetIntegerv(GL_DRAW_BUFFER, &defaultDrawBuffer);
        glGetIntegerv(GL_READ_BUFFER, &defaultReadBuffer);
        GLboolean doubleBuffered = GL_FALSE;
        glGetBooleanv(GL_DOUBLEBUFFER, &doubleBuffered);
        glDrawBuffer(doubleBuffered ? GL_BACK : GL_FRONT);
        glReadBuffer(doubleBuffered ? GL_BACK : GL_FRONT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        for (const GLenum name : unpackNames)
            glPixelStorei(name, name == GL_UNPACK_ALIGNMENT ? 1 : 0);
    }

    ~GlStateGuard() {
        // Restore framebuffer-specific draw/read selections on the correct
        // framebuffer, including the game's multiple render target selection.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(defaultDrawBuffer);
        glReadBuffer(defaultReadBuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        if (drawFramebuffer) {
            std::array<GLenum, 32> buffers{};
            for (int index = 0; index < gl.drawBuffers; ++index)
                buffers[index] = static_cast<GLenum>(drawBuffer[index]);
            glDrawBuffers(gl.drawBuffers, buffers.data());
        } else {
            glDrawBuffer(drawBuffer[0]);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glReadBuffer(readBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glUseProgram(program);
        glBindVertexArray(vertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
        // In a core context VAO 0 cannot have its element binding modified.
        if (vertexArray || gl.compatibility)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        for (int index = 0; index < 2; ++index) {
            glActiveTexture(GL_TEXTURE0 + index);
            glBindTexture(GL_TEXTURE_2D, textures[index]);
            if (gl.bindSampler)
                gl.bindSampler(index, samplers[index]);
        }
        glActiveTexture(activeTexture);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        glDepthMask(depthMask);
        if (gl.compatibility) {
            glPolygonMode(GL_FRONT, polygonMode[0]);
            glPolygonMode(GL_BACK, polygonMode[1]);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, polygonMode[0]);
        }
        glBlendColor(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glClearStencil(clearStencil);
        glBlendEquationSeparate(blendEquationRgb, blendEquationAlpha);
        glBlendFuncSeparate(blendSourceRgb, blendDestinationRgb, blendSourceAlpha, blendDestinationAlpha);
        RestoreStencil(stencilFront, GL_FRONT);
        RestoreStencil(stencilBack, GL_BACK);
        for (std::size_t index = 0; index < unpackNames.size(); ++index)
            glPixelStorei(unpackNames[index], unpack[index]);
        if (gl.clipControl)
            gl.clipControl(clipOrigin, clipDepthMode);
        for (std::size_t index = 0; index < enableCount; ++index) {
            if (enable[index].enabled) glEnable(enable[index].capability);
            else glDisable(enable[index].capability);
        }
        for (int index = 0; index < gl.drawBuffers; ++index) {
            if (blend[index].enabled) glEnablei(GL_BLEND, index);
            else glDisablei(GL_BLEND, index);
            glColorMaski(index, blend[index].colorMask[0], blend[index].colorMask[1], blend[index].colorMask[2], blend[index].colorMask[3]);
            if (gl.blendEquationIndexed && gl.blendFunctionIndexed) {
                gl.blendEquationIndexed(index, blend[index].equationRgb, blend[index].equationAlpha);
                gl.blendFunctionIndexed(index, blend[index].sourceRgb, blend[index].destinationRgb, blend[index].sourceAlpha,
                    blend[index].destinationAlpha);
            }
        }
        if (gl.viewports > 1) {
            for (int index = 0; index < gl.viewports; ++index) {
                if (scissorEnabled[index]) glEnablei(GL_SCISSOR_TEST, index);
                else glDisablei(GL_SCISSOR_TEST, index);
            }
        }
    }

    explicit operator bool() const { return true; }

private:
    struct StencilState { GLint function, reference, valueMask, writeMask, fail, depthFail, pass; };
    void SaveStencil(StencilState& state, bool back) {
        glGetIntegerv(back ? GL_STENCIL_BACK_FUNC : GL_STENCIL_FUNC, &state.function);
        glGetIntegerv(back ? GL_STENCIL_BACK_REF : GL_STENCIL_REF, &state.reference);
        glGetIntegerv(back ? GL_STENCIL_BACK_VALUE_MASK : GL_STENCIL_VALUE_MASK, &state.valueMask);
        glGetIntegerv(back ? GL_STENCIL_BACK_WRITEMASK : GL_STENCIL_WRITEMASK, &state.writeMask);
        glGetIntegerv(back ? GL_STENCIL_BACK_FAIL : GL_STENCIL_FAIL, &state.fail);
        glGetIntegerv(back ? GL_STENCIL_BACK_PASS_DEPTH_FAIL : GL_STENCIL_PASS_DEPTH_FAIL, &state.depthFail);
        glGetIntegerv(back ? GL_STENCIL_BACK_PASS_DEPTH_PASS : GL_STENCIL_PASS_DEPTH_PASS, &state.pass);
    }
    void RestoreStencil(const StencilState& state, GLenum face) {
        glStencilFuncSeparate(face, state.function, state.reference, static_cast<GLuint>(state.valueMask));
        glStencilMaskSeparate(face, static_cast<GLuint>(state.writeMask));
        glStencilOpSeparate(face, state.fail, state.depthFail, state.pass);
    }
    void Disable(GLenum capability) {
        enable[enableCount++] = {capability, glIsEnabled(capability)};
        glDisable(capability);
    }
    GlFunctions& gl;
    GLint program = 0, activeTexture = GL_TEXTURE0, vertexArray = 0, arrayBuffer = 0, elementBuffer = 0;
    GLint packBuffer = 0, unpackBuffer = 0, renderbuffer = 0, drawFramebuffer = 0, readFramebuffer = 0, readBuffer = GL_BACK;
    GLint defaultDrawBuffer = GL_BACK, defaultReadBuffer = GL_BACK;
    GLint blendEquationRgb = GL_FUNC_ADD, blendEquationAlpha = GL_FUNC_ADD;
    GLint blendSourceRgb = GL_ONE, blendDestinationRgb = GL_ZERO, blendSourceAlpha = GL_ONE, blendDestinationAlpha = GL_ZERO;
    GLint clearStencil = 0, clipOrigin = 0, clipDepthMode = 0;
    GLboolean depthMask = GL_TRUE;
    std::array<GLint, 32> drawBuffer{};
    std::array<GLint, 2> textures{}, samplers{}, polygonMode{};
    std::array<GLint, 4> viewport{}, scissor{};
    std::array<GLfloat, 4> blendColor{}, clearColor{};
    std::array<GLboolean, 32> scissorEnabled{};
    StencilState stencilFront{}, stencilBack{};
    struct BlendState {
        GLboolean enabled = GL_FALSE;
        std::array<GLboolean, 4> colorMask{};
        GLint equationRgb = GL_FUNC_ADD, equationAlpha = GL_FUNC_ADD;
        GLint sourceRgb = GL_ONE, destinationRgb = GL_ZERO, sourceAlpha = GL_ONE, destinationAlpha = GL_ZERO;
    };
    std::array<BlendState, 32> blend{};
    struct EnableState { GLenum capability; GLboolean enabled; };
    std::array<EnableState, 64> enable{};
    std::size_t enableCount = 0;
    static constexpr std::array<GLenum, 8> unpackNames{GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT,
        GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_IMAGES, GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST};
    std::array<GLint, unpackNames.size()> unpack{};
};

class UiSystem final : public SystemInterface_Win32 {
public:
    UiSystem() : epoch(RawCounter()), frequency(CounterFrequency()) {}
    double GetElapsedTime() override {
        return frequency > 0 ? static_cast<double>(RawCounter() - epoch) / static_cast<double>(frequency) : 0.;
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        Log(message);
        if (type == Rml::Log::LT_ERROR)
            lastError = message;
        return true;
    }
    const std::string& LastError() const { return lastError; }
    void SetMouseCursor(const Rml::String& name) override {
        const wchar_t* resource = name == "move" ? IDC_SIZEALL :
            name == "pointer" ? IDC_HAND : name == "text" ? IDC_IBEAM : IDC_ARROW;
        const HCURSOR cursor = LoadCursorW(nullptr, resource);
        if (g_uiCursor.exchange(cursor, std::memory_order_relaxed) != cursor)
            RequestCursorUpdate();
        // Apply on the HWND thread and preserve the game's window class cursor.
    }
    void ActivateKeyboard(Rml::Vector2f, float) override {}
private:
    std::int64_t epoch;
    std::int64_t frequency;
    std::string lastError;
};

std::vector<Rml::byte> ReadBytes(const std::filesystem::path& file, std::size_t maximum);

class UiRenderer final : public RenderInterface_GL3 {
public:
    explicit UiRenderer(HGLRC owner) : owner(owner) {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override {
        texture_dimensions = {};
        const auto fail = [&](const char* reason) -> Rml::TextureHandle {
            Log("Cannot load Ore UI texture '" + source + "': " + reason);
            return {};
        };
        if (owner != wglGetCurrentContext())
            return fail("the owning OpenGL context is not current.");
        if (source.empty() || source.size() > 32767 || source.find('\0') != Rml::String::npos ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0))
            return fail("the asset path is not valid UTF-8.");

        // Memory-loaded RML has no file URL. Resolve its resource paths against
        // this DLL's assets, without changing the game's process directory.
        const std::filesystem::path requested(RmlWin32::ConvertToUTF16(source));
        if (!requested.is_absolute() && requested.has_root_path())
            return fail("drive-relative and root-relative asset paths are unsupported.");
        std::error_code pathError;
        const auto root = std::filesystem::weakly_canonical(g_assetDirectory, pathError);
        if (pathError || root.empty())
            return fail("the package asset directory is unavailable.");
        const auto file = std::filesystem::weakly_canonical(requested.is_absolute() ? requested : root / requested, pathError);
        if (pathError)
            return fail("the asset path cannot be resolved.");
        auto filePart = file.begin();
        for (const auto& rootPart : root) {
            if (filePart == file.end() || CompareStringOrdinal(rootPart.c_str(), static_cast<int>(rootPart.native().size()),
                    filePart->c_str(), static_cast<int>(filePart->native().size()), TRUE) != CSTR_EQUAL)
                return fail("the path is outside the package asset directory.");
            ++filePart;
        }
        if (filePart == file.end())
            return fail("the path names the asset directory instead of a texture.");
        for (; filePart != file.end(); ++filePart) {
            if (filePart->native().find(L':') != std::wstring::npos)
                return fail("alternate data streams are unsupported.");
        }
        if (_wcsicmp(file.extension().c_str(), L".tga") != 0)
            return fail("only packaged uncompressed TGA textures are supported.");

        constexpr std::size_t kHeaderBytes = 18;
        constexpr std::size_t kMaximumImageBytes = 64 * 1024 * 1024;
        const auto bytes = ReadBytes(file, kHeaderBytes + kMaximumImageBytes);
        if (bytes.size() < kHeaderBytes)
            return fail("the texture is missing or has a truncated TGA header.");
        // The packaged conversion uses type 2 BGRA, eight alpha bits, and a
        // top-left origin; reject palette, RLE, and differently ordered files.
        if (bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 2 || bytes[3] != 0 || bytes[4] != 0 ||
            bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0 || bytes[16] != 32 || bytes[17] != 0x28)
            return fail("expected a 32-bit top-origin uncompressed TGA with eight alpha bits.");
        const int width = static_cast<int>(bytes[12]) | (static_cast<int>(bytes[13]) << 8);
        const int height = static_cast<int>(bytes[14]) | (static_cast<int>(bytes[15]) << 8);
        GLint maximumTextureSize = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
            width > maximumTextureSize || height > maximumTextureSize)
            return fail("the TGA dimensions are invalid or exceed the renderer limit.");
        const std::size_t imageBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        if (imageBytes > kMaximumImageBytes || bytes.size() != kHeaderBytes + imageBytes)
            return fail("the TGA pixel byte count does not match its dimensions.");

        std::vector<Rml::byte> rgba(imageBytes);
        for (std::size_t pixel = 0; pixel < imageBytes; pixel += 4) {
            const auto* bgra = bytes.data() + kHeaderBytes + pixel;
            const auto alpha = bgra[3];
            // The GL3 backend expects premultiplied RGBA, including transparent
            // padding between the original Ore UI nine-patch button images.
            rgba[pixel] = static_cast<Rml::byte>((static_cast<unsigned int>(bgra[2]) * alpha) / 255);
            rgba[pixel + 1] = static_cast<Rml::byte>((static_cast<unsigned int>(bgra[1]) * alpha) / 255);
            rgba[pixel + 2] = static_cast<Rml::byte>((static_cast<unsigned int>(bgra[0]) * alpha) / 255);
            rgba[pixel + 3] = alpha;
        }
        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        const auto texture = GenerateTexture({rgba.data(), rgba.size()}, {width, height});
        if (texture) {
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texture_dimensions = {width, height};
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        return texture ? texture : fail("OpenGL texture creation failed.");
    }
    void ReleaseTexture(Rml::TextureHandle texture) override {
        // A lost GL context has already destroyed its textures. Deleting those
        // names in a replacement context could delete unrelated game resources.
        if (owner == wglGetCurrentContext())
            RenderInterface_GL3::ReleaseTexture(texture);
    }
private:
    HGLRC owner;
};

std::unique_ptr<UiSystem> g_system;
std::unique_ptr<UiRenderer> g_renderer;
Rml::Context* g_context = nullptr;
Rml::ElementDocument* g_document = nullptr;
HGLRC g_contextOwner = nullptr;
HGLRC g_rejectedContext = nullptr;
bool g_rmlInitialized = false;
bool g_reflectingControls = false;
bool g_documentVisible = false;
bool g_panelVisible = false;
bool g_centerPanelPending = true;
bool g_hintElementVisible = false;
bool g_hintPending = true;
bool g_hintScheduled = false;
double g_hintStartedAt = -1.;
constexpr double kStartupHintDuration = 8.;
constexpr double kStartupHintFadeDuration = .4;
std::array<std::vector<Rml::byte>, 5> g_oreFonts;
std::array<bool, 256> g_heldKeys{};
std::array<bool, 3> g_heldButtons{};
wchar_t g_pendingSurrogate = 0;
double g_statusUpdate = 0.;
constexpr std::size_t kHistorySamples = 24;
constexpr double kHistoryInterval = .5;
struct SpeedSample { double speed = 0.; bool paused = false; bool valid = false; };
std::array<SpeedSample, kHistorySamples> g_speedHistory{};
std::array<Rml::Element*, kHistorySamples> g_historyBars{};
std::size_t g_historyNext = 0;
std::size_t g_historyCount = 0;
double g_historyNextSample = 0.;
double g_heldSince = 0.;
double g_observedSpeed = 1.;
bool g_observedPaused = false;
bool g_telemetryStarted = false;
bool g_historyDirty = true;
bool g_draggingPanel = false;
Rml::Vector2f g_dragPanelStart{}, g_dragMouseStart{};

std::vector<Rml::byte> ReadBytes(const std::filesystem::path& file, std::size_t maximum) {
    const HANDLE input = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(input, &length) || length.QuadPart <= 0 || static_cast<unsigned long long>(length.QuadPart) > maximum) {
        CloseHandle(input);
        return {};
    }
    std::vector<Rml::byte> bytes(static_cast<std::size_t>(length.QuadPart));
    DWORD read = 0;
    const bool success = ReadFile(input, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    CloseHandle(input);
    return success ? bytes : std::vector<Rml::byte>{};
}

std::string FormatSpeed(double speed) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.2f", speed);
    return buffer;
}

double SliderPositionForSpeed(double speed) {
    if (!std::isfinite(speed))
        return 50.;
    speed = std::clamp(speed, .01, 16.);
    // Two logarithmic halves place normal speed at the center: 0.01x to
    // 1x occupies 0-50%, and 1x to 16x occupies 50-100%.
    const double position = speed <= 1. ? 50. + 25. * std::log10(speed) :
        50. + 50. * std::log(speed) / std::log(16.);
    return std::clamp(position, 0., 100.);
}

double SpeedForSliderPosition(double position) {
    if (!std::isfinite(position))
        return 1.;
    position = std::clamp(position, 0., 100.);
    const double speed = position <= 50. ? std::pow(10., (position - 50.) / 25.) :
        std::pow(16., (position - 50.) / 50.);
    return std::clamp(std::round(speed * 100.) / 100., .01, 16.);
}

void AppendHistory(SpeedSample sample) {
    g_speedHistory[g_historyNext] = sample;
    g_historyNext = (g_historyNext + 1) % kHistorySamples;
    g_historyCount = std::min(g_historyCount + 1, kHistorySamples);
    g_historyDirty = true;
}

void ObserveSpeed(double now, const ClockStatus& status) {
    if (!g_telemetryStarted || status.paused != g_observedPaused || std::abs(status.speed - g_observedSpeed) > .000001) {
        g_heldSince = now;
        g_observedSpeed = status.speed;
        g_observedPaused = status.paused;
    }
    if (!g_telemetryStarted) {
        g_telemetryStarted = true;
        AppendHistory({status.speed, status.paused, true});
        g_historyNextSample = now + kHistoryInterval;
    } else if (now >= g_historyNextSample) {
        const double missed = std::floor((now - g_historyNextSample) / kHistoryInterval);
        // A stalled or minimized game has no samples for those intervals. Leave
        // gaps instead of inventing historical values from the current speed.
        const auto gaps = static_cast<std::size_t>(std::min(missed, static_cast<double>(kHistorySamples - 1)));
        for (std::size_t index = 0; index < gaps; ++index)
            AppendHistory({});
        AppendHistory({status.speed, status.paused, true});
        g_historyNextSample += (missed + 1.) * kHistoryInterval;
    }
}

void InitializeHistoryBars() {
    g_historyBars.fill(nullptr);
    if (auto* history = g_document->GetElementById("speed-history")) {
        std::string markup;
        for (std::size_t index = 0; index < kHistorySamples; ++index)
            markup += "<div id=\"history-bar-" + std::to_string(index) + "\" class=\"history-bar empty\"></div>";
        history->SetInnerRML(markup);
        for (std::size_t index = 0; index < kHistorySamples; ++index)
            g_historyBars[index] = g_document->GetElementById("history-bar-" + std::to_string(index));
    }
    g_historyDirty = true;
}

void ReflectHistory() {
    if (!g_historyDirty)
        return;
    const std::size_t empty = kHistorySamples - g_historyCount;
    const std::size_t first = (g_historyNext + kHistorySamples - g_historyCount) % kHistorySamples;
    for (std::size_t index = 0; index < kHistorySamples; ++index) {
        Rml::Element* bar = g_historyBars[index];
        if (!bar)
            continue;
        const SpeedSample sample = index < empty ? SpeedSample{} : g_speedHistory[(first + index - empty) % kHistorySamples];
        // A fixed log(1 + speed) scale keeps slow speeds readable and maps 0x
        // to zero, 1x to 24.5%, and 16x to the full height. No animated waveform.
        const double height = sample.valid ? std::log1p(std::clamp(sample.speed, 0., 16.)) / std::log(17.) * 100. : 0.;
        char value[32]{};
        std::snprintf(value, sizeof(value), "%.2f%%", height);
        bar->SetProperty("height", value);
        bar->SetClass("empty", !sample.valid);
        bar->SetClass("paused", sample.valid && sample.paused);
        bar->SetClass("recent", index >= kHistorySamples - 6);
        bar->SetClass("latest", sample.valid && index == kHistorySamples - 1);
        bar->SetAttribute("title", sample.valid ? FormatSpeed(sample.speed) + "x" : "尚无采样");
    }
    g_historyDirty = false;
}

std::string HeldDuration(double now) {
    const auto seconds = static_cast<unsigned long long>(std::max(0., std::floor(now - g_heldSince)));
    char text[32]{};
    std::snprintf(text, sizeof(text), "%02llu:%02llu", seconds / 60, seconds % 60);
    return text;
}

Rml::Vector2f ClampPanelPosition(Rml::Vector2f desired) {
    auto* panel = g_document ? g_document->GetElementById("panel") : nullptr;
    if (!panel || !g_context)
        return desired;
    const auto viewport = g_context->GetDimensions();
    const auto size = panel->GetBox().GetSize(Rml::BoxArea::Border);
    const float gap = 12.f * g_context->GetDensityIndependentPixelRatio();
    const float marginX = std::min(gap, std::max(0.f, (static_cast<float>(viewport.x) - size.x) * .5f));
    const float marginY = std::min(gap, std::max(0.f, (static_cast<float>(viewport.y) - size.y) * .5f));
    desired.x = std::clamp(desired.x, marginX, std::max(marginX, static_cast<float>(viewport.x) - size.x - marginX));
    desired.y = std::clamp(desired.y, marginY, std::max(marginY, static_cast<float>(viewport.y) - size.y - marginY));
    return desired;
}

void PositionPanel(Rml::Vector2f position) {
    auto* panel = g_document ? g_document->GetElementById("panel") : nullptr;
    if (!panel || !g_context)
        return;
    position = ClampPanelPosition(position);
    const float density = std::max(.1f, g_context->GetDensityIndependentPixelRatio());
    char left[32]{}, top[32]{};
    // RmlUi mouse/layout coordinates are physical pixels; the document uses dp.
    std::snprintf(left, sizeof(left), "%.3fdp", position.x / density);
    std::snprintf(top, sizeof(top), "%.3fdp", position.y / density);
    panel->SetProperty("left", left);
    panel->SetProperty("top", top);
}

bool KeepPanelInViewport() {
    auto* panel = g_document ? g_document->GetElementById("panel") : nullptr;
    if (!panel)
        return false;
    const auto position = panel->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto clamped = ClampPanelPosition(position);
    if (std::abs(position.x - clamped.x) < .5f && std::abs(position.y - clamped.y) < .5f)
        return false;
    PositionPanel(clamped);
    return true;
}

bool EditingSpeed() {
    const auto* focused = g_context ? g_context->GetFocusElement() : nullptr;
    return focused && focused->GetId() == "speed-input";
}

Rml::Element* SliderThumb(Rml::Element* slider) {
    // WidgetSlider adds its thumb as a non-DOM child. Ordinary DOM searches
    // miss it; its active state tracks pointer dragging, unlike keyboard focus.
    for (int index = 0; slider && index < slider->GetNumChildren(true); ++index) {
        auto* child = slider->GetChild(index);
        if (child && child->GetTagName() == "sliderbar")
            return child;
    }
    return nullptr;
}

void ReflectControls() {
    if (!g_document)
        return;
    g_reflectingControls = true;
    const auto status = Status();
    const double selected = status.paused ? status.resumeSpeed : status.speed;
    if (!EditingSpeed()) {
        if (auto* control = dynamic_cast<Rml::ElementFormControl*>(g_document->GetElementById("speed-input")))
            control->SetValue(FormatSpeed(selected));
    }
    if (auto* slider = g_document->GetElementById("speed-slider")) {
        slider->SetClass("paused", status.paused);
        const auto* thumb = SliderThumb(slider);
        // Rounding slow speeds to 0.01x changes their inverse position. Let
        // the pointer own the thumb until release while still updating labels.
        if (!thumb || !thumb->IsPseudoClassSet("active")) {
            if (auto* control = dynamic_cast<Rml::ElementFormControl*>(slider))
                control->SetValue(FormatSpeed(SliderPositionForSpeed(selected)));
        }
    }
    if (auto* value = g_document->GetElementById("slider-value"))
        value->SetInnerRML(FormatSpeed(selected) + "×");
    if (auto* caption = g_document->GetElementById("slider-caption"))
        caption->SetInnerRML(status.paused ? "恢复倍率" : "拖动调节");
    if (auto* pause = g_document->GetElementById("pause"))
        pause->SetInnerRML(status.paused ? "继续运行" : "暂停时间");
    if (auto* panel = g_document->GetElementById("panel"))
        panel->SetClass("paused", status.paused);
    if (auto* mode = g_document->GetElementById("mode-pill")) {
        mode->SetClass("paused", status.paused);
        mode->SetClass("boost", !status.paused && status.speed > 1.000001);
        mode->SetClass("slow", !status.paused && status.speed < .999999);
        mode->SetInnerRML(status.paused ? "已暂停" : status.speed > 1.000001 ? "加速运行" : status.speed < .999999 ? "慢速运行" : "正常运行");
    }
    Rml::ElementList buttons;
    g_document->GetElementsByTagName(buttons, "button");
    for (auto* button : buttons) {
        if (button->HasAttribute("data-speed"))
            button->SetClass("selected", std::abs(button->GetAttribute<double>("data-speed", 1.) - selected) < .005);
    }
    g_reflectingControls = false;
}

bool ApplyNumericValue() {
    auto* control = dynamic_cast<Rml::ElementFormControl*>(g_document->GetElementById("speed-input"));
    if (!control)
        return false;
    const auto value = control->GetValue();
    char* end = nullptr;
    const double speed = std::strtod(value.c_str(), &end);
    while (end && (*end == ' ' || *end == '\t')) ++end;
    const bool valid = end && end != value.c_str() && *end == '\0' && std::isfinite(speed) && speed >= .01 && speed <= 16.;
    const bool applied = valid && SetSpeed(speed);
    if (auto* error = g_document->GetElementById("input-error"))
        error->SetInnerRML(applied ? "" : "请输入 0.01 到 16.00 之间的倍率。");
    return applied;
}

class UiEvents final : public Rml::EventListener {
    void ProcessEvent(Rml::Event& event) override {
        if (g_reflectingControls || !g_document)
            return;
        Rml::Element* element = event.GetTargetElement();
        const auto& type = event.GetType();
        if (type == "dragstart" || type == "drag" || type == "dragend") {
            auto* slider = element;
            while (slider && slider != g_document && slider->GetId() != "speed-slider")
                slider = slider->GetParentNode();
            if (slider && slider != g_document) {
                // WidgetSlider clears the thumb's active state before this
                // event bubbles to the document, allowing a final sync.
                if (type == "dragend")
                    ReflectControls();
                return;
            }
            auto* handle = element;
            while (handle && handle != g_document && handle->GetId() != "drag-handle")
                handle = handle->GetParentNode();
            if (!handle || handle == g_document)
                return;
            auto* panel = g_document->GetElementById("panel");
            if (!panel)
                return;
            const Rml::Vector2f mouse(event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f));
            if (type == "dragstart") {
                g_dragPanelStart = panel->GetAbsoluteOffset(Rml::BoxArea::Border);
                g_dragMouseStart = mouse;
                g_draggingPanel = true;
                panel->SetClass("dragging", true);
            } else if (g_draggingPanel) {
                PositionPanel(g_dragPanelStart + mouse - g_dragMouseStart);
                if (type == "dragend") {
                    g_draggingPanel = false;
                    panel->SetClass("dragging", false);
                }
            }
            return;
        }
        if (event.GetType() == "change" && element && element->GetId() == "speed-slider") {
            const auto value = event.GetParameter<Rml::String>("value", "50");
            char* end = nullptr;
            const double position = std::strtod(value.c_str(), &end);
            if (end && end != value.c_str() && *end == '\0' && std::isfinite(position) &&
                SetSpeed(SpeedForSliderPosition(position)))
                ReflectControls();
            return;
        }
        if (event.GetType() != "click")
            return;
        while (element && element != g_document && element->GetTagName() != "button")
            element = element->GetParentNode();
        if (!element || element == g_document)
            return;
        const auto& id = element->GetId();
        if (id == "pause") TogglePause();
        else if (id == "reset") Reset();
        else if (id == "close") SetUiVisible(false);
        else if (id == "apply") ApplyNumericValue();
        else if (element->HasAttribute("data-speed")) SetSpeed(element->GetAttribute<double>("data-speed", 1.));
        ReflectControls();
    }
};
UiEvents g_events;

void ResetUiInput() {
    if (!g_context)
        return;
    for (std::size_t key = 0; key < g_heldKeys.size(); ++key) {
        if (g_heldKeys[key]) g_context->ProcessKeyUp(RmlWin32::ConvertKey(static_cast<int>(key)), 0);
        g_heldKeys[key] = false;
    }
    for (std::size_t button = 0; button < g_heldButtons.size(); ++button) {
        if (g_heldButtons[button]) g_context->ProcessMouseButtonUp(static_cast<int>(button), 0);
        g_heldButtons[button] = false;
    }
    g_context->ProcessMouseLeave();
    g_pendingSurrogate = 0;
    g_draggingPanel = false;
    if (auto* panel = g_document ? g_document->GetElementById("panel") : nullptr)
        panel->SetClass("dragging", false);
    if (auto* slider = g_document ? g_document->GetElementById("speed-slider") : nullptr) {
        if (auto* thumb = SliderThumb(slider))
            thumb->SetPseudoClass("active", false);
        ReflectControls();
    }
}

bool UpdateStartupHint(double now, bool panelVisible) {
    if (panelVisible || g_hintDismissRequested.load(std::memory_order_acquire))
        g_hintScheduled = false;
    if (g_hintScheduled && g_hintStartedAt < 0.)
        // Start after UI initialization, at the first frame that can draw the
        // hint. UiSystem uses the real QPC, even when the game clock is paused.
        g_hintStartedAt = now;
    const bool visible = g_hintScheduled && now - g_hintStartedAt < kStartupHintDuration;
    if (!visible)
        g_hintScheduled = false;
    g_hintVisible.store(visible, std::memory_order_release);
    return visible;
}

void SyncDocumentVisibility(bool panelVisible, bool hintVisible) {
    if (panelVisible != g_panelVisible) {
        if (!panelVisible)
            ResetUiInput();
        if (auto* backdrop = g_document->GetElementById("panel-backdrop"))
            backdrop->SetProperty("display", panelVisible ? "block" : "none");
        if (auto* panel = g_document->GetElementById("panel"))
            panel->SetProperty("display", panelVisible ? "flex" : "none");
        g_panelVisible = panelVisible;
    }
    if (hintVisible != g_hintElementVisible) {
        if (auto* hint = g_document->GetElementById("startup-hint"))
            hint->SetProperty("display", hintVisible ? "block" : "none");
        g_hintElementVisible = hintVisible;
    }
    const bool documentVisible = panelVisible || hintVisible;
    if (documentVisible != g_documentVisible) {
        if (documentVisible)
            g_document->Show(Rml::ModalFlag::None, panelVisible ? Rml::FocusFlag::Auto : Rml::FocusFlag::None);
        else
            g_document->Hide();
        g_documentVisible = documentVisible;
    }
}

void ReflectStartupHint(double now) {
    const double remaining = std::clamp(kStartupHintDuration - (now - g_hintStartedAt), 0., kStartupHintDuration);
    char width[32]{}, opacity[32]{};
    std::snprintf(width, sizeof(width), "%.3f%%", remaining / kStartupHintDuration * 100.);
    std::snprintf(opacity, sizeof(opacity), "%.3f", std::clamp(remaining / kStartupHintFadeDuration, 0., 1.));
    if (auto* progress = g_document->GetElementById("startup-hint-progress"))
        progress->SetProperty("width", width);
    if (auto* hint = g_document->GetElementById("startup-hint"))
        hint->SetProperty("opacity", opacity);
}

void DrainInput() {
    std::array<InputEvent, kInputCapacity> events{};
    std::size_t count = 0;
    bool reset = false;
    {
        std::lock_guard lock(g_inputMutex);
        count = g_inputCount;
        for (std::size_t index = 0; index < count; ++index)
            events[index] = g_input[(g_inputBegin + index) % kInputCapacity];
        g_inputBegin = g_inputCount = 0;
        reset = g_resetInput;
        g_resetInput = false;
    }
    if (reset) ResetUiInput();
    if (!UiVisible())
        return;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& input = events[index];
        const UINT message = input.message;
        switch (message) {
        case WM_MOUSEMOVE:
            g_context->ProcessMouseMove(GET_X_LPARAM(input.lParam), GET_Y_LPARAM(input.lParam), input.modifiers);
            break;
        case WM_MOUSELEAVE: g_context->ProcessMouseLeave(); break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
            const int button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK) ? 0 :
                (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK) ? 1 : 2;
            const bool down = message != WM_LBUTTONUP && message != WM_RBUTTONUP && message != WM_MBUTTONUP;
            g_heldButtons[button] = down;
            if (down) g_context->ProcessMouseButtonDown(button, input.modifiers);
            else g_context->ProcessMouseButtonUp(button, input.modifiers);
            break;
        }
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
            const float amount = static_cast<float>(GET_WHEEL_DELTA_WPARAM(input.wParam)) / WHEEL_DELTA;
            g_context->ProcessMouseWheel(message == WM_MOUSEWHEEL ? Rml::Vector2f(0.f, -amount) : Rml::Vector2f(amount, 0.f), input.modifiers);
            break;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const int key = static_cast<int>(input.wParam);
            const bool firstPress = !(input.lParam & (LPARAM(1) << 30));
            bool handled = false;
            if (firstPress && !EditingSpeed()) {
                if ((key == VK_RETURN || key == VK_SPACE) && g_context->GetFocusElement() &&
                    g_context->GetFocusElement()->GetTagName() == "button") {
                    g_context->GetFocusElement()->Click();
                    handled = true;
                } else if (key == VK_PAUSE) { TogglePause(); handled = true; }
                else if (key == VK_HOME) { Reset(); handled = true; }
                else if (key == VK_ADD || key == VK_OEM_PLUS) {
                    const auto status = Status();
                    SetSpeed(std::min(16., (status.paused ? status.resumeSpeed : status.speed) + .1));
                    handled = true;
                } else if (key == VK_SUBTRACT || key == VK_OEM_MINUS) {
                    const auto status = Status();
                    SetSpeed(std::max(.01, (status.paused ? status.resumeSpeed : status.speed) - .1));
                    handled = true;
                }
            } else if (firstPress && key == VK_RETURN && EditingSpeed()) {
                ApplyNumericValue();
                handled = true;
            }
            if (handled) ReflectControls();
            else {
                if (key >= 0 && key < static_cast<int>(g_heldKeys.size())) g_heldKeys[key] = true;
                g_context->ProcessKeyDown(RmlWin32::ConvertKey(key), input.modifiers);
            }
            break;
        }
        case WM_KEYUP: case WM_SYSKEYUP: {
            const int key = static_cast<int>(input.wParam);
            if (key >= 0 && key < static_cast<int>(g_heldKeys.size())) g_heldKeys[key] = false;
            g_context->ProcessKeyUp(RmlWin32::ConvertKey(key), input.modifiers);
            break;
        }
        case WM_UNICHAR:
            g_context->ProcessTextInput(static_cast<Rml::Character>(input.wParam));
            break;
        case WM_CHAR: {
            const auto character = static_cast<wchar_t>(input.wParam);
            if (character >= 0xd800 && character <= 0xdbff) {
                g_pendingSurrogate = character;
            } else {
                std::uint32_t codepoint = character;
                if (character >= 0xdc00 && character <= 0xdfff && g_pendingSurrogate)
                    codepoint = 0x10000 + ((g_pendingSurrogate - 0xd800) << 10) + (character - 0xdc00);
                g_pendingSurrogate = 0;
                if (codepoint >= 32 && codepoint != 127)
                    g_context->ProcessTextInput(static_cast<Rml::Character>(codepoint));
            }
            break;
        }
        }
    }
}

void ShutdownUiOnRenderThread() {
    g_ready.store(false, std::memory_order_release);
    RequestCursorUpdate();
    ResetQueuedInput();
    if (g_rmlInitialized) Rml::Shutdown();
    g_rmlInitialized = false;
    g_context = nullptr;
    g_document = nullptr;
    g_documentVisible = false;
    g_panelVisible = false;
    g_centerPanelPending = true;
    g_hintElementVisible = false;
    g_hintScheduled = false;
    g_hintStartedAt = -1.;
    g_hintVisible.store(false, std::memory_order_release);
    g_renderer.reset();
    // Initialization can fail while constructing shaders, before Rml::Initialise
    // was called. Clear the registered interfaces in that case as well.
    Rml::SetRenderInterface(nullptr);
    Rml::SetSystemInterface(nullptr);
    g_system.reset();
    for (auto& font : g_oreFonts) font.clear();
    g_heldKeys.fill(false);
    g_heldButtons.fill(false);
    g_pendingSurrogate = 0;
    g_draggingPanel = false;
    g_telemetryStarted = false;
    g_historyCount = g_historyNext = 0;
    g_historyNextSample = g_heldSince = 0.;
    g_speedHistory.fill({});
    g_historyBars.fill(nullptr);
}

bool InitializeUiOnRenderThread(HWND window, HGLRC context, int width, int height) {
    if (g_rmlInitialized) ShutdownUiOnRenderThread();
    g_contextOwner = context;
    g_system = std::make_unique<UiSystem>();
    g_system->SetWindow(window);
    // Shader compilation happens in the renderer constructor. Route those logs
    // through our system interface even before RmlUi itself is initialized.
    Rml::SetSystemInterface(g_system.get());
    g_renderer = std::make_unique<UiRenderer>(context);
    if (!static_cast<bool>(*g_renderer)) {
        ReportError("Cannot construct OpenGL renderer. " + g_gl.driverVersion +
            (g_system->LastError().empty() ? "" : "; " + g_system->LastError()));
        return false;
    }
    Rml::SetRenderInterface(g_renderer.get());
    if (!Rml::Initialise()) {
        ReportError("RmlUi initialization failed.");
        return false;
    }
    g_rmlInitialized = true;
    struct FontSpec { const wchar_t* file; const char* family; Rml::Style::FontWeight weight; bool fallback; };
    const std::array<FontSpec, 5> fonts{{
        {L"oreui/fonts/OreBody-Regular.ttf", "Ore UI", Rml::Style::FontWeight::Normal, false},
        {L"oreui/fonts/OreBody-Bold.ttf", "Ore UI", Rml::Style::FontWeight::Bold, false},
        {L"oreui/fonts/OreSeven.otf", "Ore Seven", Rml::Style::FontWeight::Normal, false},
        {L"oreui/fonts/OreTen.otf", "Ore Ten", Rml::Style::FontWeight::Normal, false},
        {L"oreui/fonts/OreCjk-Regular.otf", "Ore CJK", Rml::Style::FontWeight::Normal, true}
    }};
    for (std::size_t index = 0; index < fonts.size(); ++index) {
        const auto& spec = fonts[index];
        auto& bytes = g_oreFonts[index];
        bytes = ReadBytes(g_assetDirectory / spec.file, 16 * 1024 * 1024);
        if (bytes.empty() || !Rml::LoadFontFace({bytes.data(), bytes.size()}, spec.family,
                Rml::Style::FontStyle::Normal, spec.weight, spec.fallback)) {
            ReportError("Cannot load packaged Ore UI font: " + RmlWin32::ConvertToUTF8(spec.file));
            return false;
        }
    }
    g_context = Rml::CreateContext("mcdk-gamespeed", {width, height});
    const auto document = ReadBytes(g_assetDirectory / L"speed.rml", 1024 * 1024);
    if (!g_context || document.empty()) {
        ReportError("Cannot create UI context or read assets/speed.rml.");
        return false;
    }
    g_context->EnableMouseCursor(false);
    g_document = g_context->LoadDocumentFromMemory(Rml::String(reinterpret_cast<const char*>(document.data()), document.size()));
    if (!g_document) {
        ReportError("Cannot parse assets/speed.rml.");
        return false;
    }
    g_document->AddEventListener("click", &g_events);
    g_document->AddEventListener("change", &g_events);
    g_document->AddEventListener("dragstart", &g_events);
    g_document->AddEventListener("drag", &g_events);
    g_document->AddEventListener("dragend", &g_events);
    InitializeHistoryBars();
    ReflectControls();
    g_centerPanelPending = true;
    g_statusUpdate = 0.;
    // Failed initialization never consumes the startup hint. Once scheduled,
    // it is not armed again by hide, Esc, or a subsequent UI initialization.
    if (g_hintPending) {
        g_hintPending = false;
        g_hintScheduled = !UiVisible() && !g_hintDismissRequested.load(std::memory_order_acquire);
        g_hintStartedAt = -1.;
    }
    g_ready.store(true, std::memory_order_release);
    {
        std::lock_guard lock(g_errorMutex);
        g_overlayError.clear();
    }
    RequestCursorUpdate();
    Log("RmlUi overlay is ready. F8 or Ctrl+Shift+G toggles the panel.");
    return true;
}

void DrawOverlay(HDC device) {
    const HGLRC current = wglGetCurrentContext();
    if (!current || wglGetCurrentDC() != device)
        return;
    const HWND window = WindowFromDC(device);
    DWORD process = 0;
    if (!window || !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId())
        return;
    const HWND root = GetAncestor(window, GA_ROOT);
    RECT rectangle{};
    if (!GetClientRect(window, &rectangle) || rectangle.right < 64 || rectangle.bottom < 64)
        return;
    const HWND selected = g_window.load(std::memory_order_acquire);
    if (selected && selected != window && IsWindow(selected))
        return;
    DWORD expected = 0;
    const DWORD thread = GetCurrentThreadId();
    if (!g_renderThread.compare_exchange_strong(expected, thread) && expected != thread)
        return;
    g_window.store(window, std::memory_order_release);
    EnsureWindowSubclass(root);
    if (current == g_rejectedContext)
        return;
    const bool initiallyVisible = UiVisible();
    if (current == g_contextOwner && g_ready.load(std::memory_order_acquire) && !initiallyVisible &&
        !UpdateStartupHint(g_system->GetElapsedTime(), initiallyVisible)) {
        // Keep real telemetry while hidden without creating any GL work.
        ObserveSpeed(g_system->GetElapsedTime(), Status());
        // Hide can release compiled GPU geometry. Preserve the game's object
        // bindings during that transition, then skip all GL work while hidden.
        if (g_documentVisible || g_panelVisible || g_hintElementVisible) {
            GlStateGuard state(g_gl);
            SyncDocumentVisibility(false, false);
        }
        DrainInput();
        return;
    }
    if (g_contextOwner && current != g_contextOwner) {
        // The GL3 renderer owns VAOs, textures, shaders, and framebuffers. Never
        // release them in an unrelated context where names can refer to game
        // resources. The original context may still resume its swap callbacks.
        g_ready.store(false, std::memory_order_release);
        g_hintVisible.store(false, std::memory_order_release);
        RequestCursorUpdate();
        g_rejectedContext = current;
        ReportError("The game's OpenGL context changed; overlay resources belong to the previous context.");
        return;
    }
    if (current != g_contextOwner) {
        GlFunctions functions;
        if (!functions.Initialize()) {
            g_ready.store(false, std::memory_order_release);
            RequestCursorUpdate();
            g_rejectedContext = current;
            return;
        }
        g_gl = functions;
    }
    GlStateGuard state(g_gl);
    if (!state)
        return;
    if (current != g_contextOwner || !g_ready.load(std::memory_order_acquire)) {
        if (!InitializeUiOnRenderThread(window, current, rectangle.right, rectangle.bottom)) {
            ShutdownUiOnRenderThread();
            g_rejectedContext = current;
            return;
        }
    }
    g_context->SetDimensions({rectangle.right, rectangle.bottom});
    if (const auto getDpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))
        g_context->SetDensityIndependentPixelRatio(static_cast<float>(getDpi(window)) / 96.f);
    const bool visible = UiVisible();
    const double now = g_system->GetElapsedTime();
    const bool hintVisible = UpdateStartupHint(now, visible);
    SyncDocumentVisibility(visible, hintVisible);
    DrainInput();
    if (!visible && !hintVisible)
        return;
    const auto status = Status();
    ObserveSpeed(now, status);
    if (visible && now >= g_statusUpdate) {
        if (auto* currentSpeed = g_document->GetElementById("current-speed"))
            currentSpeed->SetInnerRML(FormatSpeed(status.speed));
        if (auto* clock = g_document->GetElementById("clock-status"))
            clock->SetInnerRML(status.scaledCalls ? "计时已接管" : "等待游戏计时");
        if (auto* duration = g_document->GetElementById("held-duration"))
            duration->SetInnerRML(HeldDuration(now));
        ReflectControls();
        g_statusUpdate = now + .2;
    }
    if (visible)
        ReflectHistory();
    if (hintVisible)
        ReflectStartupHint(now);
    g_context->Update();
    if (visible) {
        if (g_centerPanelPending) {
            // Measure the laid-out panel in physical pixels, then center it
            // before its first render. Later opens preserve the dragged spot.
            if (auto* panel = g_document->GetElementById("panel")) {
                const auto viewport = g_context->GetDimensions();
                const auto size = panel->GetBox().GetSize(Rml::BoxArea::Border);
                PositionPanel({(static_cast<float>(viewport.x) - size.x) * .5f,
                    (static_cast<float>(viewport.y) - size.y) * .5f});
                g_centerPanelPending = false;
                g_context->Update();
            }
        } else if (KeepPanelInViewport()) {
            g_context->Update();
        }
    }
    g_renderer->SetViewport(rectangle.right, rectangle.bottom);
    g_renderer->BeginFrame();
    g_context->Render();
    g_renderer->EndFrame();
}

thread_local bool g_drawing = false;

BOOL SwapWithOverlay(HDC device, SwapFunction original) {
    if (!g_drawing) {
        g_drawing = true;
        try { DrawOverlay(device); }
        catch (const std::exception& error) {
            ReportError(std::string("Rendering disabled after exception: ") + error.what());
            g_ready.store(false, std::memory_order_release);
            g_hintVisible.store(false, std::memory_order_release);
            g_rejectedContext = wglGetCurrentContext();
            RequestCursorUpdate();
        }
        catch (...) {
            ReportError("Rendering disabled after an unexpected C++ exception.");
            g_ready.store(false, std::memory_order_release);
            g_hintVisible.store(false, std::memory_order_release);
            g_rejectedContext = wglGetCurrentContext();
            RequestCursorUpdate();
        }
        // Keep the recursion guard set through the original call: some GDI/WGL
        // implementations route one swap entry point through the other.
        const BOOL result = original(device);
        g_drawing = false;
        return result;
    }
    return original(device);
}

BOOL WINAPI SwapHook(HDC device) { return SwapWithOverlay(device, g_swap); }
BOOL WINAPI WglSwapHook(HDC device) { return SwapWithOverlay(device, g_wglSwap); }

bool InstallHook(void* target, void* detour, void** original, std::vector<void*>& targets, std::string& error) {
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        error = std::string("MH_CreateHook: ") + MH_StatusToString(status);
        return false;
    }
    targets.push_back(target);
    return true;
}
} // namespace

bool InitializeOverlay(HMODULE self, std::string& error) {
    g_module = self;
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(self, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) {
        error = "Cannot resolve DLL asset directory.";
        return false;
    }
    g_assetDirectory = std::filesystem::path(std::wstring(modulePath.data(), length)).parent_path() / L"assets";
    g_installMessage = RegisterWindowMessageW(L"MCDK.GameSpeed.InstallWindowSubclass.v1");
    g_cursorMessage = RegisterWindowMessageW(L"MCDK.GameSpeed.SyncCursor.v1");
    const HMODULE graphics = GetModuleHandleW(L"gdi32.dll");
    HMODULE openGl = GetModuleHandleW(L"opengl32.dll");
    if (!openGl) openGl = LoadLibraryW(L"opengl32.dll");
    const auto swap = reinterpret_cast<void*>(GetProcAddress(graphics, "SwapBuffers"));
    const auto wglSwap = reinterpret_cast<void*>(GetProcAddress(openGl, "wglSwapBuffers"));
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto clip = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
    const auto position = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
    const auto cursor = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursor"));
    const auto showCursor = reinterpret_cast<void*>(GetProcAddress(user32, "ShowCursor"));
    const auto capture = reinterpret_cast<void*>(GetProcAddress(user32, "SetCapture"));
    if (!swap || !clip || !position || !cursor || !showCursor || !capture || !g_installMessage || !g_cursorMessage) {
        error = "Required OpenGL/Win32 overlay entry points are unavailable.";
        return false;
    }
    std::vector<void*> targets;
    if (!InstallHook(swap, reinterpret_cast<void*>(SwapHook), reinterpret_cast<void**>(&g_swap), targets, error) ||
        (wglSwap && wglSwap != swap && !InstallHook(wglSwap, reinterpret_cast<void*>(WglSwapHook), reinterpret_cast<void**>(&g_wglSwap), targets, error)) ||
        !InstallHook(clip, reinterpret_cast<void*>(ClipCursorHook), reinterpret_cast<void**>(&g_clip), targets, error) ||
        !InstallHook(position, reinterpret_cast<void*>(SetCursorPositionHook), reinterpret_cast<void**>(&g_position), targets, error) ||
        !InstallHook(cursor, reinterpret_cast<void*>(SetCursorHook), reinterpret_cast<void**>(&g_cursor), targets, error) ||
        !InstallHook(showCursor, reinterpret_cast<void*>(ShowCursorHook), reinterpret_cast<void**>(&g_showCursor), targets, error) ||
        !InstallHook(capture, reinterpret_cast<void*>(ObserveCapture), reinterpret_cast<void**>(&g_capture), targets, error)) {
        for (void* target : targets) { MH_DisableHook(target); MH_RemoveHook(target); }
        return false;
    }
    // Populate every trampoline before enabling any swap callback. Rendering
    // can initialize immediately on another thread and needs the cursor APIs.
    for (void* target : targets) {
        const MH_STATUS queued = MH_QueueEnableHook(target);
        if (queued != MH_OK) {
            error = std::string("MH_QueueEnableHook: ") + MH_StatusToString(queued);
            for (void* installed : targets) { MH_DisableHook(installed); MH_RemoveHook(installed); }
            return false;
        }
    }
    const MH_STATUS enabled = MH_ApplyQueued();
    if (enabled != MH_OK) {
        error = std::string("MH_ApplyQueued: ") + MH_StatusToString(enabled);
        for (void* target : targets) { MH_DisableHook(target); MH_RemoveHook(target); }
        return false;
    }
    Log("SwapBuffers hooks installed; waiting for the game's OpenGL render thread.");
    return true;
}

void SetUiVisible(bool visible) {
    if (visible) {
        g_hintDismissRequested.store(true, std::memory_order_release);
        g_hintVisible.store(false, std::memory_order_release);
    }
    const bool wasVisible = g_visible.exchange(visible, std::memory_order_acq_rel);
    if (!visible && wasVisible) ResetQueuedInput();
    RequestCursorUpdate();
}

bool UiVisible() { return g_visible.load(std::memory_order_acquire); }
bool StartupHintVisible() {
    return g_hintVisible.load(std::memory_order_acquire) && !g_hintDismissRequested.load(std::memory_order_acquire);
}
bool OverlayReady() { return g_ready.load(std::memory_order_acquire); }
std::string OverlayError() {
    std::lock_guard lock(g_errorMutex);
    return g_overlayError;
}
void BeginInputTrace() {
    std::lock_guard lock(g_inputTraceMutex);
    g_inputTraceNext = g_inputTraceCount = 0;
    g_inputTraceEnabled.store(true, std::memory_order_release);
}

std::string EndInputTrace() {
    g_inputTraceEnabled.store(false, std::memory_order_release);
    std::lock_guard lock(g_inputTraceMutex);
    const auto path = g_assetDirectory.parent_path() / (L"input-trace-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    std::ofstream output(path, std::ios::binary);
    if (!output) return {};
    output << "{\"window\":" << reinterpret_cast<std::uintptr_t>(g_window.load())
        << ",\"subclassWindow\":" << reinterpret_cast<std::uintptr_t>(g_subclassWindow.load())
        << ",\"mainBase\":" << reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))
        << ",\"selfBase\":" << reinterpret_cast<std::uintptr_t>(g_module) << ",\"events\":[";
    const auto begin = (g_inputTraceNext + g_inputTrace.size() - g_inputTraceCount) % g_inputTrace.size();
    for (std::size_t i = 0; i < g_inputTraceCount; ++i) {
        const auto& e = g_inputTrace[(begin + i) % g_inputTrace.size()];
        if (i) output << ',';
        output << "{\"ms\":" << e.time << ",\"thread\":" << e.thread << ",\"event\":\"" << e.event
            << "\",\"value\":" << e.value << ",\"result\":" << e.result << ",\"caller\":" << e.caller
            << ",\"foreground\":" << reinterpret_cast<std::uintptr_t>(e.foreground)
            << ",\"visible\":" << (e.visible ? "true" : "false") << ",\"inactive\":" << (e.inactive ? "true" : "false") << '}';
    }
    output << "]}";
    output.close();
    return output ? RmlWin32::ConvertToUTF8(path.wstring()) : std::string{};
}
void DeactivateOverlay() {
    g_hintDismissRequested.store(true, std::memory_order_release);
    g_hintVisible.store(false, std::memory_order_release);
    SetUiVisible(false);
}
} // namespace gamespeed::runtime
