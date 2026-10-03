#include "gamespeed/VirtualClock.hpp"

#include <cmath>
#include <limits>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace gamespeed {

VirtualClock::Guard::Guard(std::atomic_flag& lock) noexcept : lock_(lock) {
    while (lock_.test_and_set(std::memory_order_acquire)) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
        _mm_pause();
#endif
    }
}

VirtualClock::Guard::~Guard() { lock_.clear(std::memory_order_release); }

VirtualClock::VirtualClock(std::int64_t origin) noexcept
    : realCounter_(origin), virtualCounter_(origin) {}

void VirtualClock::Initialize(std::int64_t origin) noexcept {
    Guard guard(lock_);
    realCounter_ = origin;
    virtualCounter_ = origin;
    fractionalTicks_ = 0;
    speed_ = resumeSpeed_ = 1.0;
}

void VirtualClock::Advance(std::int64_t realCounter) noexcept {
    // A QPC read can precede another thread's read yet reach us later.
    // Ignoring stale reads also prevents a backward timer compensation jump.
    if (realCounter <= realCounter_) return;

    // Unsigned subtraction is well defined even for a full signed-range delta.
    const auto elapsed = static_cast<std::uint64_t>(realCounter) -
        static_cast<std::uint64_t>(realCounter_);
    realCounter_ = realCounter;
    if (speed_ == 0.0 || virtualCounter_ == std::numeric_limits<std::int64_t>::max()) return;

    const long double scaled = static_cast<long double>(elapsed) *
        static_cast<long double>(speed_) + fractionalTicks_;
    const auto headroom = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
        static_cast<std::uint64_t>(virtualCounter_);

    // Saturate before any floating-to-integer conversion that might overflow.
    // The explicit 2^64 check covers platforms where long double is double.
    if (scaled >= static_cast<long double>(headroom) || scaled >= std::ldexp(1.0L, 64)) {
        virtualCounter_ = std::numeric_limits<std::int64_t>::max();
        fractionalTicks_ = 0;
        return;
    }

    const auto wholeTicks = static_cast<std::uint64_t>(scaled);
    fractionalTicks_ = scaled - static_cast<long double>(wholeTicks);

    if (virtualCounter_ >= 0) {
        virtualCounter_ += static_cast<std::int64_t>(wholeTicks);
    } else {
        const auto magnitude = static_cast<std::uint64_t>(-(virtualCounter_ + 1)) + 1;
        if (wholeTicks < magnitude) {
            virtualCounter_ += static_cast<std::int64_t>(wholeTicks);
        } else {
            virtualCounter_ = static_cast<std::int64_t>(wholeTicks - magnitude);
        }
    }
}

std::int64_t VirtualClock::Sample(std::int64_t realCounter) noexcept {
    Guard guard(lock_);
    Advance(realCounter);
    return virtualCounter_;
}

bool VirtualClock::SetSpeed(std::int64_t realCounter, double speed) noexcept {
    if (!std::isfinite(speed) || speed < 0.0 || speed > MaximumSpeed ||
        (speed != 0.0 && speed < MinimumSpeed)) return false;
    Guard guard(lock_);
    Advance(realCounter);
    speed_ = speed;
    if (speed > 0.0) resumeSpeed_ = speed;
    return true;
}

void VirtualClock::TogglePause(std::int64_t realCounter) noexcept {
    Guard guard(lock_);
    Advance(realCounter);
    if (speed_ == 0.0) speed_ = resumeSpeed_;
    else {
        resumeSpeed_ = speed_;
        speed_ = 0.0;
    }
}

void VirtualClock::Reset(std::int64_t realCounter) noexcept {
    Guard guard(lock_);
    Advance(realCounter);
    // Returning to 1x retains the accumulated offset and fractional progress.
    // Returning to the raw QPC origin would trigger the game's compensation.
    speed_ = resumeSpeed_ = 1.0;
}

VirtualClock::Snapshot VirtualClock::State() const noexcept {
    Guard guard(lock_);
    return {speed_, resumeSpeed_, speed_ == 0.0, virtualCounter_, realCounter_};
}

}
