#include "gamespeed/Runtime.hpp"
#include "gamespeed/VirtualClock.hpp"

#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace gamespeed::runtime {
namespace {
using AdvanceTimerFunction = void (*)(void*, float);

VirtualClock clock;
std::atomic<AdvanceTimerFunction> originalTimer{nullptr};
std::atomic<bool> ready{false};
std::atomic<bool> nativeReady{false};
std::atomic<std::uint64_t> scaledCalls{0};
std::atomic<std::int64_t> simulationTicks{0};
std::atomic<std::int64_t> realTicks{0};
std::atomic<std::int64_t> frequency{0};
std::atomic<std::uint32_t> timerRva{0};
std::string nativeError;

struct AddressRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    bool Contains(std::uintptr_t address) const noexcept {
        return address >= begin && address < end;
    }
};

struct TimerProfile {
    AdvanceTimerFunction advance = nullptr;
    std::array<AddressRange, 2> simulationCallers{};
    std::array<AddressRange, 2> realCallers{};
};

TimerProfile profile;

const IMAGE_NT_HEADERS64* ModuleHeaders(HMODULE module) {
    if (!module) return nullptr;
    const auto* image = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->OptionalHeader.SizeOfImage == 0)
        return nullptr;
    return nt;
}

bool ModuleRange(HMODULE module, AddressRange& range) {
    const auto* headers = ModuleHeaders(module);
    if (!headers) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(module);
    const auto size = static_cast<std::uintptr_t>(headers->OptionalHeader.SizeOfImage);
    if (begin > std::numeric_limits<std::uintptr_t>::max() - size) return false;
    range = {begin, begin + size};
    return true;
}

bool ExecutableRange(std::uintptr_t address, std::size_t size, const AddressRange& image) {
    if (!image.Contains(address) || size > image.end - address) return false;
    const auto end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const auto protection = memory.Protect & 0xff;
        if (protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const auto region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (region > std::numeric_limits<std::uintptr_t>::max() - memory.RegionSize) return false;
        const auto regionEnd = region + memory.RegionSize;
        if (regionEnd <= address) return false;
        address = (regionEnd < end) ? regionEnd : end;
    }
    return true;
}

template<std::size_t Size>
bool Matches(std::uintptr_t address, const unsigned char (&bytes)[Size], const AddressRange& image) {
    return ExecutableRange(address, Size, image) &&
        std::memcmp(reinterpret_cast<const void*>(address), bytes, Size) == 0;
}

bool ValidateCallSite(std::uintptr_t returnAddress, const unsigned char (&load)[7],
        std::uintptr_t target, const AddressRange& image) {
    if (returnAddress < image.begin + 12 || !ExecutableRange(returnAddress - 12, 12, image))
        return false;
    const auto* code = reinterpret_cast<const unsigned char*>(returnAddress - 12);
    if (std::memcmp(code, load, 7) != 0 || code[7] != 0xe8) return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + 8, sizeof(displacement));
    const auto resolved = static_cast<std::int64_t>(returnAddress) + displacement;
    return resolved >= 0 && static_cast<std::uintptr_t>(resolved) == target;
}

bool NativeGameProfile(HMODULE module, const AddressRange& image, TimerProfile& result) {
    const auto* headers = ModuleHeaders(module);
    // These addresses belong to the verified NetEase 3.10.0.420447 client.
    // Both the function body and its simulation/real call sites must agree;
    // another build never falls back to freezing a system clock.
    if (!headers || headers->FileHeader.TimeDateStamp != 0x6ab3c118 ||
        headers->OptionalHeader.SizeOfImage != 0x1d28e000)
        return false;
    constexpr std::uint32_t advanceRva = 0x0c669a50;
    const auto target = image.begin + advanceRva;
    constexpr unsigned char head[] = {
        0x40,0x53,0x48,0x83,0xec,0x40,0xf3,0x0f,0x10,0x41,0x3c,0x48,0x8b,0xd9,
        0x0f,0x29,0x74,0x24,0x30,0x0f,0x28,0xf1,0x0f,0x29,0x7c,0x24,0x20,
        0x0f,0x57,0xff,0x0f,0x2f,0xc7,0x72,0x4f
    };
    constexpr unsigned char tickTail[] = {
        0xf3,0x0f,0x11,0x4b,0x1c,0xf3,0x0f,0x59,0x4b,0x0c,0xf3,0x0f,0x59,0x0b,
        0xf3,0x0f,0x58,0x4b,0x10,0xf3,0x0f,0x2c,0xc1,0x89,0x43,0x04
    };
    constexpr unsigned char serverSimulation[] = {0x49,0x8b,0x8d,0xd0,0x00,0x00,0x00};
    constexpr unsigned char serverReal[] = {0x49,0x8b,0x8d,0xd8,0x00,0x00,0x00};
    constexpr unsigned char clientSimulation[] = {0x48,0x8b,0x8f,0x10,0x0d,0x00,0x00};
    constexpr unsigned char clientReal[] = {0x48,0x8b,0x8f,0x18,0x0d,0x00,0x00};
    const auto serverSimReturn = image.begin + 0x0b9e67cc;
    const auto serverRealReturn = image.begin + 0x0b9e67db;
    const auto clientSimReturn = image.begin + 0x0718edd4;
    const auto clientRealReturn = image.begin + 0x0718ede8;
    if (!Matches(target, head, image) || !Matches(target + 0x16d, tickTail, image) ||
        !ValidateCallSite(serverSimReturn, serverSimulation, target, image) ||
        !ValidateCallSite(serverRealReturn, serverReal, target, image) ||
        !ValidateCallSite(clientSimReturn, clientSimulation, target, image) ||
        !ValidateCallSite(clientRealReturn, clientReal, target, image))
        return false;
    result.advance = reinterpret_cast<AdvanceTimerFunction>(target);
    result.simulationCallers = {{{serverSimReturn, serverSimReturn + 1}, {clientSimReturn, clientSimReturn + 1}}};
    result.realCallers = {{{serverRealReturn, serverRealReturn + 1}, {clientRealReturn, clientRealReturn + 1}}};
    return true;
}

bool FunctionRange(FARPROC function, const AddressRange& image, AddressRange& result) {
    if (!function) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(function);
    if (!image.Contains(address)) return false;
    DWORD64 base = 0;
    const auto* unwind = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!unwind || base != image.begin || unwind->BeginAddress >= unwind->EndAddress)
        return false;
    result = {static_cast<std::uintptr_t>(base + unwind->BeginAddress),
        static_cast<std::uintptr_t>(base + unwind->EndAddress)};
    return result.Contains(address) && ExecutableRange(result.begin, result.end - result.begin, image);
}

bool IntegrationTestProfile(HMODULE module, const AddressRange& image, TimerProfile& result) {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return false;
    const auto* basename = std::wcsrchr(path.data(), L'\\');
    basename = basename ? basename + 1 : path.data();
    if (_wcsicmp(basename, L"gamespeed_integration_tests.exe") != 0 &&
        _wcsicmp(basename, L"gamespeed_loader_tests.exe") != 0)
        return false;
    const auto advance = GetProcAddress(module, "GameSpeedTestAdvanceTimer");
    const auto simulation = GetProcAddress(module, "GameSpeedTestSimulationTimer");
    const auto real = GetProcAddress(module, "GameSpeedTestRealTimer");
    AddressRange simulationRange;
    AddressRange realRange;
    if (!advance || !ExecutableRange(reinterpret_cast<std::uintptr_t>(advance), 16, image) ||
        !FunctionRange(simulation, image, simulationRange) || !FunctionRange(real, image, realRange) ||
        (simulationRange.begin < realRange.end && realRange.begin < simulationRange.end))
        return false;
    result.advance = reinterpret_cast<AdvanceTimerFunction>(advance);
    result.simulationCallers[0] = simulationRange;
    result.realCallers[0] = realRange;
    return true;
}

bool IsCaller(std::uintptr_t caller, const std::array<AddressRange, 2>& sites) noexcept {
    for (const auto& site : sites) if (site.Contains(caller)) return true;
    return false;
}

void AdvanceSimulation(AdvanceTimerFunction original, void* timer, float argument, double speed) {
    auto* scale = reinterpret_cast<float*>(static_cast<unsigned char*>(timer) + 0x0c);
    const float previous = *scale;
#if defined(_MSC_VER)
    // This helper has only scalar locals, so MSVC can run __finally for both
    // structured and C++ exceptions thrown by the original game function.
    __try {
        *scale = static_cast<float>(static_cast<double>(previous) * speed);
        original(timer, argument);
    } __finally {
        *scale = previous;
    }
#else
    struct RestoreScale {
        float* value;
        float previous;
        ~RestoreScale() { *value = previous; }
    } restore{scale, previous};
    *scale = static_cast<float>(static_cast<double>(previous) * speed);
    original(timer, argument);
#endif
}

std::int32_t CompletedTicks(const void* timer) noexcept {
    std::int32_t ticks = 0;
    std::memcpy(&ticks, static_cast<const unsigned char*>(timer) + 0x04, sizeof(ticks));
    return ticks > 0 ? ticks : 0;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void HookAdvanceTimer(void* timer, float argument) {
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const auto original = originalTimer.load(std::memory_order_acquire);
    if (IsCaller(caller, profile.simulationCallers)) {
        // Only simulation delta is scaled. The game's own advanceTime still
        // updates its wall-clock anchors, including throughout a 0x pause.
        // Native stepping remains native: advanceTime handles it unchanged.
        AdvanceSimulation(original, timer, argument, clock.State().speed);
        scaledCalls.fetch_add(1, std::memory_order_relaxed);
        simulationTicks.fetch_add(CompletedTicks(timer), std::memory_order_relaxed);
    } else {
        original(timer, argument);
        if (IsCaller(caller, profile.realCallers))
            realTicks.fetch_add(CompletedTicks(timer), std::memory_order_relaxed);
    }
}

void ReportNativeUnavailable(const std::string& detail, std::string& error) {
    nativeError = detail;
    error = detail;
    OutputDebugStringA(("GameSpeed native timer: " + detail + "\n").c_str());
}
}

bool InitializeClock(HMODULE self, std::string& error) {
    if (ready.load(std::memory_order_acquire)) return true;
    error.clear();
    AddressRange image;
    AddressRange selfImage;
    const auto executable = GetModuleHandleW(nullptr);
    if (!ModuleRange(executable, image) || !ModuleRange(self, selfImage)) {
        error = "Unable to read main executable or injected DLL PE image range";
        return false;
    }
    LARGE_INTEGER origin{};
    LARGE_INTEGER qpcFrequency{};
    if (!QueryPerformanceCounter(&origin) || !QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart <= 0) {
        error = "QueryPerformanceCounter/QueryPerformanceFrequency initialization failed";
        return false;
    }
    clock.Initialize(origin.QuadPart);
    frequency.store(qpcFrequency.QuadPart, std::memory_order_relaxed);
    ready.store(true, std::memory_order_release);

    // QPC and std::chrono are always real system clocks. Control/status has its
    // own virtual clock; engine simulation has the separately verified hook.
    TimerProfile verified;
    if (!NativeGameProfile(executable, image, verified) && !IntegrationTestProfile(executable, image, verified)) {
        ReportNativeUnavailable("Native simulation timer profile is unavailable for this executable build; only 1x is supported", error);
        return true;
    }
    profile = verified;
    void* trampoline = nullptr;
    const auto created = MH_CreateHook(reinterpret_cast<void*>(profile.advance),
        reinterpret_cast<void*>(&HookAdvanceTimer), &trampoline);
    if (created != MH_OK) {
        ReportNativeUnavailable(std::string("MH_CreateHook(native simulation timer): ") + MH_StatusToString(created), error);
        return true;
    }
    originalTimer.store(reinterpret_cast<AdvanceTimerFunction>(trampoline), std::memory_order_release);
    const auto enabled = MH_EnableHook(reinterpret_cast<void*>(profile.advance));
    if (enabled != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(profile.advance));
        originalTimer.store(nullptr, std::memory_order_release);
        ReportNativeUnavailable(std::string("MH_EnableHook(native simulation timer): ") + MH_StatusToString(enabled), error);
        return true;
    }
    timerRva.store(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(profile.advance) - image.begin),
        std::memory_order_relaxed);
    nativeReady.store(true, std::memory_order_release);
    return true;
}

std::int64_t RawCounter() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
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
    if (!ready.load(std::memory_order_acquire) ||
        (!nativeReady.load(std::memory_order_acquire) && speed != 1.0))
        return false;
    return clock.SetSpeed(RawCounter(), speed);
}

double Speed() { return clock.State().speed; }
bool Paused() { return clock.State().paused; }
void TogglePause() {
    if (ready.load(std::memory_order_acquire) && nativeReady.load(std::memory_order_acquire))
        clock.TogglePause(RawCounter());
}
void Reset() {
    if (ready.load(std::memory_order_acquire)) clock.Reset(RawCounter());
}

ClockStatus Status() {
    if (ready.load(std::memory_order_acquire)) clock.Sample(RawCounter());
    const auto state = clock.State();
    ClockStatus status;
    status.speed = state.speed;
    status.resumeSpeed = state.resumeSpeed;
    status.paused = state.paused;
    status.scaledCalls = scaledCalls.load(std::memory_order_relaxed);
    status.virtualCounter = state.virtualCounter;
    status.realCounter = state.realCounter;
    status.nativeTickReady = nativeReady.load(std::memory_order_acquire);
    status.simulationTicks = simulationTicks.load(std::memory_order_relaxed);
    status.realTicks = realTicks.load(std::memory_order_relaxed);
    status.nativeTimerRva = timerRva.load(std::memory_order_relaxed);
    return status;
}

std::string NativeTickError() { return nativeError; }

}
