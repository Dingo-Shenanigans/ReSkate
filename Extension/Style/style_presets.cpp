#include "style_presets.h"
#include "style_file.h"
#include "style_model.h"
#include "Engine/Core/Platform/path_text.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dingosdk::style::presets {
namespace {
constexpr std::string_view preset_extension = ".style.json";
} // namespace

std::filesystem::path file_of(const std::filesystem::path &folder, const std::string &preset) {
    return folder / (preset + std::string(preset_extension));
}
bool exists(const std::filesystem::path &folder, const std::string &preset) {
    std::error_code error;
    return std::filesystem::exists(file_of(folder, preset), error);
}
std::vector<std::string> list(const std::filesystem::path &folder, const std::string &in_use) {
    std::vector<std::string> names{in_use};
    std::error_code error;
    for (std::filesystem::directory_iterator at(folder, error), end; !error && at != end; at.increment(error)) {
        const auto file = path_utf8(at->path().filename());
        if (file.size() <= preset_extension.size() || !file.ends_with(preset_extension)) continue;
        auto name = file.substr(0, file.size() - preset_extension.size());
        // Windows file names ignore case, so a name in another case is the preset in use.
        if (preset_name(name) && !console::equal(name, in_use)) names.push_back(std::move(name));
    }
    std::ranges::sort(names);
    return names;
}
Read read(const std::filesystem::path &folder, const std::string &preset) {
    Read result;
    const auto path = file_of(folder, preset);
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) return result;
        if (std::filesystem::file_size(path, error) > maximum_style_bytes || error) throw std::runtime_error("the style file is too large");
        std::ifstream stream(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        result.style = decode_style(text);
    } catch (const NewerStyle &failure) {
        result.issue = failure.what();
        result.newer = true;
    } catch (const std::exception &failure) {
        result.style = {};
        result.issue = failure.what();
        std::error_code error;
        auto aside = path;
        aside += L".unreadable";
        std::filesystem::copy_file(path, aside, std::filesystem::copy_options::overwrite_existing, error);
        result.copied = !error;
    }
    return result;
}
std::string write_atomically(const std::filesystem::path &path, std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) return "could not write the file";
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return "could not replace the file";
    return {};
}
std::string write(const std::filesystem::path &folder, const std::string &preset, const Style &style) {
    try {
        return write_atomically(file_of(folder, preset), encode_style(style));
    } catch (const std::exception &failure) {
        return failure.what();
    }
}
bool remove(const std::filesystem::path &folder, const std::string &preset) {
    std::error_code error;
    std::filesystem::remove(file_of(folder, preset), error);
    return !error;
}
std::string after_delete(const std::filesystem::path &folder) {
    for (const auto &name : list(folder, "default"))
        if (exists(folder, name)) return name;
    return "default";
}
std::string in_use_or_default(const std::optional<Json> &value) {
    if (value && value->is_string() && preset_name(value->string())) return value->string();
    return "default";
}
} // namespace dingosdk::style::presets
