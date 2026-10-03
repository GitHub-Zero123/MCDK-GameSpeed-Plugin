#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace gamespeed::runtime {
struct ClockStatus {
    double speed = 1.0;
    double resumeSpeed = 1.0;
    bool paused = false;
    std::uint64_t scaledCalls = 0;
    std::int64_t virtualCounter = 0;
    std::int64_t realCounter = 0;
};
// MinHook is initialized once by the DLL worker before these are called.
bool InitializeClock(HMODULE self, std::string& error);
bool SetSpeed(double speed);
double Speed();
bool Paused();
void TogglePause();
void Reset();
ClockStatus Status();
std::int64_t RawCounter();
std::int64_t CounterFrequency();

// Install on worker, initialize RmlUi only on the OpenGL render thread.
bool InitializeOverlay(HMODULE self, std::string& error);
void SetUiVisible(bool visible);
bool UiVisible();
bool OverlayReady();
std::string OverlayError();
// Stop UI work and return the clock to continuous 1x. DLL stays resident.
void DeactivateOverlay();
}
