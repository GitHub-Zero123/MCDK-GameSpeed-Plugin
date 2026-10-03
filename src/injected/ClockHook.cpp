#include "gamespeed/Runtime.hpp"
#include "gamespeed/VirtualClock.hpp"

#include <MinHook.h>
#include <atomic>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace gamespeed::runtime {
namespace {
using QpcFunction = BOOL (WINAPI*)(LARGE_INTEGER*);
using ChronoCounterFunction = std::int64_t (__cdecl*)();

VirtualClock clock;
std::atomic<QpcFunction> originalQpc{nullptr};
std::atomic<ChronoCounterFunction> originalChronoCounter{nullptr};
std::atomic<bool> ready{false};
std::atomic<std::uint64_t> scaledCalls{0};
std::atomic<std::int64_t> frequency{0};
std::uintptr_t executableBegin = 0;
std::uintptr_t executableEnd = 0;
std::uintptr_t selfBegin = 0;
std::uintptr_t selfEnd = 0;
thread_local bool bypassNestedQpc = false;

bool GameCaller(std::uintptr_t caller) {
    return caller >= executableBegin && caller < executableEnd &&
        !(caller >= selfBegin && caller < selfEnd);
}

class RawQpcScope final {
public:
    RawQpcScope() noexcept : previous_(bypassNestedQpc) { bypassNestedQpc = true; }
    ~RawQpcScope() { bypassNestedQpc = previous_; }
    RawQpcScope(const RawQpcScope&) = delete;
    RawQpcScope& operator=(const RawQpcScope&) = delete;
private:
    bool previous_;
};

bool ModuleRange(HMODULE module, std::uintptr_t& begin, std::uintptr_t& end) {
    if (!module) return false;
    const auto* image = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0) return false;
    begin = reinterpret_cast<std::uintptr_t>(module);
    const auto size = static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    if (begin > std::numeric_limits<std::uintptr_t>::max() - size) return false;
    end = begin + size;
    return true;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
BOOL WINAPI HookQueryPerformanceCounter(LARGE_INTEGER* result) {
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const auto raw = originalQpc.load(std::memory_order_acquire);
    const BOOL success = raw(result);
    if (success && result && GameCaller(caller) && !bypassNestedQpc) {
        result->QuadPart = clock.Sample(result->QuadPart);
        scaledCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return success;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::int64_t __cdecl HookChronoCounter() {
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const bool nested = bypassNestedQpc;
    const auto original = originalChronoCounter.load(std::memory_order_acquire);
    std::int64_t counter = 0;
    {
        // The wrapper's own QPC call must stay raw. Filtering at this entry
        // retains the game's std::chrono path without scaling other DLL users.
        RawQpcScope rawScope;
        counter = original();
    }
    if (!nested && GameCaller(caller)) {
        counter = clock.Sample(counter);
        scaledCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return counter;
}

bool HookFailure(const char* operation, MH_STATUS status, std::string& error) {
    error = std::string(operation) + ": " + MH_StatusToString(status);
    return false;
}

void ClockWarning(const std::string& detail, std::string& warning) {
    if (!warning.empty()) warning += "; ";
    warning += detail;
    OutputDebugStringA(("GameSpeed clock warning: " + detail + "\n").c_str());
}

void InitializeChronoCounter(std::string& warning) {
    // Do not load a runtime into the game solely for this optional adapter.
    // The target client already imports MSVCP140, and it is loaded before a
    // normal gameplay attachment. Other runtimes keep using the QPC adapter.
    const auto runtime = GetModuleHandleW(L"msvcp140.dll");
    const auto target = reinterpret_cast<ChronoCounterFunction>(
        runtime ? GetProcAddress(runtime, "_Query_perf_counter") : nullptr);
    if (!target) return;

    // A trampoline cannot outlive its target module. Keep an already-loaded
    // runtime resident for the same lifetime as this resident timer DLL.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(target), &pinned)) {
        ClockWarning("Unable to pin MSVCP140 _Query_perf_counter; chrono adapter skipped (Windows error " +
            std::to_string(GetLastError()) + ")", warning);
        return;
    }
    void* trampoline = nullptr;
    const auto created = MH_CreateHook(reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&HookChronoCounter), &trampoline);
    if (created != MH_OK) {
        ClockWarning(std::string("MH_CreateHook(MSVCP140 chrono): ") + MH_StatusToString(created), warning);
        return;
    }
    originalChronoCounter.store(reinterpret_cast<ChronoCounterFunction>(trampoline), std::memory_order_release);
    const auto enabled = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enabled != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        originalChronoCounter.store(nullptr, std::memory_order_release);
        ClockWarning(std::string("MH_EnableHook(MSVCP140 chrono): ") + MH_StatusToString(enabled), warning);
    }
    // _Query_perf_frequency is deliberately unchanged, just like QPF.
}
}

bool InitializeClock(HMODULE self, std::string& error) {
    if (ready.load(std::memory_order_acquire)) return true;
    if (!ModuleRange(GetModuleHandleW(nullptr), executableBegin, executableEnd) ||
        !ModuleRange(self, selfBegin, selfEnd)) {
        error = "Unable to read main executable or injected DLL PE image range";
        return false;
    }
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto target = reinterpret_cast<QpcFunction>(
        kernel ? GetProcAddress(kernel, "QueryPerformanceCounter") : nullptr);
    LARGE_INTEGER origin{};
    LARGE_INTEGER qpcFrequency{};
    if (!target || !target(&origin) || !QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart <= 0) {
        error = "QueryPerformanceCounter/QueryPerformanceFrequency initialization failed";
        return false;
    }
    clock.Initialize(origin.QuadPart);
    frequency.store(qpcFrequency.QuadPart, std::memory_order_relaxed);

    // GetProcAddress resolves the Kernel32 -> KernelBase/API-set forwarder.
    // Hook that actual implementation so imports resolved through either path work.
    void* trampoline = nullptr;
    const auto created = MH_CreateHook(reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&HookQueryPerformanceCounter), &trampoline);
    if (created != MH_OK) return HookFailure("MH_CreateHook(QPC)", created, error);
    originalQpc.store(reinterpret_cast<QpcFunction>(trampoline), std::memory_order_release);
    const auto enabled = MH_EnableHook(reinterpret_cast<void*>(target));
    if (enabled != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        originalQpc.store(nullptr, std::memory_order_release);
        return HookFailure("MH_EnableHook(QPC)", enabled, error);
    }
    InitializeChronoCounter(error);
    ready.store(true, std::memory_order_release);
    return true;
}

std::int64_t RawCounter() {
    LARGE_INTEGER value{};
    const auto raw = originalQpc.load(std::memory_order_acquire);
    if (raw) raw(&value);
    else QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t CounterFrequency() {
    auto value = frequency.load(std::memory_order_relaxed);
    if (value > 0) return value;
    LARGE_INTEGER raw{};
    QueryPerformanceFrequency(&raw);
    return raw.QuadPart;
}

bool SetSpeed(double speed) {
    return ready.load(std::memory_order_acquire) && clock.SetSpeed(RawCounter(), speed);
}

double Speed() { return clock.State().speed; }
bool Paused() { return clock.State().paused; }
void TogglePause() {
    if (ready.load(std::memory_order_acquire)) clock.TogglePause(RawCounter());
}
void Reset() {
    if (ready.load(std::memory_order_acquire)) clock.Reset(RawCounter());
}

ClockStatus Status() {
    if (ready.load(std::memory_order_acquire)) clock.Sample(RawCounter());
    const auto state = clock.State();
    return {state.speed, state.resumeSpeed, state.paused,
        scaledCalls.load(std::memory_order_relaxed), state.virtualCounter, state.realCounter};
}

}
