#include <gamespeed/CursorVisibility.hpp>
#include <iostream>
#include <stdexcept>

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        for (const int initial : {-5, -1, 0, 4}) {
            int physical = initial;
            int logical = initial;
            gamespeed::CursorVisibility cursor;
            auto native = [&](bool show) { return physical += show ? 1 : -1; };
            for (int cycle = 0; cycle < 20; ++cycle) {
                cursor.Acquire(native);
                Require(physical >= 0, "Panel acquisition left cursor hidden");
                // Delayed game hide/show requests after activation; compare
                // every returned count against an independent Win32 counter.
                for (int i = 0; i < 80; ++i) {
                    const bool show = i >= 40;
                    logical += show ? 1 : -1;
                    Require(cursor.Change(show, native) == logical, "Game counter changed under the overlay");
                    Require(physical >= 0, "Late game request hid the modal cursor");
                }
                int attempts = 0;
                while (cursor.Change(false, native) >= 0) {
                    --logical;
                    Require(++attempts < 20, "Game hide-until-negative loop did not terminate");
                }
                --logical;
                Require(physical == 0, "Game hide loop hid the physical cursor");
                attempts = 0;
                while (cursor.Change(true, native) < 0) {
                    ++logical;
                    Require(++attempts < 20, "Game show-until-visible loop did not terminate");
                }
                ++logical;
                cursor.Release(native);
                Require(physical == logical, "Focus loss or close did not restore the game counter exactly");
            }
        }
        std::cout << "Cursor visibility, delayed hide loops and exact release passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
