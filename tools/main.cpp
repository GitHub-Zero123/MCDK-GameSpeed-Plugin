#include <gamespeed/Client.hpp>
#include <windows.h>
#include <charconv>
#include <iostream>
#include <string>

namespace {
void Usage() {
    std::cout << "GameSpeed (Windows x64)\n"
        << "  gamespeed inject <pid> <absolute-path-to-gamespeed_hook.dll>\n"
        << "  gamespeed <status|pause|resume|reset|show|hide|shutdown|input-trace-start|input-trace-stop> <pid>\n"
        << "  gamespeed set <pid> <0 or 0.01..16>\n"
        << "F8 / Ctrl+Shift+G toggles the in-game RmlUi panel. shutdown restores continuous 1x and hides it.\n";
}
std::string Utf8(const wchar_t* value) {
    const auto count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string text(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), count, nullptr, nullptr);
    text.pop_back();
    return text;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { Usage(); return argc == 1 ? 0 : 2; }
    const std::string op = Utf8(argv[1]);
    const std::string pidText = Utf8(argv[2]);
    std::uint32_t pid = 0;
    const auto parsed = std::from_chars(pidText.data(), pidText.data() + pidText.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != pidText.data() + pidText.size() || pid == 0) {
        std::cerr << "Invalid process PID\n"; return 2;
    }
    gamespeed::Result result;
    if (op == "inject" && argc == 4) result = gamespeed::Inject(pid, argv[3]);
    else if (op == "set" && argc == 4) result = gamespeed::SendCommand(pid, "set " + Utf8(argv[3]));
    else if (argc == 3 && (op == "status" || op == "pause" || op == "resume" || op == "reset" || op == "show" || op == "hide" || op == "shutdown" || op == "input-trace-start" || op == "input-trace-stop"))
        result = gamespeed::SendCommand(pid, op);
    else { Usage(); return 2; }
    (result.ok ? std::cout : std::cerr) << result.message << '\n';
    return result.ok ? 0 : 1;
}
