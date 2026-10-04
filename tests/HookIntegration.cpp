#include <gamespeed/Client.hpp>
#include <windows.h>
#include <gl/GL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// This owned fixture exposes the native boundary rather than assuming that
// all executable QPC readers belong to simulation. Production accepts these
// exports only for the integration-test executable's exact basename.
struct GameSpeedTestTimer {
    float rate = 20.f;                         // 0x00
    std::int32_t ticks = 0;                    // 0x04
    float alpha = 0.f;                         // 0x08
    float timeScale = 1.f;                     // 0x0c
    float carry = 0.f;                         // 0x10
    float unused14 = 0.f;                     // 0x14
    float currentTime = 0.f;                   // 0x18
    float elapsedDelta = 0.f;                  // 0x1c
    double unused20 = 0.;                     // 0x20
    double timestamp28 = 0.;                  // 0x28
    double timestamp30 = 0.;                  // 0x30
    float unused38 = 0.f;                     // 0x38
    float stepping = -1.f;                    // 0x3c
    std::int64_t lastRawQpc = 0;
    double simulatedSeconds = 0.;
    std::int64_t frequency = 0;
};
static_assert(offsetof(GameSpeedTestTimer, rate) == 0x00);
static_assert(offsetof(GameSpeedTestTimer, ticks) == 0x04);
static_assert(offsetof(GameSpeedTestTimer, timeScale) == 0x0c);
static_assert(offsetof(GameSpeedTestTimer, carry) == 0x10);
static_assert(offsetof(GameSpeedTestTimer, currentTime) == 0x18);
static_assert(offsetof(GameSpeedTestTimer, elapsedDelta) == 0x1c);
static_assert(offsetof(GameSpeedTestTimer, timestamp28) == 0x28);
static_assert(offsetof(GameSpeedTestTimer, timestamp30) == 0x30);
static_assert(offsetof(GameSpeedTestTimer, stepping) == 0x3c);
static volatile LONG simulationWrapperCalls = 0;
static volatile LONG realWrapperCalls = 0;

extern "C" __declspec(dllexport) __declspec(noinline)
void __fastcall GameSpeedTestAdvanceTimer(void* object, float) {
    auto& timer = *static_cast<GameSpeedTestTimer*>(object);
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    if (timer.frequency <= 0) {
        QueryPerformanceFrequency(&frequency);
        timer.frequency = frequency.QuadPart;
    }
    double elapsed = timer.lastRawQpc && timer.frequency > 0 ?
        static_cast<double>(now.QuadPart - timer.lastRawQpc) / static_cast<double>(timer.frequency) : 0.;
    // Update the wall-clock anchor even with scale zero. A resumed simulation
    // should not consume the wall time spent paused as a backlog of ticks.
    timer.lastRawQpc = now.QuadPart;
    timer.timestamp28 = timer.timestamp30;
    timer.timestamp30 = timer.frequency > 0 ? static_cast<double>(now.QuadPart) / timer.frequency : 0.;
    elapsed = std::clamp(elapsed, 0., .1);
    const double scaled = elapsed * static_cast<double>(timer.timeScale);
    timer.elapsedDelta = static_cast<float>(scaled);
    timer.currentTime += timer.elapsedDelta;
    timer.simulatedSeconds += scaled;
    timer.carry += static_cast<float>(scaled * static_cast<double>(timer.rate));
    timer.ticks = static_cast<std::int32_t>(std::floor(timer.carry));
    timer.carry -= static_cast<float>(timer.ticks);
    timer.alpha = timer.carry;
}

extern "C" __declspec(dllexport) __declspec(noinline)
void __fastcall GameSpeedTestSimulationTimer(void* timer, float argument) {
    GameSpeedTestAdvanceTimer(timer, argument);
    InterlockedIncrement(&simulationWrapperCalls);
}

extern "C" __declspec(dllexport) __declspec(noinline)
void __fastcall GameSpeedTestRealTimer(void* timer, float argument) {
    GameSpeedTestAdvanceTimer(timer, argument);
    InterlockedIncrement(&realWrapperCalls);
}

namespace {
struct SharedState {
    volatile LONG quit = 0;
    volatile LONG regressions = 0;
    volatile LONG stateFailures = 0;
    volatile LONG frames = 0;
    alignas(8) volatile LONG64 pixelHash = 0;
    alignas(8) volatile LONG64 lastQpc = 0;
    alignas(8) volatile LONG64 lastChrono = 0;
    alignas(8) volatile LONG64 simulationCounter = 0;
    alignas(8) volatile LONG64 realCounter = 0;
    volatile LONG deadlineFailures = 0;
    volatile LONG readbackError = 0;
    volatile LONG swapError = 0;
    volatile LONG captureRequest = 0;
    volatile LONG initializationError = 0;
    volatile LONG contextMajor = 0;
    volatile LONG contextMinor = 0;
    volatile LONG contextProfile = 0;
    volatile LONG stateFailureMask = 0;
    volatile LONG windowThread = 0;
    volatile LONG renderThread = 0;
    volatile LONG foregroundFixtureReady = 0;
    volatile LONG cursorCheckSerial = 0;
    volatile LONG cursorVisible = 0;
    volatile LONG cursorCounter = 0;
    volatile LONG cursorApiFailures = 0;
    volatile LONG gameMouseActivations = 0;
    volatile LONG gameButtonDowns = 0;
    volatile LONG activationResult = 0;
    volatile LONG chordSerial = 0;
    alignas(8) volatile LONG64 window = 0;
};
constexpr int Width = 640, Height = 480;

SharedState* childShared = nullptr;
PVOID volatile mockForeground = nullptr;
PVOID volatile* foregroundImport = nullptr;
PVOID originalForegroundImport = nullptr;
bool gameCursorHidden = false;

HWND WINAPI TestForegroundWindow() {
    return static_cast<HWND>(InterlockedCompareExchangePointer(&mockForeground, nullptr, nullptr));
}

bool ReplaceForegroundImport(HWND window) {
    // Replace only this dependency in the injected DLL inside our owned child.
    // No SetForegroundWindow, SetFocus, AttachThreadInput, or desktop input is
    // used: the production CapturesInput path sees a controlled hidden HWND.
    const HMODULE hook = GetModuleHandleW(L"gamespeed_hook.dll");
    if (!hook) return false;
    auto* base = reinterpret_cast<unsigned char*>(hook);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* functions = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (std::size_t index = 0; names[index].u1.AddressOfData; ++index) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[index].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), "GetForegroundWindow") != 0) continue;
            auto* slot = reinterpret_cast<PVOID volatile*>(&functions[index].u1.Function);
            DWORD protection = 0;
            if (!VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), PAGE_READWRITE, &protection))
                return false;
            InterlockedExchangePointer(&mockForeground, window);
            originalForegroundImport = InterlockedExchangePointer(slot, reinterpret_cast<PVOID>(&TestForegroundWindow));
            foregroundImport = slot;
            DWORD ignored = 0;
            VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), protection, &ignored);
            // Model the gameplay cursor mode before the panel is opened.
            SetCursor(nullptr);
            ShowCursor(FALSE);
            gameCursorHidden = true;
            return true;
        }
    }
    return false;
}

void RestoreForegroundImport() {
    if (!foregroundImport) return;
    DWORD protection = 0;
    if (VirtualProtect(const_cast<PVOID*>(foregroundImport), sizeof(PVOID), PAGE_READWRITE, &protection)) {
        InterlockedExchangePointer(foregroundImport, originalForegroundImport);
        DWORD ignored = 0;
        VirtualProtect(const_cast<PVOID*>(foregroundImport), sizeof(PVOID), protection, &ignored);
    }
    foregroundImport = nullptr;
}

// Windows' OpenGL header exposes only 1.1. Load just the modern entry points
// used by this test, independently of the DLL's renderer and GL loader.
constexpr GLenum GlMajorVersion = 0x821B, GlMinorVersion = 0x821C;
constexpr GLenum GlContextProfileMask = 0x9126;
constexpr GLint GlCoreProfileBit = 0x00000001;
constexpr GLenum GlArrayBuffer = 0x8892, GlElementArrayBuffer = 0x8893;
constexpr GLenum GlArrayBufferBinding = 0x8894, GlElementArrayBufferBinding = 0x8895;
constexpr GLenum GlStaticDraw = 0x88E4, GlVertexArrayBinding = 0x85B5;
constexpr GLenum GlVertexShader = 0x8B31, GlFragmentShader = 0x8B30;
constexpr GLenum GlCompileStatus = 0x8B81, GlLinkStatus = 0x8B82, GlInfoLogLength = 0x8B84;
constexpr GLenum GlCurrentProgram = 0x8B8D, GlActiveTexture = 0x84E0, GlTexture0 = 0x84C0;
constexpr GLenum GlFramebuffer = 0x8D40, GlReadFramebuffer = 0x8CA8, GlDrawFramebuffer = 0x8CA9;
constexpr GLenum GlReadFramebufferBinding = 0x8CAA, GlDrawFramebufferBinding = 0x8CA6;
constexpr GLenum GlColorAttachment0 = 0x8CE0, GlFramebufferComplete = 0x8CD5;
constexpr GLenum GlBlendSrcRgb = 0x80C9, GlBlendDstRgb = 0x80C8;
constexpr GLenum GlBlendSrcAlpha = 0x80CB, GlBlendDstAlpha = 0x80CA;
constexpr GLenum GlBlendEquationRgb = 0x8009, GlBlendEquationAlpha = 0x883D;
constexpr GLenum GlFuncAdd = 0x8006, GlFuncReverseSubtract = 0x800B;

template<typename Function>
Function LoadGl(const char* name) {
    const auto address = wglGetProcAddress(name);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (!address || value <= 3 || value == ~std::uintptr_t{0})
        throw std::runtime_error(std::string("Missing OpenGL entry point: ") + name);
    return reinterpret_cast<Function>(address);
}

struct CoreGl {
    using GenObjects = void(APIENTRY*)(GLsizei, GLuint*);
    using DeleteObjects = void(APIENTRY*)(GLsizei, const GLuint*);
    using BindObject = void(APIENTRY*)(GLuint);
    using BindTarget = void(APIENTRY*)(GLenum, GLuint);
    using BufferData = void(APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
    using ShaderSource = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
    using CreateShader = GLuint(APIENTRY*)(GLenum);
    using CreateProgram = GLuint(APIENTRY*)();
    using GetObject = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using ObjectLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
    using AttachShader = void(APIENTRY*)(GLuint, GLuint);
    using ActiveTexture = void(APIENTRY*)(GLenum);
    using FramebufferTexture = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using CheckFramebuffer = GLenum(APIENTRY*)(GLenum);
    using BlendFuncSeparate = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
    using BlendEquationSeparate = void(APIENTRY*)(GLenum, GLenum);

    GenObjects genVertexArrays = LoadGl<GenObjects>("glGenVertexArrays");
    BindObject bindVertexArray = LoadGl<BindObject>("glBindVertexArray");
    DeleteObjects deleteVertexArrays = LoadGl<DeleteObjects>("glDeleteVertexArrays");
    GenObjects genBuffers = LoadGl<GenObjects>("glGenBuffers");
    BindTarget bindBuffer = LoadGl<BindTarget>("glBindBuffer");
    BufferData bufferData = LoadGl<BufferData>("glBufferData");
    DeleteObjects deleteBuffers = LoadGl<DeleteObjects>("glDeleteBuffers");
    CreateShader createShader = LoadGl<CreateShader>("glCreateShader");
    ShaderSource shaderSource = LoadGl<ShaderSource>("glShaderSource");
    BindObject compileShader = LoadGl<BindObject>("glCompileShader");
    GetObject getShader = LoadGl<GetObject>("glGetShaderiv");
    ObjectLog shaderLog = LoadGl<ObjectLog>("glGetShaderInfoLog");
    BindObject deleteShader = LoadGl<BindObject>("glDeleteShader");
    CreateProgram createProgram = LoadGl<CreateProgram>("glCreateProgram");
    AttachShader attachShader = LoadGl<AttachShader>("glAttachShader");
    BindObject linkProgram = LoadGl<BindObject>("glLinkProgram");
    GetObject getProgram = LoadGl<GetObject>("glGetProgramiv");
    ObjectLog programLog = LoadGl<ObjectLog>("glGetProgramInfoLog");
    BindObject useProgram = LoadGl<BindObject>("glUseProgram");
    BindObject deleteProgram = LoadGl<BindObject>("glDeleteProgram");
    ActiveTexture activeTexture = LoadGl<ActiveTexture>("glActiveTexture");
    GenObjects genFramebuffers = LoadGl<GenObjects>("glGenFramebuffers");
    BindTarget bindFramebuffer = LoadGl<BindTarget>("glBindFramebuffer");
    FramebufferTexture framebufferTexture = LoadGl<FramebufferTexture>("glFramebufferTexture2D");
    CheckFramebuffer checkFramebuffer = LoadGl<CheckFramebuffer>("glCheckFramebufferStatus");
    DeleteObjects deleteFramebuffers = LoadGl<DeleteObjects>("glDeleteFramebuffers");
    BlendFuncSeparate blendFunc = LoadGl<BlendFuncSeparate>("glBlendFuncSeparate");
    BlendEquationSeparate blendEquation = LoadGl<BlendEquationSeparate>("glBlendEquationSeparate");

    GLuint Shader(GLenum type, const char* source) const {
        const GLuint shader = createShader(type);
        shaderSource(shader, 1, &source, nullptr);
        compileShader(shader);
        GLint compiled = GL_FALSE;
        getShader(shader, GlCompileStatus, &compiled);
        if (compiled != GL_TRUE) {
            GLint length = 0;
            getShader(shader, GlInfoLogLength, &length);
            std::string log(static_cast<std::size_t>(length > 0 ? length : 1), '\0');
            shaderLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            deleteShader(shader);
            throw std::runtime_error("Core test shader compilation failed: " + log);
        }
        return shader;
    }
};

struct GameGlState {
    GLuint program = 0, vertexArray = 0, vertexBuffer = 0, indexBuffer = 0;
    GLuint framebuffer = 0, texture = 0;

    void Initialize(const CoreGl& gl, int width, int height) {
        const GLuint vertex = gl.Shader(GlVertexShader,
            "#version 150 core\nvoid main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}");
        const GLuint fragment = gl.Shader(GlFragmentShader,
            "#version 150 core\nout vec4 color;void main(){color=vec4(1.0);}");
        program = gl.createProgram();
        gl.attachShader(program, vertex);
        gl.attachShader(program, fragment);
        gl.linkProgram(program);
        gl.deleteShader(vertex);
        gl.deleteShader(fragment);
        GLint linked = GL_FALSE;
        gl.getProgram(program, GlLinkStatus, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            gl.getProgram(program, GlInfoLogLength, &length);
            std::string log(static_cast<std::size_t>(length > 0 ? length : 1), '\0');
            gl.programLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
            throw std::runtime_error("Core test program linking failed: " + log);
        }
        gl.genVertexArrays(1, &vertexArray);
        gl.bindVertexArray(vertexArray);
        gl.genBuffers(1, &vertexBuffer);
        gl.bindBuffer(GlArrayBuffer, vertexBuffer);
        constexpr GLfloat vertices[] = {0.0f, 0.0f, 0.0f};
        gl.bufferData(GlArrayBuffer, sizeof(vertices), vertices, GlStaticDraw);
        gl.genBuffers(1, &indexBuffer);
        gl.bindBuffer(GlElementArrayBuffer, indexBuffer);
        constexpr GLuint index = 0;
        gl.bufferData(GlElementArrayBuffer, sizeof(index), &index, GlStaticDraw);
        gl.activeTexture(GlTexture0 + 3);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.genFramebuffers(1, &framebuffer);
        gl.bindFramebuffer(GlFramebuffer, framebuffer);
        gl.framebufferTexture(GlFramebuffer, GlColorAttachment0, GL_TEXTURE_2D, texture, 0);
        if (gl.checkFramebuffer(GlFramebuffer) != GlFramebufferComplete)
            throw std::runtime_error("Core test framebuffer is incomplete");
        gl.bindFramebuffer(GlFramebuffer, 0);
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error("Core test GL object initialization failed");
    }

    void Apply(const CoreGl& gl, int width, int height) const {
        gl.useProgram(program);
        gl.bindVertexArray(vertexArray);
        gl.bindBuffer(GlArrayBuffer, vertexBuffer);
        gl.bindBuffer(GlElementArrayBuffer, indexBuffer);
        gl.activeTexture(GlTexture0 + 3);
        glBindTexture(GL_TEXTURE_2D, texture);
        gl.bindFramebuffer(GlFramebuffer, framebuffer);
        glViewport(7, 11, width - 24, height - 28);
        glScissor(13, 17, width / 3, height / 3);
        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        gl.blendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA);
        gl.blendEquation(GlFuncReverseSubtract, GlFuncAdd);
    }

    LONG Check(int width, int height) const {
        LONG failures = 0;
        auto matches = [](GLenum query, GLint expected) {
            GLint actual = 0;
            glGetIntegerv(query, &actual);
            return actual == expected;
        };
        if (!matches(GlCurrentProgram, static_cast<GLint>(program))) failures |= 1;
        if (!matches(GlVertexArrayBinding, static_cast<GLint>(vertexArray))) failures |= 2;
        if (!matches(GlArrayBufferBinding, static_cast<GLint>(vertexBuffer)) ||
            !matches(GlElementArrayBufferBinding, static_cast<GLint>(indexBuffer))) failures |= 4;
        if (!matches(GlActiveTexture, static_cast<GLint>(GlTexture0 + 3)) ||
            !matches(GL_TEXTURE_BINDING_2D, static_cast<GLint>(texture))) failures |= 8;
        if (!matches(GlDrawFramebufferBinding, static_cast<GLint>(framebuffer)) ||
            !matches(GlReadFramebufferBinding, static_cast<GLint>(framebuffer))) failures |= 16;
        std::array<GLint, 4> viewport{}, scissor{};
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
        if (viewport != std::array<GLint, 4>{7, 11, width - 24, height - 28} ||
            scissor != std::array<GLint, 4>{13, 17, width / 3, height / 3}) failures |= 32;
        GLboolean depthMask = GL_FALSE;
        std::array<GLboolean, 4> colorMask{};
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask.data());
        if (!glIsEnabled(GL_DEPTH_TEST) || !glIsEnabled(GL_CULL_FACE) || glIsEnabled(GL_BLEND) ||
            !glIsEnabled(GL_SCISSOR_TEST) || depthMask != GL_TRUE ||
            colorMask != std::array<GLboolean, 4>{GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE}) failures |= 64;
        if (!matches(GlBlendSrcRgb, GL_DST_COLOR) || !matches(GlBlendDstRgb, GL_ONE_MINUS_SRC_ALPHA) ||
            !matches(GlBlendSrcAlpha, GL_ONE_MINUS_DST_ALPHA) || !matches(GlBlendDstAlpha, GL_SRC_ALPHA) ||
            !matches(GlBlendEquationRgb, GlFuncReverseSubtract) || !matches(GlBlendEquationAlpha, GlFuncAdd)) failures |= 128;
        return failures;
    }

    void Destroy(const CoreGl& gl) const {
        gl.useProgram(0);
        gl.bindFramebuffer(GlFramebuffer, 0);
        gl.bindVertexArray(0);
        gl.bindBuffer(GlArrayBuffer, 0);
        gl.deleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        gl.deleteBuffers(1, &indexBuffer);
        gl.deleteBuffers(1, &vertexBuffer);
        gl.deleteVertexArrays(1, &vertexArray);
        gl.deleteProgram(program);
    }
};

void SavePreview(int width, int height, const std::vector<unsigned char>& pixels, LONG variant, bool compact) {
    std::array<wchar_t, 32768> self{};
    GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    const wchar_t* filename = variant == 5 ? (compact ? L"overlay-startup-hint-compact-preview.bmp" : L"overlay-startup-hint-preview.bmp") :
        compact ? L"overlay-compact-preview.bmp" : variant == 4 ? L"overlay-overview-preview.bmp" :
        variant == 2 ? L"overlay-paused-preview.bmp" : variant == 3 ? L"overlay-fast-preview.bmp" : L"overlay-preview.bmp";
    const auto path = std::filesystem::path(self.data()).parent_path() / filename;
    const auto rowBytes = static_cast<std::uint32_t>((width * 3 + 3) & ~3);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + rowBytes * height;
    BITMAPINFOHEADER bitmap{};
    bitmap.biSize = sizeof(bitmap);
    bitmap.biWidth = width;
    bitmap.biHeight = height;
    bitmap.biPlanes = 1;
    bitmap.biBitCount = 24;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
    std::vector<char> row(rowBytes);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>((y * width + x) * 3);
            row[x * 3] = static_cast<char>(pixels[offset + 2]);
            row[x * 3 + 1] = static_cast<char>(pixels[offset + 1]);
            row[x * 3 + 2] = static_cast<char>(pixels[offset]);
        }
        output.write(row.data(), row.size());
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP + 9 && foregroundImport) {
        BYTE original[256]{}, chord[256]{};
        GetKeyboardState(original);
        std::memcpy(chord, original, sizeof(chord));
        chord[VK_CONTROL] = chord[VK_SHIFT] = 0x80;
        SetKeyboardState(chord);
        // Dispatch on this owner thread without another GetMessage restoring
        // the posted keyboard message's original modifier snapshot.
        SendMessageW(window, WM_KEYDOWN, 'G', 1);
        chord[VK_CONTROL] = chord[VK_SHIFT] = 0;
        SetKeyboardState(chord);
        SendMessageW(window, WM_KEYUP, 'G', (LPARAM(1) << 31) | 1);
        SetKeyboardState(original);
        InterlockedIncrement(&childShared->chordSerial);
        return 0;
    }
    if (message == WM_APP + 7 && foregroundImport) {
        SendMessageW(window, WM_APP + 4, FALSE, 0);
        // Windows asks about the activation click before SETFOCUS. The game
        // must not see it, even while the foreground/focus state is changing.
        const auto result = SendMessageW(window, WM_MOUSEACTIVATE,
            reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        InterlockedExchange(&childShared->activationResult, static_cast<LONG>(result));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        // Also exercise the gap where foreground ownership has returned but
        // the queued focus notification has not run yet.
        InterlockedExchangePointer(&mockForeground, window);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        SendMessageW(window, WM_APP + 4, TRUE, 0);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        return 0;
    }
    if (message == WM_APP + 8 && foregroundImport) {
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        return 0;
    }
    if (foregroundImport && message == WM_MOUSEACTIVATE) {
        InterlockedIncrement(&childShared->gameMouseActivations);
        SetCursor(nullptr); // Model the game's click-to-return-to-HUD path.
        return MA_ACTIVATE;
    }
    if (foregroundImport && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)) {
        InterlockedIncrement(&childShared->gameButtonDowns);
        SetCursor(nullptr);
        return 0;
    }
    if (message == WM_APP + 3) {
        const bool ready = foregroundImport || ReplaceForegroundImport(window);
        InterlockedExchange(&childShared->foregroundFixtureReady, ready ? 1 : -1);
        return 0;
    }
    if (message == WM_APP + 4 && foregroundImport) {
        InterlockedExchangePointer(&mockForeground, wparam ? window : nullptr);
        SendMessageW(window, WM_ACTIVATEAPP, wparam ? TRUE : FALSE, 0);
        SendMessageW(window, WM_ACTIVATE, wparam ? WA_ACTIVE : WA_INACTIVE, 0);
        SendMessageW(window, wparam ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        return 0;
    }
    if (message == WM_APP + 5 || message == WM_APP + 6) {
        if (message == WM_APP + 6) {
            // Model the game's late cursor-mode update without giving the
            // posted repair message a chance to mask a missing API hook.
            const HCURSOR before = GetCursor();
            const HCURSOR previous = SetCursor(nullptr);
            if (!GetCursor() || previous != before)
                InterlockedIncrement(&childShared->cursorApiFailures);
            const int beforeCount = ShowCursor(TRUE) - 1;
            ShowCursor(FALSE);
            const int hiddenCount = ShowCursor(FALSE);
            const int restoredCount = ShowCursor(TRUE);
            if (hiddenCount != beforeCount - 1 || restoredCount != beforeCount)
                InterlockedIncrement(&childShared->cursorApiFailures);
        }
        const bool visible = GetCursor() != nullptr;
        // ShowCursor belongs to this isolated window owner's input queue.
        // Read its count with one immediately balanced probe.
        const int probed = ShowCursor(TRUE);
        ShowCursor(FALSE);
        // ShowCursor now returns the game's logical count. The physical count
        // invariant and hide-until-negative loops have a separate unit test.
        InterlockedExchange(&childShared->cursorVisible, visible ? 1 : 0);
        InterlockedExchange(&childShared->cursorCounter, probed - 1);
        InterlockedIncrement(&childShared->cursorCheckSerial);
        return 0;
    }
    if (foregroundImport && message == WM_SETFOCUS) {
        // GLFW re-applies its disabled cursor after the subclass forwards this
        // message. This was the regression: buttons worked, cursor stayed null.
        SetCursor(nullptr);
        if (!gameCursorHidden) {
            ShowCursor(FALSE);
            gameCursorHidden = true;
        }
        return 0;
    }
    if (foregroundImport && message == WM_KILLFOCUS) {
        if (gameCursorHidden) {
            ShowCursor(TRUE);
            gameCursorHidden = false;
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    }
    if (foregroundImport && (message == WM_ACTIVATEAPP || message == WM_ACTIVATE))
        return 0; // Avoid DefWindowProc's focus changes for synthetic activation.
    if (message == WM_APP + 2) {
        // SetKeyboardState affects only this isolated HWND owner's input table.
        // Exercise modifier handling without touching foreground/system input.
        BYTE keys[256]{};
        GetKeyboardState(keys);
        keys[VK_CONTROL] = wparam ? 0x80 : 0;
        keys[VK_SHIFT] = wparam ? 0x80 : 0;
        SetKeyboardState(keys);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int Child(const wchar_t* mappingName, bool compact, bool overview) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (!mapping) return 2;
    auto* shared = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) { CloseHandle(mapping); return 3; }
    childShared = shared;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.style = CS_OWNDC;
    type.lpfnWndProc = WindowProc;
    type.hInstance = instance;
    type.lpszClassName = L"GameSpeed.Isolated.OpenGL.Test";
    RegisterClassW(&type);
    const HWND window = CreateWindowExW(0, type.lpszClassName, L"GameSpeed isolated test", WS_OVERLAPPEDWINDOW,
        0, 0, (compact ? 420 : overview ? 960 : Width) + 16,
        (compact ? 360 : overview ? 720 : Height) + 39, nullptr, nullptr, instance, nullptr);
    if (!window) return 4;
    InterlockedExchange64(&shared->window, reinterpret_cast<LONG64>(window));
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    const auto pixelFormat = ChoosePixelFormat(dc, &format);
    if (!pixelFormat || !SetPixelFormat(dc, pixelFormat, &format)) return 5;
    // Bootstrap WGL extension lookup, then discard the legacy context before
    // testing. The overlay must work in a real desktop OpenGL core context.
    const HGLRC bootstrap = wglCreateContext(dc);
    if (!bootstrap || !wglMakeCurrent(dc, bootstrap)) return 6;
    using CreateContext = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    HGLRC context = nullptr;
    try {
        const auto createContext = LoadGl<CreateContext>("wglCreateContextAttribsARB");
        constexpr int attributes[] = {
            0x2091, 3, // WGL_CONTEXT_MAJOR_VERSION_ARB
            0x2092, 2, // WGL_CONTEXT_MINOR_VERSION_ARB
            0x9126, 1, // WGL_CONTEXT_PROFILE_MASK_ARB, CORE_PROFILE_BIT_ARB
            0
        };
        context = createContext(dc, nullptr, attributes);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(bootstrap);
    if (!context) return 7;
    // Keep the test window hidden and leave the user's game and focus alone.
    // Window messages and rendering run on different threads as in the game.
    InterlockedExchange(&shared->windowThread, static_cast<LONG>(GetCurrentThreadId()));
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 3));
    // Exercise an executable's MSVC steady_clock wrapper as a real-time clock.
    const HMODULE standardLibrary = LoadLibraryW(L"msvcp140.dll");
    using PerfCounter = std::int64_t(__cdecl*)();
    const auto chronoCounter = standardLibrary ? reinterpret_cast<PerfCounter>(GetProcAddress(standardLibrary, "_Query_perf_counter")) : nullptr;
    std::thread renderThread([&] {
      InterlockedExchange(&shared->renderThread, static_cast<LONG>(GetCurrentThreadId()));
      if (!wglMakeCurrent(dc, context)) {
        InterlockedExchange(&shared->initializationError, 1);
        return;
      }
      try {
      GLint major = 0, minor = 0, profile = 0;
      glGetIntegerv(GlMajorVersion, &major);
      glGetIntegerv(GlMinorVersion, &minor);
      glGetIntegerv(GlContextProfileMask, &profile);
      InterlockedExchange(&shared->contextMajor, major);
      InterlockedExchange(&shared->contextMinor, minor);
      InterlockedExchange(&shared->contextProfile, profile);
      if (major != 3 || minor != 2 || (profile & GlCoreProfileBit) == 0)
        throw std::runtime_error("WGL did not create the requested OpenGL 3.2 core context");
      const CoreGl gl;
      GameGlState gameState;
      gameState.Initialize(gl, width, height);
      LARGE_INTEGER before{};
      QueryPerformanceCounter(&before);
      LARGE_INTEGER frequency{};
      QueryPerformanceFrequency(&frequency);
      GameSpeedTestTimer simulationTimer;
      GameSpeedTestTimer realTimer;
      while (!InterlockedCompareExchange(&shared->quit, 0, 0)) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // The executable's render cadence really depends on QPC. Freezing all
        // executable readers would prevent this deadline from ever arriving.
        // An independent OS watchdog converts that mistake into a bounded
        // regression failure instead of leaving a hung test subprocess.
        const auto frameDeadline = now.QuadPart + std::max<std::int64_t>(1, frequency.QuadPart / 500);
        const auto watchdog = GetTickCount64() + 250;
        do {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= frameDeadline) break;
            if (GetTickCount64() >= watchdog) {
                InterlockedIncrement(&shared->deadlineFailures);
                break;
            }
            Sleep(1);
        } while (!InterlockedCompareExchange(&shared->quit, 0, 0));
        InterlockedExchange64(&shared->lastQpc, now.QuadPart);
        if (chronoCounter) InterlockedExchange64(&shared->lastChrono, chronoCounter());
        if (now.QuadPart < before.QuadPart) InterlockedIncrement(&shared->regressions);
        if (before.QuadPart < now.QuadPart) before = now;
        // SIM and REAL share the render thread and the same native target;
        // their caller boundaries, rather than their thread IDs, differ.
        GameSpeedTestSimulationTimer(&simulationTimer, 0.f);
        GameSpeedTestRealTimer(&realTimer, 0.f);
        InterlockedExchange64(&shared->simulationCounter,
            static_cast<LONG64>(simulationTimer.simulatedSeconds * static_cast<double>(frequency.QuadPart)));
        InterlockedExchange64(&shared->realCounter,
            static_cast<LONG64>(realTimer.simulatedSeconds * static_cast<double>(frequency.QuadPart)));
        gl.bindFramebuffer(GlFramebuffer, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glViewport(0, 0, width, height);
        glClearColor(0.04f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        // Simulate game state that would suppress a flat UI if not isolated,
        // and verify the detour leaves the following game frame unaffected.
        gameState.Apply(gl, width, height);
        SwapBuffers(dc);
        const auto swapError = glGetError();
        if (swapError != GL_NO_ERROR) InterlockedExchange(&shared->swapError, static_cast<LONG>(swapError));
        const LONG stateFailure = gameState.Check(width, height);
        if (stateFailure) {
            InterlockedIncrement(&shared->stateFailures);
            InterlockedOr(&shared->stateFailureMask, stateFailure);
        }
        const auto stateError = glGetError();
        if (stateError != GL_NO_ERROR) InterlockedExchange(&shared->swapError, static_cast<LONG>(stateError));
        gl.bindFramebuffer(GlReadFramebuffer, 0);
        glReadBuffer(GL_FRONT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        const auto preparationError = glGetError();
        if (preparationError != GL_NO_ERROR) InterlockedExchange(&shared->readbackError, static_cast<LONG>(preparationError));
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        const GLenum readbackError = glGetError();
        if (readbackError != GL_NO_ERROR) InterlockedExchange(&shared->readbackError, static_cast<LONG>(readbackError));
        std::uint64_t hash = 1469598103934665603ULL;
        for (unsigned char pixel : pixels) { hash ^= pixel; hash *= 1099511628211ULL; }
        InterlockedExchange64(&shared->pixelHash, static_cast<LONG64>(hash));
        const LONG captureVariant = InterlockedCompareExchange(&shared->captureRequest, 0, 0);
        if (captureVariant) {
            SavePreview(width, height, pixels, captureVariant, compact);
            InterlockedExchange(&shared->captureRequest, 0);
        }
        InterlockedIncrement(&shared->frames);
      }
      gameState.Destroy(gl);
      } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        InterlockedExchange(&shared->initializationError, 2);
      }
      wglMakeCurrent(nullptr, nullptr);
    });
    while (!InterlockedCompareExchange(&shared->quit, 0, 0)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(1);
    }
    renderThread.join();
    if (foregroundImport) {
        SendMessageW(window, WM_APP + 4, FALSE, 0);
        RestoreForegroundImport();
    }
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    if (standardLibrary) FreeLibrary(standardLibrary);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 0;
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
LONG CheckOwnerCursor(HWND window, SharedState* shared, bool lateHide = false) {
    const LONG previous = InterlockedCompareExchange(&shared->cursorCheckSerial, 0, 0);
    Require(PostMessageW(window, lateHide ? WM_APP + 6 : WM_APP + 5, 0, 0) != FALSE,
        "Cannot request owner-thread cursor check");
    const auto deadline = GetTickCount64() + 2000;
    while (InterlockedCompareExchange(&shared->cursorCheckSerial, 0, 0) == previous && GetTickCount64() < deadline)
        Sleep(5);
    Require(InterlockedCompareExchange(&shared->cursorCheckSerial, 0, 0) != previous,
        "Owner-thread cursor check did not finish");
    Require(InterlockedCompareExchange(&shared->cursorVisible, 0, 0) != 0,
        "UI cursor remained hidden after the game's focus handler");
    Require(InterlockedCompareExchange(&shared->cursorApiFailures, 0, 0) == 0,
        "A late game SetCursor call hid the UI pointer or changed the previous-handle return value");
    return InterlockedCompareExchange(&shared->cursorCounter, 0, 0);
}
std::int64_t Number(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    const auto index = json.find(marker);
    Require(index != std::string::npos, "Missing response field: " + key + ": " + json);
    std::int64_t value = 0;
    const auto* start = json.data() + index + marker.size();
    const auto parsed = std::from_chars(start, json.data() + json.size(), value);
    Require(parsed.ec == std::errc{}, "Invalid response field: " + key);
    return value;
}
std::string Command(DWORD pid, const std::string& command) {
    const auto response = gamespeed::SendCommand(pid, command);
    Require(response.ok, command + " failed: " + response.message);
    return response.message;
}
void WaitForFrames(SharedState* shared, LONG minimum) {
    const auto deadline = GetTickCount64() + 2000;
    while (InterlockedCompareExchange(&shared->frames, 0, 0) < minimum && GetTickCount64() < deadline)
        Sleep(5);
    Require(InterlockedCompareExchange(&shared->deadlineFailures, 0, 0) == 0,
        "Executable QPC render deadline stalled; simulation pause affected real-time scheduling");
    Require(InterlockedCompareExchange(&shared->frames, 0, 0) >= minimum,
        "Render frames stopped while waiting for a native timer update");
}
double Slope(DWORD pid, SharedState* shared, const std::string& speed) {
    Command(pid, "set " + speed);
    WaitForFrames(shared, InterlockedCompareExchange(&shared->frames, 0, 0) + 2);
    const auto childBefore = InterlockedCompareExchange64(&shared->lastQpc, 0, 0);
    const auto chronoBefore = InterlockedCompareExchange64(&shared->lastChrono, 0, 0);
    const auto simulationBefore = InterlockedCompareExchange64(&shared->simulationCounter, 0, 0);
    const auto realBefore = InterlockedCompareExchange64(&shared->realCounter, 0, 0);
    LARGE_INTEGER parentBefore{}, parentAfter{};
    QueryPerformanceCounter(&parentBefore);
    const auto before = Command(pid, "status");
    Sleep(500);
    const auto after = Command(pid, "status");
    QueryPerformanceCounter(&parentAfter);
    const auto childAfter = InterlockedCompareExchange64(&shared->lastQpc, 0, 0);
    const auto chronoAfter = InterlockedCompareExchange64(&shared->lastChrono, 0, 0);
    const auto simulationAfter = InterlockedCompareExchange64(&shared->simulationCounter, 0, 0);
    const auto realAfter = InterlockedCompareExchange64(&shared->realCounter, 0, 0);
    const auto virtualDelta = Number(after, "virtualCounter") - Number(before, "virtualCounter");
    const auto realDelta = Number(after, "realCounter") - Number(before, "realCounter");
    Require(realDelta > 0, "Real QPC did not advance");
    const double childSlope = static_cast<double>(childAfter - childBefore) / static_cast<double>(parentAfter.QuadPart - parentBefore.QuadPart);
    const double expected = std::stod(speed);
    Require(std::abs(childSlope - 1.) < 0.18, "Main-EXE QPC was scaled instead of remaining at real time: " + std::to_string(childSlope));
    if (chronoBefore != 0 && chronoAfter != 0) {
        const double chronoSlope = static_cast<double>(chronoAfter - chronoBefore) / static_cast<double>(parentAfter.QuadPart - parentBefore.QuadPart);
        Require(std::abs(chronoSlope - 1.) < 0.18, "Main-EXE MSVC chrono wrapper was scaled: " + std::to_string(chronoSlope));
    }
    const auto parentDelta = static_cast<double>(parentAfter.QuadPart - parentBefore.QuadPart);
    const double simulationSlope = static_cast<double>(simulationAfter - simulationBefore) / parentDelta;
    const double realSlope = static_cast<double>(realAfter - realBefore) / parentDelta;
    Require(std::abs(simulationSlope - expected) < (expected > 1. ? .22 : .12),
        "Native SIM elapsed-time slope incorrect: " + std::to_string(simulationSlope));
    Require(std::abs(realSlope - 1.) < .18,
        "Native REAL timer was scaled with simulation: " + std::to_string(realSlope));
    Require(InterlockedCompareExchange(&shared->deadlineFailures, 0, 0) == 0,
        "QPC frame deadline failed during a speed change");
    return static_cast<double>(virtualDelta) / static_cast<double>(realDelta);
}
}

int wmain(int argc, wchar_t** argv) {
    if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--child")
        return Child(argv[2], argc == 4 && std::wstring_view(argv[3]) == L"--compact",
            argc == 4 && std::wstring_view(argv[3]) == L"--overview");
    const bool compact = argc == 3 && std::wstring_view(argv[2]) == L"--compact";
    const bool overview = argc == 3 && std::wstring_view(argv[2]) == L"--overview";
    if (argc != 2 && !compact && !overview) return 2;
    const auto mappingName = L"Local\\MCDK.GameSpeed.Test." + std::to_wstring(GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedState), mappingName.c_str());
    if (!mapping) return 3;
    auto* shared = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) { CloseHandle(mapping); return 4; }
    std::array<wchar_t, 32768> self{};
    GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    std::wstring childCommand = L"\"" + std::wstring(self.data()) + L"\" --child \"" + mappingName + L"\"";
    if (compact) childCommand += L" --compact";
    if (overview) childCommand += L" --overview";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(self.data(), childCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
        UnmapViewOfFile(shared); CloseHandle(mapping); return 5;
    }
    int exitCode = 0;
    try {
        for (int attempt = 0; attempt < 200 && InterlockedCompareExchange(&shared->frames, 0, 0) < 3; ++attempt) {
            if (InterlockedCompareExchange(&shared->initializationError, 0, 0) ||
                WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(10);
        }
        Require(InterlockedCompareExchange(&shared->initializationError, 0, 0) == 0,
            "Core OpenGL child initialization failed: " + std::to_string(shared->initializationError));
        Require(InterlockedCompareExchange(&shared->frames, 0, 0) >= 3, "Isolated OpenGL child failed to start");
        Require(shared->contextMajor == 3 && shared->contextMinor == 2,
            "Isolated OpenGL context must exercise the game's OpenGL 3.2 version");
        Require((shared->contextProfile & GlCoreProfileBit) != 0, "Isolated OpenGL context is not a core profile");
        Require(shared->windowThread != shared->renderThread, "Window and render test threads must differ");
        const auto baseline = InterlockedCompareExchange64(&shared->pixelHash, 0, 0);
        const auto attached = gamespeed::Inject(child.dwProcessId, argv[1]);
        Require(attached.ok, "Injection failed: " + attached.message);
        // A duplicate attach must reuse the existing worker and pipe.
        Require(gamespeed::Inject(child.dwProcessId, argv[1]).ok, "Duplicate injection did not reuse the DLL");
        std::string status;
        for (int attempt = 0; attempt < 100; ++attempt) {
            status = Command(child.dwProcessId, "status");
            if (status.find("\"overlayReady\":true") != std::string::npos &&
                status.find("\"nativeTickReady\":true") != std::string::npos) break;
            Sleep(20);
        }
        Require(status.find("\"overlayReady\":true") != std::string::npos, "RmlUi initialization failed: " + status);
        Require(status.find("\"nativeTickReady\":true") != std::string::npos,
            "Owned native Timer adapter did not initialize: " + status);
        Require(Number(status, "scaledCalls") > 0, "Native SIM timer calls were not adapted");
        Require(Number(status, "simulationTicks") >= 0 && Number(status, "realTicks") >= 0,
            "Native timer tick metrics are missing");
        Require(status.find("\"uiVisible\":false") != std::string::npos,
            "Injection opened the control panel instead of the startup hint");
        const HWND window = reinterpret_cast<HWND>(InterlockedCompareExchange64(&shared->window, 0, 0));
        Require(PostMessageW(window, WM_APP + 3, 0, 0) != FALSE, "Cannot install isolated foreground fixture");
        const auto fixtureDeadline = GetTickCount64() + 2000;
        while (InterlockedCompareExchange(&shared->foregroundFixtureReady, 0, 0) == 0 && GetTickCount64() < fixtureDeadline)
            Sleep(5);
        Require(InterlockedCompareExchange(&shared->foregroundFixtureReady, 0, 0) == 1,
            "Cannot replace injected DLL foreground import in owned test child");
        Sleep(350);
        Require(Command(child.dwProcessId, "status").find("\"startupHintVisible\":true") != std::string::npos,
            "The startup shortcut hint did not appear while the panel was hidden");
        Require(InterlockedCompareExchange64(&shared->pixelHash, 0, 0) != baseline,
            "The startup shortcut hint did not render");
        InterlockedExchange(&shared->captureRequest, 5);
        Sleep(120);
        if (!compact) {
            // The tutorial and render deadlines use real time while only the
            // simulation accumulator is paused, even on the same thread.
            const auto pausedHint = Command(child.dwProcessId, "pause");
            WaitForFrames(shared, InterlockedCompareExchange(&shared->frames, 0, 0) + 2);
            const auto hintSimulation = InterlockedCompareExchange64(&shared->simulationCounter, 0, 0);
            const auto hintReal = InterlockedCompareExchange64(&shared->realCounter, 0, 0);
            const auto hintFrames = InterlockedCompareExchange(&shared->frames, 0, 0);
            const auto hintTicks = Command(child.dwProcessId, "status");
            const ULONGLONG hintDeadline = GetTickCount64() + 10000;
            while (Command(child.dwProcessId, "status").find("\"startupHintVisible\":true") != std::string::npos &&
                GetTickCount64() < hintDeadline)
                Sleep(50);
            status = Command(child.dwProcessId, "status");
            Require(status.find("\"startupHintVisible\":false") != std::string::npos,
                "The startup hint did not expire while the game clock was paused");
            Require(status.find("\"uiVisible\":false") != std::string::npos,
                "Hint expiry opened the control panel");
            Require(Number(status, "virtualCounter") == Number(pausedHint, "virtualCounter"),
                "The tutorial countdown advanced the paused game clock");
            Require(InterlockedCompareExchange64(&shared->simulationCounter, 0, 0) == hintSimulation,
                "The native SIM accumulator advanced during the paused tutorial");
            Require(InterlockedCompareExchange64(&shared->realCounter, 0, 0) > hintReal &&
                InterlockedCompareExchange(&shared->frames, 0, 0) > hintFrames + 5,
                "Native REAL time or rendering stopped during the paused tutorial");
            Require(Number(status, "simulationTicks") == Number(hintTicks, "simulationTicks"),
                "Native SIM ticks accumulated while paused");
            Require(Number(status, "realTicks") > Number(hintTicks, "realTicks"),
                "Native REAL ticks did not advance while SIM was paused");
            Sleep(120);
            Require(InterlockedCompareExchange64(&shared->pixelHash, 0, 0) == baseline,
                "The startup hint left visible UI after its deadline");
            Command(child.dwProcessId, "resume");
        }
        Command(child.dwProcessId, "show");
        Sleep(450);
        Require(Command(child.dwProcessId, "status").find("\"startupHintVisible\":false") != std::string::npos,
            "Opening the panel did not dismiss the startup hint");
        Require(InterlockedCompareExchange64(&shared->pixelHash, 0, 0) != baseline, "RmlUi did not change the OpenGL framebuffer");
        const LONG initialCursorCounter = CheckOwnerCursor(window, shared);
        for (int attempt = 0; attempt < 3; ++attempt) {
            Require(PostMessageW(window, WM_APP + 7, 0, 0) != FALSE, "Cannot simulate click activation");
            Require(CheckOwnerCursor(window, shared) == initialCursorCounter,
                "Click activation changed UI cursor ownership");
            Require(InterlockedCompareExchange(&shared->activationResult, 0, 0) == MA_ACTIVATEANDEAT,
                "UI did not consume the click that reactivates the game window");
            Require(InterlockedCompareExchange(&shared->gameMouseActivations, 0, 0) == 0 &&
                InterlockedCompareExchange(&shared->gameButtonDowns, 0, 0) == 0,
                "A refocus click reached the game's HUD input handler");
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            Require(PostMessageW(window, WM_APP + 4, FALSE, 0) != FALSE, "Cannot simulate focus loss");
            Require(PostMessageW(window, WM_APP + 4, TRUE, 0) != FALSE, "Cannot simulate focus return");
            Require(CheckOwnerCursor(window, shared) == initialCursorCounter,
                "UI cursor display count accumulated across focus cycles");
            Require(CheckOwnerCursor(window, shared, true) == initialCursorCounter,
                "A late game cursor-mode update changed the cursor display count");
            Require(Command(child.dwProcessId, "status").find("\"uiVisible\":true") != std::string::npos,
                "Focus return closed the panel");
        }
        InterlockedExchange(&shared->captureRequest, 1);
        Sleep(100);
        Command(child.dwProcessId, "hide");
        Require(PostMessageW(window, WM_APP + 8, 0, 0) != FALSE,
            "Cannot verify gameplay input after hiding the panel");
        const auto inputReturnDeadline = GetTickCount64() + 2000;
        while (InterlockedCompareExchange(&shared->gameButtonDowns, 0, 0) == 0 &&
            GetTickCount64() < inputReturnDeadline)
            Sleep(5);
        Require(InterlockedCompareExchange(&shared->gameButtonDowns, 0, 0) == 1,
            "Hiding the panel did not return mouse input to gameplay");
        Require(PostMessageW(window, WM_KEYDOWN, VK_F8, 1) != FALSE, "Cannot deliver test F8 key");
        PostMessageW(window, WM_KEYUP, VK_F8, (LPARAM(1) << 31) | 1);
        Sleep(100);
        Require(Command(child.dwProcessId, "status").find("\"uiVisible\":true") != std::string::npos,
            "Window/render-thread input bootstrap failed: F8 did not reopen UI");
        for (int attempt = 0; attempt < 2; ++attempt) {
            Require(PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 1) != FALSE, "Cannot deliver Escape close key");
            PostMessageW(window, WM_KEYUP, VK_ESCAPE, (LPARAM(1) << 31) | 1);
            Sleep(100);
            Require(Command(child.dwProcessId, "status").find("\"uiVisible\":false") != std::string::npos,
                "Escape did not close the overlay");
            Require(Command(child.dwProcessId, "status").find("\"startupHintVisible\":false") != std::string::npos,
                "Closing the panel replayed the startup hint");
            Require(PostMessageW(window, WM_KEYDOWN, VK_F8, 1) != FALSE, "Cannot deliver F8 after Escape");
            PostMessageW(window, WM_KEYUP, VK_F8, (LPARAM(1) << 31) | 1);
            Sleep(100);
            Require(Command(child.dwProcessId, "status").find("\"uiVisible\":true") != std::string::npos,
                "F8 could not reopen the overlay after Escape");
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto chordSerial = InterlockedCompareExchange(&shared->chordSerial, 0, 0);
            Require(PostMessageW(window, WM_APP + 9, 0, 0) != FALSE, "Cannot dispatch isolated shortcut chord");
            const auto chordDeadline = GetTickCount64() + 2000;
            while (InterlockedCompareExchange(&shared->chordSerial, 0, 0) == chordSerial && GetTickCount64() < chordDeadline)
                Sleep(5);
            Require(InterlockedCompareExchange(&shared->chordSerial, 0, 0) > chordSerial,
                "Owner thread did not process the shortcut chord");
            Require(Command(child.dwProcessId, "status").find(attempt == 0 ? "\"uiVisible\":false" : "\"uiVisible\":true") != std::string::npos,
                "Ctrl+Shift+G could not toggle the overlay after Escape");
        }
        Require(PostMessageW(window, WM_KEYDOWN, VK_INSERT, 1) != FALSE, "Cannot deliver legacy Insert key");
        PostMessageW(window, WM_KEYUP, VK_INSERT, (LPARAM(1) << 31) | 1);
        Sleep(100);
        Require(Command(child.dwProcessId, "status").find("\"uiVisible\":false") != std::string::npos,
            "Legacy Insert toggle did not hide UI");
        Require(PostMessageW(window, WM_KEYDOWN, VK_F8, 1) != FALSE, "Cannot deliver F8 reopen key");
        PostMessageW(window, WM_KEYUP, VK_F8, (LPARAM(1) << 31) | 1);
        Sleep(100);
        Require(Command(child.dwProcessId, "status").find("\"uiVisible\":true") != std::string::npos,
            "F8 did not reopen UI after legacy Insert toggle");
        const auto fast = Slope(child.dwProcessId, shared, "2");
        Require(std::abs(fast - 2.0) < 0.05, "2x clock slope incorrect: " + std::to_string(fast));
        InterlockedExchange(&shared->captureRequest, 3);
        Sleep(120);
        const auto slow = Slope(child.dwProcessId, shared, "0.25");
        Require(std::abs(slow - 0.25) < 0.02, "0.25x clock slope incorrect: " + std::to_string(slow));
        const auto pause = Command(child.dwProcessId, "pause");
        WaitForFrames(shared, InterlockedCompareExchange(&shared->frames, 0, 0) + 2);
        const auto simulationPaused = InterlockedCompareExchange64(&shared->simulationCounter, 0, 0);
        const auto realPaused = InterlockedCompareExchange64(&shared->realCounter, 0, 0);
        const auto qpcPaused = InterlockedCompareExchange64(&shared->lastQpc, 0, 0);
        const auto chronoPaused = InterlockedCompareExchange64(&shared->lastChrono, 0, 0);
        const auto framesPaused = InterlockedCompareExchange(&shared->frames, 0, 0);
        const auto pausedTicks = Command(child.dwProcessId, "status");
        Sleep(250);
        InterlockedExchange(&shared->captureRequest, 2);
        Sleep(120);
        status = Command(child.dwProcessId, "status");
        Require(InterlockedCompareExchange64(&shared->simulationCounter, 0, 0) == simulationPaused,
            "Native SIM elapsed time advanced while paused");
        Require(InterlockedCompareExchange64(&shared->realCounter, 0, 0) > realPaused,
            "Native REAL elapsed time froze with simulation");
        Require(InterlockedCompareExchange64(&shared->lastQpc, 0, 0) > qpcPaused,
            "Main-EXE QPC froze while the native SIM timer was paused");
        if (chronoPaused)
            Require(InterlockedCompareExchange64(&shared->lastChrono, 0, 0) > chronoPaused,
                "Main-EXE chrono froze while simulation was paused");
        Require(InterlockedCompareExchange(&shared->frames, 0, 0) >= framesPaused + 3,
            "Rendering stopped during native SIM pause");
        Require(Number(status, "virtualCounter") == Number(pause, "virtualCounter"), "Paused API clock advanced");
        Require(Number(status, "simulationTicks") == Number(pausedTicks, "simulationTicks"),
            "Native SIM ticks advanced during pause");
        Require(Number(status, "realTicks") > Number(pausedTicks, "realTicks"),
            "Native REAL ticks stopped during SIM pause");

        // A live panel must still close, reopen, and process a resume key while
        // simulation is frozen. These are messages to our hidden test HWND.
        Require(PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 1) != FALSE, "Cannot close paused panel");
        PostMessageW(window, WM_KEYUP, VK_ESCAPE, (LPARAM(1) << 31) | 1);
        Sleep(100);
        Require(Command(child.dwProcessId, "status").find("\"uiVisible\":false") != std::string::npos,
            "Escape could not close the paused panel");
        Require(PostMessageW(window, WM_KEYDOWN, VK_F8, 1) != FALSE, "Cannot reopen paused panel");
        PostMessageW(window, WM_KEYUP, VK_F8, (LPARAM(1) << 31) | 1);
        Sleep(100);
        Require(Command(child.dwProcessId, "status").find("\"uiVisible\":true") != std::string::npos,
            "F8 could not reopen the paused panel");
        Require(InterlockedCompareExchange64(&shared->simulationCounter, 0, 0) == simulationPaused,
            "Closing or reopening the panel resumed simulation");
        Require(PostMessageW(window, WM_KEYDOWN, VK_PAUSE, 1) != FALSE, "Cannot deliver UI resume key");
        PostMessageW(window, WM_KEYUP, VK_PAUSE, (LPARAM(1) << 31) | 1);
        const auto resumeDeadline = GetTickCount64() + 2000;
        do {
            status = Command(child.dwProcessId, "status");
            if (status.find("\"paused\":false") != std::string::npos) break;
            Sleep(10);
        } while (GetTickCount64() < resumeDeadline);
        Require(status.find("\"paused\":false") != std::string::npos,
            "The paused UI could not process its resume key");
        WaitForFrames(shared, InterlockedCompareExchange(&shared->frames, 0, 0) + 3);
        Require(InterlockedCompareExchange64(&shared->simulationCounter, 0, 0) > simulationPaused,
            "Native SIM did not resume after the UI resume key");
        Require(!gamespeed::SendCommand(child.dwProcessId, "set nan").ok, "NaN speed was accepted");
        Require(!gamespeed::SendCommand(child.dwProcessId, "set 17").ok, "Invalid speed was accepted");
        const auto beforeReset = Command(child.dwProcessId, "status");
        const auto reset = Command(child.dwProcessId, "reset");
        Require(Number(reset, "virtualCounter") >= Number(beforeReset, "virtualCounter"), "Reset made the clock go backwards");
        if (overview) {
            Command(child.dwProcessId, "set 1"); Sleep(1500);
            Command(child.dwProcessId, "set 4"); Sleep(2000);
            Command(child.dwProcessId, "set 0.5"); Sleep(2000);
            Command(child.dwProcessId, "pause"); Sleep(1500);
            Command(child.dwProcessId, "resume");
            Command(child.dwProcessId, "set 2"); Sleep(2000);
            Command(child.dwProcessId, "set 1"); Sleep(2000);
            InterlockedExchange(&shared->captureRequest, 4);
            Sleep(250);
        }
        Command(child.dwProcessId, "shutdown");
        status = Command(child.dwProcessId, "status");
        Require(status.find("\"uiVisible\":false") != std::string::npos, "Shutdown did not hide UI");
        Require(status.find("\"speed\":1,") != std::string::npos, "Shutdown did not return to 1x");
        Require(InterlockedCompareExchange(&shared->regressions, 0, 0) == 0, "Game before/now observed backwards QPC");
        Require(InterlockedCompareExchange(&shared->deadlineFailures, 0, 0) == 0,
            "Executable render QPC deadline stalled during a speed change or pause");
        Require(InterlockedCompareExchange(&shared->stateFailures, 0, 0) == 0,
            "Overlay changed game OpenGL state (binding/viewport/mask/blend bitmask): " + std::to_string(shared->stateFailureMask));
        Require(InterlockedCompareExchange(&shared->readbackError, 0, 0) == 0, "OpenGL framebuffer readback failed");
        Require(InterlockedCompareExchange(&shared->swapError, 0, 0) == 0, "OpenGL overlay reported an error after SwapBuffers: " + std::to_string(shared->swapError));
        std::cout << "Isolated x64 injection, native SIM/REAL separation, real QPC deadlines, pause/UI resume, pipe validation and RmlUi OpenGL "
            << shared->contextMajor << '.' << shared->contextMinor << " core rendering/state restoration passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; exitCode = 1; }
    InterlockedExchange(&shared->quit, 1);
    if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1); // Only our isolated test child.
        WaitForSingleObject(child.hProcess, 1000);
        exitCode = 1;
    }
    DWORD childExit = 0;
    if (!GetExitCodeProcess(child.hProcess, &childExit) || childExit != 0) {
        std::cerr << "Isolated child exit failed: " << childExit << '\n';
        exitCode = 1;
    }
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    UnmapViewOfFile(shared); CloseHandle(mapping);
    return exitCode;
}
