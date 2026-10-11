#include "custom_level_manifest.h"

#include "Engine/Core/Json/json.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>

namespace dingosdk {
namespace {

constexpr std::string_view native_root = "levels/game/dingolevel_root/dingolevel_root";

char folded(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::string fold(std::string_view value) {
    std::string result(value);
    for (auto& character : result) character = folded(character);
    return result;
}

bool same(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](char a, char b) { return folded(a) == folded(b); });
}

bool segment_character(char value) {
    const auto c = static_cast<unsigned char>(value);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || value == '_' || value == '-' || value == '.';
}

bool valid_asset(std::string_view asset) {
    if (asset.empty() || asset.size() > 255 || same(asset, native_root)) return false;
    constexpr std::string_view prefix = "levels/game/";
    if (asset.size() <= prefix.size() || !same(asset.substr(0, prefix.size()), prefix)) return false;

    std::size_t segments{};
    for (std::size_t begin = prefix.size(); begin < asset.size();) {
        const auto end = asset.find('/', begin);
        const auto length = (end == std::string_view::npos ? asset.size() : end) - begin;
        if (!length) return false;
        const auto segment = asset.substr(begin, length);
        if (segment == "." || segment == ".." ||
            !std::all_of(segment.begin(), segment.end(), segment_character)) return false;
        ++segments;
        if (end == std::string_view::npos) break;
        begin = end + 1;
        if (begin == asset.size()) return false;
    }
    return segments >= 2;
}

bool valid_display_name(std::string_view value) {
    if (value.empty() || value.size() > 96) return false;
    return std::none_of(value.begin(), value.end(), [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return c < 0x20 || c == 0x7f;
    });
}

bool valid_start_point(std::string_view value) {
    if (value.empty() || value.size() > 127) return false;
    return std::all_of(value.begin(), value.end(), [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || value == '/' || value == '_' ||
            value == '-' || value == '.' || value == ' ';
    });
}

void exact_fields(const Json& object, std::initializer_list<std::string_view> expected,
                  std::string_view description) {
    if (!object.is_object() || object.size() != expected.size())
        throw std::runtime_error(std::string(description) + " has unexpected fields");
    for (const auto field : expected)
        if (!object.contains(field)) throw std::runtime_error(
            std::string(description) + " is missing " + std::string(field));
}

CustomLevelManifest parse(std::string_view text) {
    if (text.size() > custom_level_manifest_max_bytes)
        throw std::runtime_error("custom-level manifest exceeds 64 KiB");

    // Json::parse skips a byte-order mark, which several Windows editors still
    // write. The launcher and the server accept one, so the game must too.
    const auto root = Json::parse(text, JsonLimits{
        custom_level_manifest_max_bytes, 8, 8192});
    exact_fields(root, {"schema", "levels"}, "custom-level manifest");
    if (!root.at("schema").is_number_integer() || root.at("schema").get<std::uint64_t>() != 1)
        throw std::runtime_error("custom-level manifest schema must be integer 1");
    const auto& rows = root.at("levels");
    if (!rows.is_array()) throw std::runtime_error("custom-level manifest levels must be an array");
    if (rows.size() > custom_level_manifest_max_levels)
        throw std::runtime_error("custom-level manifest exceeds 64 levels");

    CustomLevelManifest result;
    result.present = true;
    std::set<std::string, std::less<>> assets;
    for (const auto& row : rows) {
        exact_fields(row, {"asset", "displayName", "startPoints"}, "custom-level row");
        const auto& asset = row.at("asset");
        const auto& display_name = row.at("displayName");
        const auto& points = row.at("startPoints");
        if (!asset.is_string() || !valid_asset(asset.string()))
            throw std::runtime_error("custom-level asset is not a safe levels/game path");
        if (!display_name.is_string() || !valid_display_name(display_name.string()))
            throw std::runtime_error("custom-level displayName is invalid");
        if (!points.is_array() || points.size() > 64)
            throw std::runtime_error("custom-level startPoints must be an array of at most 64 names");
        if (!assets.emplace(fold(asset.string())).second)
            throw std::runtime_error("custom-level assets must be unique ignoring ASCII case");

        LevelInfo level;
        level.asset = asset.string();
        level.display_name = display_name.string();
        level.manifest_only = true;
        std::set<std::string, std::less<>> point_names;
        for (const auto& point : points) {
            if (!point.is_string() || !valid_start_point(point.string()))
                throw std::runtime_error("custom-level start point name is invalid");
            if (!point_names.emplace(fold(point.string())).second)
                throw std::runtime_error("custom-level start points must be unique ignoring ASCII case");
            level.start_points.push_back({point.string(), false});
        }
        if (level.start_points.size() == 1)
            level.manifest_start_point = level.start_points.front().name;
        result.levels.push_back(std::move(level));
    }
    return result;
}

} // namespace

CustomLevelManifest parse_custom_level_manifest(std::string_view text) noexcept {
    try {
        return parse(text);
    } catch (const std::exception& error) {
        CustomLevelManifest result;
        result.present = true;
        result.issue = error.what();
        return result;
    } catch (...) {
        CustomLevelManifest result;
        result.present = true;
        result.issue = "custom-level manifest parsing failed";
        return result;
    }
}

CustomLevelManifest read_custom_level_manifest(const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        const auto exists = std::filesystem::exists(path, error);
        if (error) throw std::runtime_error("custom-level manifest path cannot be inspected");
        if (!exists) return {};
        if (!std::filesystem::is_regular_file(path, error) || error)
            throw std::runtime_error("custom-level manifest is not a regular file");
        const auto bytes = std::filesystem::file_size(path, error);
        if (error || bytes > custom_level_manifest_max_bytes)
            throw std::runtime_error("custom-level manifest exceeds 64 KiB");
        if (bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("custom-level manifest cannot be read");

        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("custom-level manifest cannot be opened");
        std::string text(static_cast<std::size_t>(bytes), '\0');
        if (bytes && !file.read(text.data(), static_cast<std::streamsize>(bytes)))
            throw std::runtime_error("custom-level manifest cannot be read");
        auto result = parse_custom_level_manifest(text);
        result.present = true;
        return result;
    } catch (const std::exception& error) {
        CustomLevelManifest result;
        result.present = true;
        result.issue = error.what();
        return result;
    } catch (...) {
        CustomLevelManifest result;
        result.present = true;
        result.issue = "custom-level manifest loading failed";
        return result;
    }
}

CustomLevelManifest combine_custom_level_manifests(std::span<const CustomLevelManifest> manifests) {
    CustomLevelManifest result;
    std::set<std::string, std::less<>> assets;
    for (const auto& manifest : manifests) {
        if (!manifest.present) continue;
        result.present = true;
        if (!manifest.issue.empty()) continue;
        for (const auto& level : manifest.levels) {
            if (result.levels.size() >= combined_custom_level_limit) return result;
            if (!assets.emplace(fold(level.asset)).second) continue;
            result.levels.push_back(level);
        }
    }
    return result;
}

WorldCatalog merge_custom_levels(WorldCatalog native, const CustomLevelManifest& manifest) {
    if (!manifest.issue.empty()) return native;
    for (const auto& declared : manifest.levels) {
        const auto found = std::find_if(native.levels.begin(), native.levels.end(), [&](const LevelInfo& level) {
            return same(level.asset, declared.asset);
        });
        if (found != native.levels.end()) {
            found->display_name = declared.display_name;
            found->custom = true;
            found->manifest_start_point = declared.manifest_start_point;
            for (const auto& point : declared.start_points) {
                const auto exists = std::ranges::any_of(found->start_points, [&](const StartPointInfo& native_point) {
                    return same(native_point.name, point.name);
                });
                if (!exists) found->start_points.push_back(point);
            }
            continue;
        }
        if (native.levels.size() >= 128) break;
        native.levels.push_back(declared);
        native.levels.back().custom = true;
    }
    return native;
}

} // namespace dingosdk
