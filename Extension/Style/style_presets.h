#pragma once
#include "Extension/Style/style_pose.h"
#include "Engine/Core/Json/json.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The preset files in one folder: <name>.style.json. No game access.
namespace dingosdk::style::presets {
[[nodiscard]] std::filesystem::path file_of(const std::filesystem::path &folder, const std::string &preset);
[[nodiscard]] bool exists(const std::filesystem::path &folder, const std::string &preset);
// Sorted; holds `in_use` even when it has no file.
[[nodiscard]] std::vector<std::string> list(const std::filesystem::path &folder, const std::string &in_use);
struct Read {
    Style style;
    std::string issue; // empty when the file was read or is missing
    bool copied{};     // an unreadable file was copied aside with ".unreadable" added
    bool newer{};      // the file is from a newer ReSkate; it is not copied
    // The session must not write over the file: it is newer, or it could not be read and has no copy.
    [[nodiscard]] bool keep() const noexcept { return newer || (!issue.empty() && !copied); }
};
[[nodiscard]] Read read(const std::filesystem::path &folder, const std::string &preset);
// Empty on success, else why.
[[nodiscard]] std::string write(const std::filesystem::path &folder, const std::string &preset, const Style &style);
// Makes the folder. Empty on success, else why.
[[nodiscard]] std::string write_atomically(const std::filesystem::path &path, std::string_view bytes);
bool remove(const std::filesystem::path &folder, const std::string &preset);
// The preset to use after the one in use is deleted: the first saved preset, else "default".
[[nodiscard]] std::string after_delete(const std::filesystem::path &folder);
// The profile value of the preset in use: its text when that is a preset name, else "default".
[[nodiscard]] std::string in_use_or_default(const std::optional<Json> &value);
} // namespace dingosdk::style::presets
