#include "physics_presets.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace dingosdk::physics_tuning::presets {
namespace {
constexpr std::size_t max_name = 40;
constexpr std::size_t max_file = 1024 * 1024;
constexpr std::string_view format = "ReSkate physics preset";

std::filesystem::path file_for(const std::filesystem::path &directory, std::string_view name) {
    return directory / (std::string(name) + ".json");
}
} // namespace

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > max_name || name.front() == ' ' || name.back() == ' ') return false;
    return std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
    });
}

std::vector<std::string> list(const std::filesystem::path &directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || it->path().extension() != ".json") continue;
        const auto stem = it->path().stem().string();
        if (valid_name(stem)) names.push_back(stem);
    }
    std::ranges::sort(names);
    return names;
}

bool save(const std::filesystem::path &directory, std::string_view name, const Values &values) {
    if (!valid_name(name)) return false;
    try {
        Json entries = Json::object();
        for (const auto &[key, value] : values)
            if (std::isfinite(value)) entries[key] = static_cast<double>(value);
        Json root = Json::object();
        root["format"] = std::string(format);
        root["version"] = 1;
        root["values"] = std::move(entries);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        const auto target = file_for(directory, name);
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            const auto text = root.dump(2);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) { std::filesystem::remove(temporary, error); return false; }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) { std::filesystem::remove(temporary, error); return false; }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Values> load(const std::filesystem::path &directory, std::string_view name) {
    if (!valid_name(name)) return std::nullopt;
    try {
        std::ifstream in(file_for(directory, name), std::ios::binary);
        if (!in) return std::nullopt;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.size() > max_file) return std::nullopt;
        const auto root = Json::parse(text);
        if (!root.is_object() || !root.contains("values") || !root.at("values").is_object()) return std::nullopt;
        if (root.contains("format") && (!root.at("format").is_string() || root.at("format").string() != format)) return std::nullopt;
        Values values;
        for (const auto &[key, value] : root.at("values").items()) {
            if (!value.is_number()) continue;
            const auto number = value.get<double>();
            if (std::isfinite(number) && std::fabs(number) <= 1e6) values[key] = static_cast<float>(number);
        }
        return values;
    } catch (...) {
        return std::nullopt;
    }
}

bool remove(const std::filesystem::path &directory, std::string_view name) {
    if (!valid_name(name)) return false;
    std::error_code error;
    return std::filesystem::remove(file_for(directory, name), error) && !error;
}
} // namespace dingosdk::physics_tuning::presets
