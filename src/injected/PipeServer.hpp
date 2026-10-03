#pragma once
#include <string>
namespace gamespeed::runtime {
[[noreturn]] void RunPipeServer(bool clockReady, const std::string& startupError);
}
