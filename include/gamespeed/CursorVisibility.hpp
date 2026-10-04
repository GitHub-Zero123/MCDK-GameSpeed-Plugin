#pragma once

namespace gamespeed {
// Used only by the HWND owner thread. Keep the application's ShowCursor
// counter distinct from the temporary increments needed by a modal overlay.
class CursorVisibility {
public:
    template<class Show> void Acquire(Show show) {
        int physical = show(true);
        physical = show(false);
        Normalize(physical, show);
    }
    template<class Show> int Change(bool visible, Show show) {
        const int physical = show(visible);
        const int logical = physical - offset_;
        Normalize(physical, show);
        return logical;
    }
    template<class Show> void Release(Show show) {
        while (offset_ > 0) { show(false); --offset_; }
    }
private:
    template<class Show> void Normalize(int physical, Show show) {
        while (physical < 0) { physical = show(true); ++offset_; }
        while (physical > 0 && offset_ > 0) { physical = show(false); --offset_; }
    }
    int offset_ = 0;
};
}
