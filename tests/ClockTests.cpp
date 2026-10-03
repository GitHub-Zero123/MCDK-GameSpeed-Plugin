#include "gamespeed/VirtualClock.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestPiecewiseContinuity() {
    gamespeed::VirtualClock clock(1000);
    Require(clock.Sample(1100) == 1100, "initial QPC origin must be preserved");
    Require(clock.SetSpeed(1100, 2.0), "2x should be accepted");
    Require(clock.Sample(1200) == 1300, "elapsed ticks should scale at 2x");
    Require(clock.SetSpeed(1250, 0.5), "0.5x should be accepted");
    Require(clock.Sample(1250) == 1400, "speed transition must retain old segment");
    Require(clock.Sample(1350) == 1450, "new speed must apply after transition");
    clock.Reset(1400);
    Require(clock.Sample(1400) == 1475, "reset must preserve accumulated offset");
    Require(clock.Sample(1500) == 1575, "reset must use continuous 1x");
}

void TestPauseAndStaleSamples() {
    gamespeed::VirtualClock clock(10);
    Require(clock.SetSpeed(20, 3.0), "3x should be accepted");
    clock.TogglePause(30);
    Require(clock.Sample(80) == 50, "pause must freeze virtual counter");
    Require(clock.State().paused && clock.State().resumeSpeed == 3.0, "pause should remember speed");
    Require(clock.Sample(15) == 50, "stale samples must not move counter backward");
    clock.TogglePause(90);
    Require(clock.Sample(100) == 80, "resume must exclude paused real time");
    Require(clock.SetSpeed(100, 0.0), "zero is an explicit pause");
    clock.TogglePause(110);
    Require(clock.Sample(120) == 110, "explicit pause must retain resume speed");
    Require(clock.SetSpeed(110, 2.0), "stale speed transition is accepted at latest anchor");
    Require(clock.Sample(130) == 130, "stale transition must not scale old time twice");
}

void TestFractionalTicksAndLargeOrigin() {
    const auto base = std::numeric_limits<std::int64_t>::max() - 100000;
    gamespeed::VirtualClock clock(base);
    Require(clock.SetSpeed(base, 0.125), "fractional speed should be accepted");
    for (std::int64_t tick = 1; tick <= 8000; ++tick) {
        Require(clock.Sample(base + tick) == base + tick / 8,
            "sub-tick progress must accumulate independently of large origin");
    }
    gamespeed::VirtualClock decimal(0);
    Require(decimal.SetSpeed(0, 0.01), "minimum speed should be accepted");
    for (std::int64_t tick = 1; tick <= 100000; ++tick) decimal.Sample(tick);
    Require(std::llabs(decimal.State().virtualCounter - 1000) <= 1,
        "fractional decimal speed must retain progress across small samples");
    gamespeed::VirtualClock changes(0);
    Require(changes.SetSpeed(0, 0.5), "half speed accepted");
    Require(changes.Sample(1) == 0, "half tick starts fractional carry");
    Require(changes.SetSpeed(1, 0.25), "quarter speed accepted");
    Require(changes.Sample(3) == 1, "speed change must preserve fractional carry");
}

void TestInvalidValuesAndOverflow() {
    gamespeed::VirtualClock clock(100);
    for (const auto invalid : {-1.0, 0.001, 16.001, std::numeric_limits<double>::infinity(),
         -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        Require(!clock.SetSpeed(200, invalid), "invalid speed must be rejected");
        Require(clock.State().realCounter == 100 && clock.State().speed == 1.0,
            "rejection must not mutate timer state");
    }
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    gamespeed::VirtualClock nearLimit(maximum - 10);
    Require(nearLimit.SetSpeed(maximum - 10, 16.0), "maximum speed should be accepted");
    Require(nearLimit.Sample(maximum - 9) == maximum, "overflow must saturate");
    nearLimit.Reset(maximum - 8);
    Require(nearLimit.Sample(maximum) == maximum, "saturated clock must remain monotonic");
    gamespeed::VirtualClock fullRange(std::numeric_limits<std::int64_t>::min());
    Require(fullRange.Sample(maximum) == maximum, "full signed delta must avoid overflow");
    gamespeed::VirtualClock negative(-100);
    Require(negative.SetSpeed(-100, 2.0), "negative origins are supported");
    Require(negative.Sample(-25) == 50, "integer update must safely cross zero");
}

void TestConcurrentSamplesAndControl() {
    gamespeed::VirtualClock clock(0);
    Require(clock.SetSpeed(0, 2.0), "concurrent clock speed accepted");
    std::atomic<std::int64_t> real{0};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&] {
            std::int64_t before = 0;
            for (int iteration = 0; iteration < 25000; ++iteration) {
                const auto stamp = real.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((iteration & 31) == 0) std::this_thread::yield();
                const auto now = clock.Sample(stamp);
                if (now < before || clock.Sample(stamp - 1) < now) failed.store(true);
                before = now;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Require(!failed.load(), "concurrent delayed QPC samples must remain monotonic");
    Require(clock.Sample(real.load()) == 400000, "concurrent samples must not double-count time");

    gamespeed::VirtualClock changing(0);
    real.store(0);
    threads.clear();
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker] {
            std::int64_t before = 0;
            for (int iteration = 0; iteration < 12000; ++iteration) {
                const auto stamp = real.fetch_add(1, std::memory_order_relaxed) + 1;
                if (worker == 0 && iteration % 37 == 0) changing.SetSpeed(stamp, 0.25);
                if (worker == 1 && iteration % 41 == 0) changing.SetSpeed(stamp, 8.0);
                if (worker == 2 && iteration % 43 == 0) changing.TogglePause(stamp);
                if (worker == 3 && iteration % 47 == 0) changing.Reset(stamp);
                const auto now = changing.Sample(stamp);
                if (now < before) failed.store(true);
                before = now;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Require(!failed.load(), "concurrent speed/reset/pause controls must remain monotonic");
    const auto state = changing.State();
    Require(state.realCounter == real.load(), "concurrent controls must retain latest QPC anchor");
    Require(state.virtualCounter >= 0 && state.virtualCounter <= real.load() * 8,
        "concurrent controls must stay within accumulated rate bounds");
}
}

int main() {
    try {
        TestPiecewiseContinuity();
        TestPauseAndStaleSamples();
        TestFractionalTicksAndLargeOrigin();
        TestInvalidValuesAndOverflow();
        TestConcurrentSamplesAndControl();
        std::cout << "All virtual clock tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Virtual clock test failed: " << error.what() << '\n';
        return 1;
    }
}
