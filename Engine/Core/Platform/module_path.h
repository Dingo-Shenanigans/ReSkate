#pragma once
#include <Windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dingosdk {
[[nodiscard]] inline std::filesystem::path executable_folder() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("the executable's folder is unknown");
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}
} // namespace dingosdk
