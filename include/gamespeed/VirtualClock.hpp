#pragma once

#include <atomic>
#include <cstdint>

namespace gamespeed {

// The origin remains in integer QPC ticks. Only elapsed ticks are scaled,
// preserving precision even after a machine has been running for a long time.
// All methods serialize on a small spin lock; no clock/OS calls occur under it.
class VirtualClock final {
public:
    static constexpr double MinimumSpeed = 0.01;
    static constexpr double MaximumSpeed = 16.0;

    struct Snapshot {
        double speed;
        double resumeSpeed;
        bool paused;
        std::int64_t virtualCounter;
        std::int64_t realCounter;
    };

    explicit VirtualClock(std::int64_t origin = 0) noexcept;
    VirtualClock(const VirtualClock&) = delete;
    VirtualClock& operator=(const VirtualClock&) = delete;

    // Use only before exposing the clock to the game's timer calls.
    void Initialize(std::int64_t origin) noexcept;
    std::int64_t Sample(std::int64_t realCounter) noexcept;
    bool SetSpeed(std::int64_t realCounter, double speed) noexcept;
    void TogglePause(std::int64_t realCounter) noexcept;
    void Reset(std::int64_t realCounter) noexcept;
    Snapshot State() const noexcept;

private:
    class Guard final {
    public:
        explicit Guard(std::atomic_flag& lock) noexcept;
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        std::atomic_flag& lock_;
    };

    void Advance(std::int64_t realCounter) noexcept;

    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::int64_t realCounter_ = 0;
    std::int64_t virtualCounter_ = 0;
    // Kept separately from the integer origin to retain sub-tick progress.
    long double fractionalTicks_ = 0;
    double speed_ = 1.0;
    double resumeSpeed_ = 1.0;
};

}
