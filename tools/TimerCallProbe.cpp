#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#define PROBE_NOINLINE __declspec(noinline)
#else
#define PROBE_NOINLINE __attribute__((noinline))
#endif

namespace {
using QpcFunction = BOOL (WINAPI*)(LARGE_INTEGER*);
using ChronoFunction = std::int64_t (__cdecl*)();
using TimerFunction = void (__fastcall*)(void*, float);
constexpr std::size_t MaximumRecords = 128;
constexpr std::size_t MaximumFrames = 8;
constexpr std::size_t MaximumTimerObjects = 32;
constexpr DWORD SampleMilliseconds = 5000;
constexpr std::uint64_t BurstWindowMicroseconds = 1000;

enum class Api : unsigned char { Qpc, Chrono, Timer };
struct TimerFields {
    bool valid = false;
    float rate = 0;
    std::int32_t ticks = 0;
    float alpha = 0;
    float timeScale = 0;
    float carry = 0;
    float currentTime = 0;
    float elapsedDelta = 0;
    double timestamp28 = 0;
    double timestamp30 = 0;
    float step = 0;
};
struct Record {
    Api api = Api::Qpc;
    DWORD thread = 0;
    std::uintptr_t caller = 0;
    std::array<std::uintptr_t, MaximumFrames> frames{};
    unsigned short frameCount = 0;
    std::uint64_t calls = 0;
    std::int64_t rawFirst = 0;
    std::int64_t rawLast = 0;
    std::int64_t burstBucket = 0;
    std::uint64_t bucketCalls = 0;
    std::uint64_t maximumBurst = 0;
    std::uintptr_t timer = 0;
    float argumentFirst = 0;
    float argumentLast = 0;
    TimerFields beforeFirst{};
    TimerFields beforeLast{};
    TimerFields afterLast{};
};

std::array<Record, MaximumRecords> records{};
std::size_t recordCount = 0;
std::atomic_flag recordLock = ATOMIC_FLAG_INIT;
std::atomic<bool> sampling{false};
std::atomic<QpcFunction> originalQpc{nullptr};
std::atomic<ChronoFunction> originalChrono{nullptr};
std::atomic<TimerFunction> originalTimer{nullptr};
std::atomic<std::int64_t> continuityOffset{0};
std::atomic<std::uint64_t> eligibleCalls{0};
std::atomic<std::uint64_t> droppedContention{0};
std::atomic<std::uint64_t> droppedCapacity{0};
std::atomic<std::uint64_t> offsetSaturations{0};
std::uintptr_t executableBegin = 0;
std::uintptr_t executableEnd = 0;
DWORD executableTimestamp = 0;
std::int64_t frequency = 0;
std::int64_t burstTicks = 1;
std::array<std::uintptr_t, MaximumTimerObjects> timerObjects{};
std::size_t timerObjectCount = 0;
std::uintptr_t requestedTimerRva = 0;
bool timerOnlyMode = false;
bool timerSignatureVerified = false;
thread_local bool bypass = false;

struct RawScope final {
    bool previous = bypass;
    RawScope() noexcept { bypass = true; }
    ~RawScope() { bypass = previous; }
};

bool GameCaller(std::uintptr_t caller) noexcept {
    return caller >= executableBegin && caller < executableEnd;
}

std::int64_t WithContinuityOffset(std::int64_t value) noexcept {
    const auto offset = continuityOffset.load(std::memory_order_relaxed);
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (offset > 0 && value > maximum - offset) {
        offsetSaturations.fetch_add(1, std::memory_order_relaxed);
        return maximum;
    }
    if (offset < 0 && value < minimum - offset) {
        offsetSaturations.fetch_add(1, std::memory_order_relaxed);
        return minimum;
    }
    return value + offset;
}

// Both this function and its detours retain their frames. Skip the recorder
// and the detour, and always include the direct caller as the first frame.
PROBE_NOINLINE void Observe(Api api, std::uintptr_t caller, std::int64_t raw,
    std::uintptr_t timer = 0, float argument = 0, const TimerFields* before = nullptr,
    const TimerFields* after = nullptr) noexcept {
    if (!sampling.load(std::memory_order_acquire)) return;
    eligibleCalls.fetch_add(1, std::memory_order_relaxed);
    if (recordLock.test_and_set(std::memory_order_acquire)) {
        droppedContention.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!sampling.load(std::memory_order_relaxed)) {
        recordLock.clear(std::memory_order_release);
        return;
    }
    if (api == Api::Timer) {
        const auto found = std::find(timerObjects.begin(), timerObjects.begin() + timerObjectCount, timer);
        if (found == timerObjects.begin() + timerObjectCount) {
            if (timerObjectCount == MaximumTimerObjects) {
                droppedCapacity.fetch_add(1, std::memory_order_relaxed);
                recordLock.clear(std::memory_order_release);
                return;
            }
            timerObjects[timerObjectCount++] = timer;
        }
    }
    void* captured[MaximumFrames]{};
    const auto capturedCount = RtlCaptureStackBackTrace(2, static_cast<DWORD>(MaximumFrames), captured, nullptr);
    std::array<std::uintptr_t, MaximumFrames> frames{};
    unsigned short frameCount = 0;
    frames[frameCount++] = caller;
    for (unsigned short index = 0; index < capturedCount && frameCount < MaximumFrames; ++index) {
        const auto frame = reinterpret_cast<std::uintptr_t>(captured[index]);
        // On compilers with a different unwind shape, deduplicate the direct
        // caller while preserving the remaining captured frames in order.
        if (frame != caller) frames[frameCount++] = frame;
    }
    const DWORD thread = GetCurrentThreadId();
    Record* found = nullptr;
    for (std::size_t index = 0; index < recordCount; ++index) {
        auto& candidate = records[index];
        if (candidate.api == api && candidate.timer == timer && candidate.thread == thread && candidate.caller == caller &&
            candidate.frameCount == frameCount && candidate.frames == frames) {
            found = &candidate;
            break;
        }
    }
    const auto bucket = raw / burstTicks;
    if (!found) {
        if (recordCount == MaximumRecords) {
            droppedCapacity.fetch_add(1, std::memory_order_relaxed);
            recordLock.clear(std::memory_order_release);
            return;
        }
        found = &records[recordCount++];
        found->api = api;
        found->thread = thread;
        found->caller = caller;
        found->frames = frames;
        found->frameCount = frameCount;
        found->rawFirst = raw;
        found->burstBucket = bucket;
        found->timer = timer;
        found->argumentFirst = argument;
        if (before) found->beforeFirst = *before;
    }
    ++found->calls;
    found->rawLast = raw;
    if (bucket != found->burstBucket) {
        found->burstBucket = bucket;
        found->bucketCalls = 0;
    }
    ++found->bucketCalls;
    found->maximumBurst = std::max(found->maximumBurst, found->bucketCalls);
    if (api == Api::Timer) {
        found->argumentLast = argument;
        if (before) found->beforeLast = *before;
        if (after) found->afterLast = *after;
    }
    recordLock.clear(std::memory_order_release);
}

PROBE_NOINLINE BOOL WINAPI ObserveQpc(LARGE_INTEGER* result) {
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const bool nested = bypass;
    RawScope rawScope;
    const BOOL success = originalQpc.load(std::memory_order_acquire)(result);
    if (!nested && success && result && GameCaller(caller)) {
        const auto raw = result->QuadPart;
        Observe(Api::Qpc, caller, raw);
        // This is a constant offset, never a virtual or slowed clock. It
        // preserves an already-installed production hook's 1x time domain.
        result->QuadPart = WithContinuityOffset(raw);
    }
    return success;
}

PROBE_NOINLINE std::int64_t __cdecl ObserveChrono() {
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const bool nested = bypass;
    RawScope rawScope;
    const auto raw = originalChrono.load(std::memory_order_acquire)();
    if (!nested && GameCaller(caller)) {
        Observe(Api::Chrono, caller, raw);
        return WithContinuityOffset(raw);
    }
    return raw;
}

TimerFields ReadTimerFields(const void* timer) noexcept {
    TimerFields fields{};
    if (!timer) return fields;
#if defined(_MSC_VER)
    __try {
#endif
        const auto bytes = static_cast<const unsigned char*>(timer);
        std::memcpy(&fields.rate, bytes + 0x00, sizeof(fields.rate));
        std::memcpy(&fields.ticks, bytes + 0x04, sizeof(fields.ticks));
        std::memcpy(&fields.alpha, bytes + 0x08, sizeof(fields.alpha));
        std::memcpy(&fields.timeScale, bytes + 0x0c, sizeof(fields.timeScale));
        std::memcpy(&fields.carry, bytes + 0x10, sizeof(fields.carry));
        std::memcpy(&fields.currentTime, bytes + 0x18, sizeof(fields.currentTime));
        std::memcpy(&fields.elapsedDelta, bytes + 0x1c, sizeof(fields.elapsedDelta));
        std::memcpy(&fields.timestamp28, bytes + 0x28, sizeof(fields.timestamp28));
        std::memcpy(&fields.timestamp30, bytes + 0x30, sizeof(fields.timestamp30));
        std::memcpy(&fields.step, bytes + 0x3c, sizeof(fields.step));
        fields.valid = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fields.valid = false;
    }
#endif
    return fields;
}

PROBE_NOINLINE void __fastcall ObserveTimer(void* timer, float argument) {
#if defined(_MSC_VER)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
    const bool observe = !bypass && sampling.load(std::memory_order_acquire);
    RawScope rawScope;
    const auto before = observe ? ReadTimerFields(timer) : TimerFields{};
    // The probe never changes this pointer, input float, object fields, or
    // native return behavior. All effects belong to the original routine.
    originalTimer.load(std::memory_order_acquire)(timer, argument);
    if (observe) {
        const auto after = ReadTimerFields(timer);
        LARGE_INTEGER raw{};
        // Called from this diagnostic DLL, this read bypasses the production
        // hook's main-executable filter without installing another QPC hook.
        if (QueryPerformanceCounter(&raw))
            Observe(Api::Timer, caller, raw.QuadPart, reinterpret_cast<std::uintptr_t>(timer), argument, &before, &after);
    }
}

std::string Hex(std::uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::string JsonString(const std::string& value) {
    std::ostringstream stream;
    stream << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') stream << '\\' << static_cast<char>(character);
        else if (character < 32) stream << "\\u00" << hex[character >> 4] << hex[character & 15];
        else stream << static_cast<char>(character);
    }
    stream << '"';
    return stream.str();
}

bool ReadExecutableRange() noexcept {
    const auto module = GetModuleHandleW(nullptr);
    if (!module) return false;
    const auto image = reinterpret_cast<const unsigned char*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || !nt->OptionalHeader.SizeOfImage) return false;
    executableBegin = reinterpret_cast<std::uintptr_t>(module);
    const auto size = static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    if (executableBegin > std::numeric_limits<std::uintptr_t>::max() - size) return false;
    executableEnd = executableBegin + size;
    executableTimestamp = nt->FileHeader.TimeDateStamp;
    return true;
}

bool ReadOffset(const std::wstring& path, bool& provided, std::string& error) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return true;
        error = "Cannot read continuity offset (Windows error " + std::to_string(failure) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    char contents[65]{};
    DWORD received = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 64 &&
        ReadFile(file, contents, static_cast<DWORD>(size.QuadPart), &received, nullptr) &&
        received == size.QuadPart;
    CloseHandle(file);
    if (!read) {
        error = "Continuity offset must contain one signed int64, at most 64 bytes";
        return false;
    }
    const char* begin = contents;
    const char* end = contents + received;
    const auto whitespace = [](char character) { return character == ' ' || character == '\r' || character == '\n' || character == '\t'; };
    while (begin != end && whitespace(*begin)) ++begin;
    while (begin != end && whitespace(end[-1])) --end;
    std::int64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        error = "Invalid signed int64 continuity offset";
        return false;
    }
    continuityOffset.store(value, std::memory_order_relaxed);
    provided = true;
    return true;
}

bool ReadTimerTarget(const std::wstring& path, std::string& error) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return true;
        error = "Cannot read timer target (Windows error " + std::to_string(failure) + ")";
        return false;
    }
    timerOnlyMode = true;
    LARGE_INTEGER size{};
    char contents[65]{};
    DWORD received = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 64 &&
        ReadFile(file, contents, static_cast<DWORD>(size.QuadPart), &received, nullptr) &&
        received == size.QuadPart;
    CloseHandle(file);
    if (!read) { error = "Timer target must contain one hexadecimal RVA, at most 64 bytes"; return false; }
    const char* begin = contents;
    const char* end = contents + received;
    const auto whitespace = [](char character) { return character == ' ' || character == '\r' || character == '\n' || character == '\t'; };
    while (begin != end && whitespace(*begin)) ++begin;
    while (begin != end && whitespace(end[-1])) --end;
    if (end - begin >= 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) begin += 2;
    const auto parsed = std::from_chars(begin, end, requestedTimerRva, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) { error = "Invalid hexadecimal timer target RVA"; return false; }
    constexpr unsigned char expected[] = {
        0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0xf3, 0x0f, 0x10, 0x41, 0x3c, 0x48, 0x8b, 0xd9,
        0x0f, 0x29, 0x74, 0x24, 0x30, 0x0f, 0x28, 0xf1, 0x0f, 0x29, 0x7c, 0x24, 0x20,
        0x0f, 0x57, 0xff, 0x0f, 0x2f, 0xc7, 0x72, 0x4f
    };
    const auto imageSize = executableEnd - executableBegin;
    if (imageSize < sizeof(expected) || requestedTimerRva > imageSize - sizeof(expected)) {
        error = "Timer target RVA is outside the main executable image";
        return false;
    }
    std::array<unsigned char, sizeof(expected)> actual{};
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(executableBegin + requestedTimerRva),
            actual.data(), actual.size(), &copied) || copied != actual.size() ||
        std::memcmp(actual.data(), expected, actual.size()) != 0) {
        error = "Timer target does not match the required 35-byte advanceTime signature";
        return false;
    }
    timerSignatureVerified = true;
    return true;
}

void WriteNumber(std::ostream& stream, double value) {
    if (std::isfinite(value)) stream << std::setprecision(17) << value;
    else stream << "null";
}

void WriteTimerFields(std::ostream& stream, const TimerFields& fields) {
    if (!fields.valid) { stream << "null"; return; }
    stream << "{\"rate_00\": "; WriteNumber(stream, fields.rate);
    stream << ", \"ticks_04\": " << fields.ticks << ", \"alpha_08\": "; WriteNumber(stream, fields.alpha);
    stream << ", \"timeScale_0c\": "; WriteNumber(stream, fields.timeScale);
    stream << ", \"carry_10\": "; WriteNumber(stream, fields.carry);
    stream << ", \"currentTime_18\": "; WriteNumber(stream, fields.currentTime);
    stream << ", \"elapsedDelta_1c\": "; WriteNumber(stream, fields.elapsedDelta);
    stream << ", \"timestamp_28\": "; WriteNumber(stream, fields.timestamp28);
    stream << ", \"timestamp_30\": "; WriteNumber(stream, fields.timestamp30);
    stream << ", \"step_3c\": "; WriteNumber(stream, fields.step);
    stream << '}';
}

void WriteReport(const std::wstring& path, const SYSTEMTIME& timestamp, std::int64_t rawStarted,
    std::int64_t rawEnded, bool offsetProvided, bool chronoInstalled, const std::string& error) {
    // Only the worker can wait for this lock. Detours always use try_lock and
    // stop accessing records once sampling is false; no game thread waits.
    while (recordLock.test_and_set(std::memory_order_acquire)) Sleep(1);
    const auto snapshot = records;
    const auto count = recordCount;
    const auto objectCount = timerObjectCount;
    recordLock.clear(std::memory_order_release);
    char time[40]{};
    std::snprintf(time, sizeof(time), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(timestamp.wYear), static_cast<unsigned>(timestamp.wMonth),
        static_cast<unsigned>(timestamp.wDay), static_cast<unsigned>(timestamp.wHour),
        static_cast<unsigned>(timestamp.wMinute), static_cast<unsigned>(timestamp.wSecond),
        static_cast<unsigned>(timestamp.wMilliseconds));
    std::ostringstream stream;
    stream << "{\n  \"schemaVersion\": 1,\n  \"pid\": " << GetCurrentProcessId()
        << ",\n  \"timestampUtc\": " << JsonString(time)
        << ",\n  \"mainBase\": " << JsonString(Hex(executableBegin))
        << ",\n  \"mainSize\": " << executableEnd - executableBegin
        << ",\n  \"mainPeTimestamp\": " << executableTimestamp
        << ",\n  \"frequency\": " << frequency
        << ",\n  \"mode\": " << JsonString(timerOnlyMode ? "timer-only" : "QPC-and-chrono")
        << ",\n  \"timerTargetRva\": " << JsonString(Hex(requestedTimerRva))
        << ",\n  \"timerSignatureVerified\": " << (timerSignatureVerified ? "true" : "false")
        << ",\n  \"maximumTimerObjects\": " << MaximumTimerObjects
        << ",\n  \"timerObjectCount\": " << objectCount
        << ",\n  \"clockSource\": " << JsonString(timerOnlyMode ?
            "Real QPC from the probe DLL; native timer forwarded unmodified; no QPC or chrono hook installed" :
            "Real QPC; only a fixed main-executable continuity offset is applied; no speed scaling")
        << ",\n  \"continuityOffset\": " << continuityOffset.load(std::memory_order_relaxed)
        << ",\n  \"offsetFileProvided\": " << (offsetProvided ? "true" : "false")
        << ",\n  \"requestedSampleMilliseconds\": " << SampleMilliseconds
        << ",\n  \"rawStarted\": " << rawStarted << ",\n  \"rawEnded\": " << rawEnded
        << ",\n  \"chronoAdapterInstalled\": " << (chronoInstalled ? "true" : "false")
        << ",\n  \"burstWindowMicroseconds\": " << BurstWindowMicroseconds
        << ",\n  \"burstDefinition\": \"Maximum recorded calls for one API/thread/stack in a fixed 1 ms real-QPC bucket; dropped calls are excluded\""
        << ",\n  \"maximumRecords\": " << MaximumRecords << ",\n  \"maximumFrames\": " << MaximumFrames
        << ",\n  \"eligibleCalls\": " << eligibleCalls.load(std::memory_order_relaxed)
        << ",\n  \"droppedContention\": " << droppedContention.load(std::memory_order_relaxed)
        << ",\n  \"droppedCapacity\": " << droppedCapacity.load(std::memory_order_relaxed)
        << ",\n  \"offsetSaturations\": " << offsetSaturations.load(std::memory_order_relaxed)
        << ",\n  \"error\": " << JsonString(error) << ",\n  \"records\": [";
    for (std::size_t index = 0; index < count; ++index) {
        const auto& item = snapshot[index];
        stream << (index ? ",\n" : "\n") << "    {\"api\": "
            << JsonString(item.api == Api::Qpc ? "QueryPerformanceCounter" : item.api == Api::Chrono ? "MSVCP140::_Query_perf_counter" : "Timer::advanceTime")
            << ", \"threadId\": " << item.thread << ", \"caller\": " << JsonString(Hex(item.caller))
            << ", \"callerRva\": ";
        if (GameCaller(item.caller)) stream << JsonString(Hex(item.caller - executableBegin));
        else stream << "null";
        stream << ", \"calls\": " << item.calls << ", \"rawFirst\": " << item.rawFirst
            << ", \"rawLast\": " << item.rawLast << ", \"maxBurst\": " << item.maximumBurst
            << ", \"frames\": [";
        for (unsigned short frame = 0; frame < item.frameCount; ++frame)
            stream << (frame ? ", " : "") << JsonString(Hex(item.frames[frame]));
        stream << ']';
        if (item.api == Api::Timer) {
            stream << ", \"this\": " << JsonString(Hex(item.timer)) << ", \"argumentFirst\": ";
            WriteNumber(stream, item.argumentFirst);
            stream << ", \"argumentLast\": "; WriteNumber(stream, item.argumentLast);
            stream << ", \"firstBefore\": "; WriteTimerFields(stream, item.beforeFirst);
            stream << ", \"lastBefore\": "; WriteTimerFields(stream, item.beforeLast);
            stream << ", \"lastAfter\": "; WriteTimerFields(stream, item.afterLast);
        }
        stream << '}';
    }
    stream << "\n  ]\n}\n";
    const auto report = stream.str();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }
}

DWORD WINAPI Start(void* argument) {
    const auto module = static_cast<HMODULE>(argument);
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Start), &pinned)) return 1;
    std::array<wchar_t, 32768> modulePath{};
    const auto length = GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) return 2;
    const std::wstring fullPath(modulePath.data(), length);
    const auto slash = fullPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 3;
    const auto directory = fullPath.substr(0, slash + 1);
    const auto process = std::to_wstring(GetCurrentProcessId());
    const auto output = directory + L"timer-calls-" + process + L".json";
    const auto offsetFile = directory + L"timer-offset-" + process + L".txt";
    const auto timerTargetFile = directory + L"timer-target-" + process + L".txt";
    SYSTEMTIME timestamp{};
    GetSystemTime(&timestamp);
    std::string error;
    bool offsetProvided = false;
    bool chronoInstalled = false;
    std::int64_t rawStarted = 0;
    std::int64_t rawEnded = 0;
    void* qpcTarget = nullptr;
    void* chronoTarget = nullptr;
    void* timerTarget = nullptr;
    bool qpcEnabled = false;
    bool chronoCreated = false;
    bool timerEnabled = false;
    const auto fail = [&error](const char* operation, MH_STATUS status) {
        if (!error.empty()) error += "; ";
        error += std::string(operation) + ": " + MH_StatusToString(status);
    };

    do {
        if (!ReadExecutableRange()) { error = "Cannot read main executable PE range"; break; }
        if (!ReadTimerTarget(timerTargetFile, error)) break;
        if (!timerOnlyMode && !ReadOffset(offsetFile, offsetProvided, error)) break;
        LARGE_INTEGER qpcFrequency{};
        if (!QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart <= 0) {
            error = "QueryPerformanceFrequency failed";
            break;
        }
        frequency = qpcFrequency.QuadPart;
        burstTicks = std::max<std::int64_t>(1, frequency / 1000);
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        qpcTarget = kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "QueryPerformanceCounter")) : nullptr;
        if (!qpcTarget) { error = "QueryPerformanceCounter is unavailable"; break; }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK) { fail("MH_Initialize", initialized); break; }
        void* trampoline = nullptr;
        if (timerOnlyMode) {
            timerTarget = reinterpret_cast<void*>(executableBegin + requestedTimerRva);
            const auto created = MH_CreateHook(timerTarget, reinterpret_cast<void*>(&ObserveTimer), &trampoline);
            if (created != MH_OK) { fail("MH_CreateHook(Timer::advanceTime)", created); break; }
            originalTimer.store(reinterpret_cast<TimerFunction>(trampoline), std::memory_order_release);
            const auto enabled = MH_EnableHook(timerTarget);
            if (enabled != MH_OK) { fail("MH_EnableHook(Timer::advanceTime)", enabled); break; }
            timerEnabled = true;
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            rawStarted = value.QuadPart;
            sampling.store(true, std::memory_order_release);
            Sleep(SampleMilliseconds);
            sampling.store(false, std::memory_order_release);
            QueryPerformanceCounter(&value);
            rawEnded = value.QuadPart;
            break;
        }
        const auto created = MH_CreateHook(qpcTarget, reinterpret_cast<void*>(&ObserveQpc), &trampoline);
        if (created != MH_OK) { fail("MH_CreateHook(QPC)", created); break; }
        originalQpc.store(reinterpret_cast<QpcFunction>(trampoline), std::memory_order_release);
        // Never load another CRT into the game to obtain an optional adapter.
        const auto runtime = GetModuleHandleW(L"msvcp140.dll");
        chronoTarget = runtime ? reinterpret_cast<void*>(GetProcAddress(runtime, "_Query_perf_counter")) : nullptr;
        if (chronoTarget) {
            HMODULE pinnedRuntime = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(chronoTarget), &pinnedRuntime)) {
                error = "Cannot pin MSVCP140; chrono adapter skipped";
                chronoTarget = nullptr;
            } else {
                const auto chronoStatus = MH_CreateHook(chronoTarget, reinterpret_cast<void*>(&ObserveChrono), &trampoline);
                if (chronoStatus == MH_OK) {
                    chronoCreated = true;
                    originalChrono.store(reinterpret_cast<ChronoFunction>(trampoline), std::memory_order_release);
                } else fail("MH_CreateHook(chrono)", chronoStatus);
            }
        }
        const auto enabled = MH_EnableHook(qpcTarget);
        if (enabled != MH_OK) { fail("MH_EnableHook(QPC)", enabled); break; }
        qpcEnabled = true;
        if (chronoCreated) {
            const auto chronoStatus = MH_EnableHook(chronoTarget);
            if (chronoStatus == MH_OK) chronoInstalled = true;
            else fail("MH_EnableHook(chrono)", chronoStatus);
        }
        LARGE_INTEGER value{};
        originalQpc.load(std::memory_order_acquire)(&value);
        rawStarted = value.QuadPart;
        sampling.store(true, std::memory_order_release);
        Sleep(SampleMilliseconds);
        sampling.store(false, std::memory_order_release);
        originalQpc.load(std::memory_order_acquire)(&value);
        rawEnded = value.QuadPart;
    } while (false);

    sampling.store(false, std::memory_order_release);
    // Restore the pre-probe bytes, including an existing production detour.
    // Retain trampolines: a thread can still be returning from an in-flight
    // invocation after DisableHook. The diagnostic DLL remains pinned.
    if (timerEnabled) {
        const auto status = MH_DisableHook(timerTarget);
        if (status != MH_OK) fail("MH_DisableHook(Timer::advanceTime)", status);
    }
    if (chronoInstalled) {
        const auto status = MH_DisableHook(chronoTarget);
        if (status != MH_OK) fail("MH_DisableHook(chrono)", status);
    }
    if (qpcEnabled) {
        const auto status = MH_DisableHook(qpcTarget);
        if (status != MH_OK) fail("MH_DisableHook(QPC)", status);
    }
    WriteReport(output, timestamp, rawStarted, rawEnded, offsetProvided, chronoInstalled, error);
    return error.empty() ? 0 : 4;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const HANDLE worker = CreateThread(nullptr, 0, Start, module, 0, nullptr);
        if (!worker) return FALSE;
        CloseHandle(worker);
    }
    return TRUE;
}
