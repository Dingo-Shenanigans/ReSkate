#include "world_model.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/world_model.h"
#include "Engine/Core/Text/ascii.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <sstream>
#include <string_view>

namespace dingosdk {
namespace {
constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
template<class T> bool read(HANDLE process, std::uintptr_t address, T& value) {
    if (address < 0x10000 || address > highest - sizeof(value)) return false;
    SIZE_T count{};
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value,
                             sizeof(value), &count) && count == sizeof(value);
}
bool name(HANDLE process, std::uintptr_t address, std::string& value, bool asset = false) {
    value.clear();
    if (address < 0x10000 || address > highest - 256) return false;
    static const std::size_t page_size = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
    if (!page_size) return false;
    std::array<char, 256> buffer{};
    for (std::size_t offset = 0; offset < buffer.size();) {
        // A terminator in the last readable byte must not require access to the
        // following page. Read only a bounded page fragment, never the pointer
        // directly, and reject incomplete reads before examining the buffer.
        const auto current = address + offset;
        const auto size = std::min(buffer.size() - offset, page_size - current % page_size);
        SIZE_T copied{};
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(current), buffer.data(), size, &copied) ||
            copied != size) return false;
        for (std::size_t i = 0; i < size; ++i) {
            const char c = buffer[i];
            if (!c) return !asset || value.empty() ||
                (ascii_lower(value).starts_with("levels/") && value.size() > 7 && value.find("..") == std::string::npos);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.' || c == ' ')) return false;
            value += c;
        }
        offset += size;
    }
    return false;
}
bool indirect_name(HANDLE process, std::uintptr_t address, std::string& value, bool asset = false) {
    std::uintptr_t pointer{};
    if (!read(process, address, pointer)) return false;
    if (!pointer) { value.clear(); return true; }
    const bool okay = name(process, pointer, value, asset);
    if (!okay) value.clear();
    return okay;
}
bool count(HANDLE process, std::uintptr_t array, std::uint32_t maximum, std::uint32_t& result) {
    if (!array) { result = 0; return true; }
    if (array < 0x10004 || !read(process, array - 4, result)) return false;
    result &= 0x7fffffff;
    return result <= maximum;
}
struct SublevelHeader {
    std::uintptr_t manager{}, current_root{}, tree_root{};
    std::uint8_t active{};
    bool operator==(const SublevelHeader&) const = default;
};
bool sublevel_header(HANDLE process, std::uintptr_t base, SublevelHeader& header) {
    if (!read(process, base + addr::world_model::sublevel_manager, header.manager) || header.manager < 0x10000 ||
        header.manager > highest - 0xb1 || header.manager % 8 ||
        !read(process, header.manager + 0x40, header.current_root) ||
        !read(process, header.manager + 0x58, header.tree_root) ||
        !read(process, header.manager + 0xb0, header.active) || header.active > 1) return false;
    return !header.current_root || (header.current_root >= 0x10000 && header.current_root <= highest &&
                                    header.current_root % 8 == 0);
}
}

WorldCatalog read_world_catalog(HANDLE process, std::uintptr_t base) {
    WorldCatalog output;
    output.issue = "Level registry unavailable";
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return output;
    std::uintptr_t manager{}, begin{}, end{};
    if (!read(process, base + addr::world_model::level_registry, manager) || manager < 0x10000 || manager > highest - 0x30 ||
        !read(process, manager + 0x20, begin) || !read(process, manager + 0x28, end)) return output;
    if (end < begin || begin % 8 || end % 8 || (end - begin) / 8 > 128 ||
        (begin && (begin < 0x10000 || begin > highest)) ||
        (end && (end < 0x10000 || end > highest)) || (end != begin && !begin)) {
        output.issue = "Level registry bounds rejected";
        return output;
    }
    for (auto item = begin; item != end; item += 8) {
        std::uintptr_t metadata{}, points{};
        std::uint32_t points_count{};
        LevelInfo level;
        if (!read(process, item, metadata) || metadata < 0x10000 || metadata > highest - 0x69 ||
            !indirect_name(process, metadata + 0x48, level.asset, true) || level.asset.empty() ||
            !read(process, metadata + 0x68, level.level_flag68) ||
            !read(process, metadata + 0x20, points) || !count(process, points, 64, points_count) ||
            points > highest - 0x18ULL * points_count) {
            output.issue = "Level metadata layout rejected";
            return output;
        }
        for (std::uint32_t i = 0; i < points_count; ++i) {
            StartPointInfo point;
            std::uint8_t is_default{};
            if (!indirect_name(process, points + 0x18ULL * i + 8, point.name) ||
                !read(process, points + 0x18ULL * i + 0x10, is_default) || is_default > 1) {
                output.issue = "Start-point metadata layout rejected";
                return output;
            }
            point.is_default = is_default != 0;
            level.start_points.push_back(std::move(point));
        }
        output.levels.push_back(std::move(level));
    }
    output.available = true;
    output.issue.clear();
    return output;
}

SublevelCatalog read_server_sublevels(HANDLE process, std::uintptr_t base) {
    SublevelCatalog output;
    output.issue = "Server sublevel manager unavailable";
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return output;
    SublevelHeader header;
    if (!sublevel_header(process, base, header)) return output;
    const auto sentinel = header.manager + 0x48;
    std::vector<std::uintptr_t> pending{header.tree_root};
    std::vector<std::uintptr_t> visited;
    std::vector<SublevelInfo> levels;
    visited.reserve(128);
    levels.reserve(128);
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        if (!node || node == sentinel) continue;
        if (node < 0x10000 || node > highest - 0x38 || node % 8) {
            output.issue = "Sublevel node pointer rejected";
            return output;
        }
        if (std::find(visited.begin(), visited.end(), node) != visited.end()) {
            output.issue = "Sublevel tree cycle or duplicate node rejected";
            return output;
        }
        if (visited.size() == 128) {
            output.issue = "Sublevel tree exceeds 128 nodes";
            return output;
        }
        visited.push_back(node);
        std::uintptr_t left{}, right{};
        SublevelInfo level;
        if (!read(process, node, left) || !read(process, node + 8, right) ||
            !read(process, node + 0x28, level.state) ||
            !indirect_name(process, node + 0x30, level.asset, true) || level.asset.empty()) {
            output.issue = "Sublevel metadata layout rejected";
            return output;
        }
        levels.push_back(std::move(level));
        // At most two links per accepted node; the traversal stops at 129 nodes.
        if (right && right != sentinel) pending.push_back(right);
        if (left && left != sentinel) pending.push_back(left);
    }
    SublevelHeader after;
    if (!sublevel_header(process, base, after) || after != header) {
        output.issue = "Sublevel header changed or became unavailable during read";
        return output;
    }
    output.available = true;
    output.active = header.active != 0 && header.current_root != 0;
    output.issue.clear();
    output.levels = std::move(levels);
    return output;
}

WorldDescription read_world_description(HANDLE process, std::uintptr_t descriptor) {
    WorldDescription output;
    if (descriptor < 0x10000 || descriptor > highest - 0x30 ||
        !indirect_name(process, descriptor + 0x10, output.level, true) || output.level.empty() ||
        !indirect_name(process, descriptor + 8, output.start_point) ||
        !indirect_name(process, descriptor + 0x18, output.lm_start_point) ||
        !indirect_name(process, descriptor + 0x20, output.lm_level, true) ||
        !read(process, descriptor + 0x28, output.field28) || !read(process, descriptor + 0x2c, output.flags)) return output;
    output.available = true;
    std::uintptr_t attributes{};
    std::uint32_t attribute_count{};
    if (!read(process, descriptor, attributes) || !count(process, attributes, 64, attribute_count) ||
        attributes > highest - 0x10ULL * attribute_count) return output;
    bool have_game_mode{}, have_hosted_mode{};
    for (std::uint32_t i = 0; i < attribute_count; ++i) {
        std::string key, value;
        if (!indirect_name(process, attributes + 0x10ULL * i, key)) return output;
        key = ascii_lower(key);
        if (key != "gamemode" && key != "hostedmode") continue;
        // Native lookup returns the first case-insensitive match.
        if ((key == "gamemode" && have_game_mode) || (key == "hostedmode" && have_hosted_mode)) continue;
        if (!indirect_name(process, attributes + 0x10ULL * i + 8, value)) return output;
        if (key == "gamemode") { output.game_mode = value; have_game_mode = true; }
        else { output.hosted_mode = value; have_hosted_mode = true; }
    }
    output.attributes_available = true;
    return output;
}

std::string world_catalog_json(const WorldCatalog& catalog) {
    std::ostringstream out;
    out << "{\"available\":" << (catalog.available ? "true" : "false") << ",\"issue\":" << json::quoted(catalog.issue) << ",\"levels\":[";
    bool comma = false;
    for (const auto& level : catalog.levels) {
        if (comma) out << ',';
        comma = true;
        out << "{\"asset\":" << json::quoted(level.asset) << ",\"display_name\":" << json::quoted(level.display_name)
            << ",\"manifest_only\":" << (level.manifest_only ? "true" : "false")
            << ",\"manifest_start_point\":" << json::quoted(level.manifest_start_point)
            << ",\"level_flag68\":" << static_cast<unsigned>(level.level_flag68)
            << ",\"start_points\":[";
        bool point_comma = false;
        for (const auto& point : level.start_points) {
            if (point_comma) out << ',';
            point_comma = true;
            out << "{\"name\":" << json::quoted(point.name) << ",\"default\":" << (point.is_default ? "true" : "false") << '}';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string sublevel_catalog_json(const SublevelCatalog& catalog) {
    std::ostringstream out;
    out << "{\"available\":" << (catalog.available ? "true" : "false")
        << ",\"active\":" << (catalog.active ? "true" : "false")
        << ",\"issue\":" << json::quoted(catalog.issue) << ",\"levels\":[";
    bool comma = false;
    for (const auto& level : catalog.levels) {
        if (comma) out << ',';
        comma = true;
        out << "{\"asset\":" << json::quoted(level.asset) << ",\"state\":" << level.state << '}';
    }
    out << "]}";
    return out.str();
}

std::string world_description_json(const WorldDescription& d) {
    std::ostringstream out;
    out << "{\"available\":" << (d.available ? "true" : "false") << ",\"level\":" << json::quoted(d.level)
        << ",\"start_point\":" << json::quoted(d.start_point) << ",\"lm_level\":" << json::quoted(d.lm_level)
        << ",\"lm_start_point\":" << json::quoted(d.lm_start_point) << ",\"field28\":" << d.field28 << ",\"flags\":[";
    for (std::size_t i = 0; i < d.flags.size(); ++i) { if (i) out << ','; out << static_cast<unsigned>(d.flags[i]); }
    out << "],\"attributes_available\":" << (d.attributes_available ? "true" : "false")
        << ",\"game_mode\":" << json::quoted(d.game_mode) << ",\"hosted_mode\":" << json::quoted(d.hosted_mode) << '}';
    return out.str();
}
}
