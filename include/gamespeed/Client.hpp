#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace gamespeed {
struct Result {
    bool ok = false;
    std::string message;
};
std::wstring PipeName(std::uint32_t pid);
Result SendCommand(std::uint32_t pid, std::string_view command, std::uint32_t timeoutMs = 3000);
Result Inject(std::uint32_t pid, const std::filesystem::path& dll, std::uint32_t timeoutMs = 15000);
}
