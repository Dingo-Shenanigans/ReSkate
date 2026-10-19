#include "mod_list.h"
#include "Engine/Game/Build/supported_build.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace dingosdk::mods {
namespace {
namespace fs = std::filesystem;

constexpr char layout_name[] = "layout.toc";
constexpr char levels_name[] = "reskate-levels.json";
// Written by the tool that built the mod (Studio, since 2026-09-24): who made
// it and when. Purely informational, so it is read loosely and never rejected.
constexpr char build_name[] = "reskate-build.json";
// mods.json and the list of mods left out: as much as the JSON reader itself takes, so the
// number of mods is not held down by the size of the file that lists them.
constexpr std::size_t maximum_order_bytes = 4 * 1024 * 1024;
constexpr std::size_t maximum_build_bytes = 16 * 1024;
constexpr std::size_t maximum_info_bytes = 32 * 1024;
constexpr std::size_t maximum_levels_bytes = 64 * 1024;

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::vector<unsigned char> read_file(const fs::path& path, std::size_t maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open " + path_utf8(path));
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximum)
        throw std::runtime_error("File exceeds size limit: " + path_utf8(path));
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read " + path_utf8(path));
    return bytes;
}

// Text bound for display and the log, kept to printable characters and a sane
// length. UTF-8 sequences pass through; control bytes become '?'.
std::string printable(std::string_view text, std::size_t limit) {
    std::string result;
    for (const auto value : text.substr(0, limit)) {
        const auto ch = static_cast<unsigned char>(value);
        result += ch >= 0x20 && ch != 0x7f ? value : '?';
    }
    if (text.size() > limit) {
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) result.pop_back();
        if (!result.empty() && static_cast<unsigned char>(result.back()) >= 0xc0) result.pop_back();
        result += "...";
    }
    return result;
}

// Players and tools write these by hand, and several Windows editors still
// prepend a byte-order mark, so skip one rather than rejecting the file.
Json parse(const fs::path& path, std::size_t maximum) {
    const auto bytes = read_file(path, maximum);
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    return Json::parse(text, JsonLimits{maximum, 8, 8192});
}

std::string string_field(const Json& root, const char* name, std::size_t limit) {
    return root.contains(name) && root.at(name).is_string() ? printable(root.at(name).string(), limit) : std::string{};
}

// reskate-build.json: {"schema":1,"tool":"ReSkateStudio","version":"1.0.0",
// "built":"2026-09-24T18:35:00Z"}. Extra fields are fine; a broken file is a
// note, never a reason to skip the mod.
void read_build_info(Mod& mod, std::vector<std::string>& notes) {
    const auto path = mod.directory / build_name;
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return;
    try {
        const auto root = parse(path, maximum_build_bytes);
        if (!root.is_object()) throw std::runtime_error("not a JSON object");
        mod.tool = string_field(root, "tool", 64);
        mod.version = string_field(root, "version", 64);
        mod.built = string_field(root, "built", 64);
        if (mod.tool.empty() && mod.version.empty() && mod.built.empty())
            throw std::runtime_error("it names no tool, version or build time");
    } catch (const std::exception& failure) {
        notes.push_back("Mod " + mod.name + ": " + build_name + " could not be read (" + failure.what() + ")");
    }
}

// manifest.json (or reskate-mod.json for mods made before manifests): the
// author's own description of the mod.
void read_mod_info(Mod& mod, std::vector<std::string>& notes) {
    std::error_code error;
    const bool manifest = fs::is_regular_file(mod.directory / manifest_file, error);
    const char* name = manifest ? manifest_file : info_file;
    const auto path = mod.directory / name;
    if (!fs::is_regular_file(path, error) || error) return;
    try {
        const auto root = parse(path, maximum_info_bytes);
        if (!root.is_object()) throw std::runtime_error("not a JSON object");
        if (auto title = string_field(root, "name", 64); !title.empty()) mod.title = std::move(title);
        mod.author = string_field(root, "author", 64);
        auto version = string_field(root, manifest ? "version_number" : "version", 32);
        if (version.empty()) version = string_field(root, manifest ? "version" : "version_number", 32);
        if (!version.empty()) mod.version = std::move(version);
        mod.description = string_field(root, "description", 1024);
    } catch (const std::exception& failure) {
        notes.push_back("Mod " + mod.name + ": " + name + " could not be read (" + failure.what() + ")");
    }
}

// The level assets the mod registers, for its summary line and for naming
// the mod when one of them never finishes loading. The strict reading that
// decides what the level list shows happens in the runtime, with its own log.
void read_levels(Mod& mod) {
    if (!mod.provides_levels) return;
    try {
        const auto root = parse(mod.directory / levels_name, maximum_levels_bytes);
        if (!root.is_object() || !root.contains("levels") || !root.at("levels").is_array()) return;
        for (const auto& row : root.at("levels")) {
            if (!row.is_object() || !row.contains("asset") || !row.at("asset").is_string()) continue;
            mod.levels.push_back(printable(row.at("asset").string(), 160));
            if (mod.levels.size() >= 64) break;
        }
    } catch (...) {
        // The runtime reports what is wrong with the file when it reads it.
    }
}

// .reskate-studio-patch: the stamp line, then key=value lines. A mod that ships game data must
// record the Skate.exe it was built for, and it must be this one (supported_build).
void read_studio_marker(Mod& mod) {
    const auto path = mod.directory / studio_marker_file;
    std::error_code error;
    if (fs::is_regular_file(path, error) && !error) {
        mod.studio_marker = true;
        try {
            const auto bytes = read_file(path, 4096);
            std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
            for (std::size_t at = 0; at < text.size();) {
                const auto end = text.find('\n', at);
                auto line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (line.starts_with("skate_sha256=")) {
                    auto value = lower(line.substr(13));
                    const bool hex = value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                    });
                    if (hex) mod.skate_sha256 = std::move(value);
                }
                if (end == std::string_view::npos) break;
                at = end + 1;
            }
        } catch (...) {}
    }
    if (mod.studio_marker) {
        if (mod.skate_sha256.empty())
            mod.outdated = "it was built by an older ReSkate Studio that does not record which game version it is for";
        else if (mod.skate_sha256 != supported_build::game_sha256)
            mod.outdated = "it was built for another game version (Skate.exe " + mod.skate_sha256.substr(0, 12) + ")";
    } else if (mod.provides_layout) {
        mod.outdated = "it ships game data without a ReSkate Studio game version stamp";
    }
}

std::vector<std::string> folders_on_disk(const fs::path& root) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error)) {
        if (error) break;
        if (!entry.is_directory(error) || error) { error.clear(); continue; }
        const auto name = ascii_path(entry.path().filename());
        if (valid_mod_name(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return lower(a) < lower(b);
    });
    return names;
}

// mods.json rows in priority order. Throws when the file is malformed, so a
// typo cannot silently change which mods load.
std::vector<std::pair<std::string, bool>> read_order(const fs::path& path) {
    const auto root = parse(path, maximum_order_bytes);
    if (!root.is_object() || root.size() != 2 || !root.contains("schema") || !root.contains("mods"))
        throw std::runtime_error("mods.json must hold exactly schema and mods");
    if (!root.at("schema").is_number_integer() || root.at("schema").get<std::uint64_t>() != 1)
        throw std::runtime_error("mods.json schema must be integer 1");
    const auto& rows = root.at("mods");
    if (!rows.is_array()) throw std::runtime_error("mods.json mods must be an array");

    std::vector<std::pair<std::string, bool>> order;
    std::set<std::string, std::less<>> seen;
    for (const auto& row : rows) {
        if (!row.is_object() || row.size() != 2 || !row.contains("name") || !row.contains("enabled"))
            throw std::runtime_error("each mods.json row must hold exactly name and enabled");
        const auto& name = row.at("name");
        const auto& enabled = row.at("enabled");
        if (!name.is_string() || !valid_mod_name(name.string()))
            throw std::runtime_error("mods.json name is not a plain folder name");
        if (!enabled.is_boolean()) throw std::runtime_error("mods.json enabled must be true or false");
        if (!seen.emplace(lower(name.string())).second)
            throw std::runtime_error("mods.json lists " + name.string() + " twice");
        order.emplace_back(name.string(), enabled.get<bool>());
    }
    return order;
}

} // namespace

std::string mod_fingerprint(const std::filesystem::path& directory) {
    std::vector<std::string> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) { error.clear(); continue; }
        const auto size = it->file_size(error);
        const auto written = it->last_write_time(error).time_since_epoch().count();
        // Wide, so any file name counts; ASCII names give the same text as before.
        const auto relative = fs::relative(it->path(), directory, error).generic_wstring();
        std::string name;
        for (const auto ch : relative) {
            if (ch < 0x80) name.push_back(static_cast<char>(ch));
            else name += '#' + std::to_string(static_cast<unsigned>(ch));
        }
        files.push_back(lower(name) + '|' +
                        std::to_string(size) + '|' + std::to_string(written));
        error.clear();
    }
    std::sort(files.begin(), files.end());
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (const auto& file : files)
        for (const auto c : file + '\n') { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return std::to_string(files.size()) + '-' + text;
}

std::map<std::string, Exclusion, std::less<>> read_exclusions(const std::filesystem::path& mods_root) noexcept {
    std::map<std::string, Exclusion, std::less<>> result;
    try {
        const auto path = mods_root / exclusions_file;
        std::error_code error;
        if (!fs::is_regular_file(path, error)) return result;
        const auto root = parse(path, maximum_order_bytes);
        if (!root.is_object() || !root.contains("mods") || !root.at("mods").is_object()) return result;
        for (const auto& [name, row] : root.at("mods").items()) {
            if (!valid_mod_name(name) || !row.is_object()) continue;
            Exclusion exclusion;
            exclusion.fingerprint = row.value("fingerprint", std::string{});
            exclusion.sdk = row.value("sdk", std::string{});
            exclusion.conflictWith = row.value("conflict_with", std::string{});
            exclusion.conflictFingerprint = row.value("conflict_fingerprint", std::string{});
            if (row.contains("problems") && row.at("problems").is_array())
                for (const auto& problem : row.at("problems"))
                    if (problem.is_string() && exclusion.problems.size() < 16)
                        exclusion.problems.push_back(printable(problem.string(), 600));
            if (!exclusion.fingerprint.empty()) result.emplace(name, std::move(exclusion));
        }
    } catch (...) { /* An unreadable file just means merging again. */ }
    return result;
}

void write_exclusions(const std::filesystem::path& mods_root,
                      const std::map<std::string, Exclusion, std::less<>>& exclusions) noexcept {
    try {
        const auto path = mods_root / exclusions_file;
        std::error_code error;
        if (exclusions.empty()) { fs::remove(path, error); return; }
        auto rows = Json::object();
        for (const auto& [name, exclusion] : exclusions) {
            auto row = Json::object();
            row["fingerprint"] = exclusion.fingerprint;
            row["sdk"] = exclusion.sdk;
            if (!exclusion.conflictWith.empty()) {
                row["conflict_with"] = exclusion.conflictWith;
                row["conflict_fingerprint"] = exclusion.conflictFingerprint;
            }
            auto problems = Json::array();
            for (std::size_t i = 0; i < exclusion.problems.size() && i < 16; ++i)
                problems.push_back(exclusion.problems[i].substr(0, 600));
            row["problems"] = std::move(problems);
            rows[name] = std::move(row);
        }
        auto root = Json::object();
        root["schema"] = 1;
        root["mods"] = std::move(rows);
        const auto text = root.dump(2) + "\n";
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!output.flush()) return;
        }
        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } catch (...) {}
}

std::string ascii_path(const std::filesystem::path& path) {
    const auto wide = path.generic_wstring();
    std::string text;
    text.reserve(wide.size());
    for (const auto ch : wide) {
        if (ch == 0 || ch > 0x7f) return {};
        text.push_back(static_cast<char>(ch));
    }
    return text;
}

bool valid_mod_name(std::string_view name) {
    if (name.empty() || name.size() > maximum_mod_name || name.front() == '.') return false;
    return std::all_of(name.begin(), name.end(), [](char value) {
        const auto ch = static_cast<unsigned char>(value);
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || value == '_' || value == '-' || value == '.' || value == ' ';
    });
}

Mod describe_mod(const std::filesystem::path& directory, std::string name, std::vector<std::string>& notes) {
    std::error_code error;
    Mod mod;
    mod.name = std::move(name);
    mod.title = mod.name;
    mod.directory = directory;
    mod.provides_layout = fs::is_regular_file(mod.directory / layout_name, error);
    mod.provides_levels = fs::is_regular_file(mod.directory / levels_name, error);
    for (fs::directory_iterator it(mod.directory / "parks", error), end; !error && it != end; it.increment(error)) {
        auto file = ascii_path(it->path().filename());
        if (!file.ends_with(".park.json") || mod.park_maps.size() >= 16) continue;
        file.resize(file.size() - 10);
        mod.park_maps.push_back(std::move(file));
    }
    std::sort(mod.park_maps.begin(), mod.park_maps.end());
    read_build_info(mod, notes);
    read_mod_info(mod, notes);
    read_levels(mod);
    read_studio_marker(mod);
    return mod;
}

ModList scan_mods(const std::filesystem::path& data_root) noexcept {
    ModList result;
    try {
        if (data_root.empty()) {
            result.issue = "The engine data root could not be resolved";
            return result;
        }
        result.root = data_root / mods_folder;
        std::error_code error;
        if (!fs::is_directory(result.root, error) || error) return result;
        result.present = true;

        const auto names = folders_on_disk(result.root);
        std::map<std::string, std::string, std::less<>> present;
        for (const auto& name : names) present.emplace(lower(name), name);

        std::vector<std::pair<std::string, bool>> order;
        const auto order_path = result.root / order_file;
        if (fs::is_regular_file(order_path, error)) {
            try {
                order = read_order(order_path);
            } catch (const std::exception& failure) {
                result.issue = failure.what();
                order.clear();
            }
        }
        // Folders nobody listed still load, after the listed ones, so dropping
        // a mod in works without editing mods.json.
        std::set<std::string, std::less<>> listed;
        for (const auto& [name, enabled] : order) listed.emplace(lower(name));
        for (const auto& name : names)
            if (!listed.contains(lower(name))) order.emplace_back(name, true);

        for (const auto& [name, enabled] : order) {
            const auto found = present.find(lower(name));
            if (found == present.end()) {
                result.missing.push_back(name);
                continue;
            }
            ModEntry entry;
            entry.enabled = enabled;
            entry.mod = describe_mod(result.root / found->second, found->second, result.notes);
            result.entries.push_back(std::move(entry));
        }
        // Only mods named in the exclusions file are fingerprinted.
        for (const auto& [name, exclusion] : read_exclusions(result.root))
            for (const auto& entry : result.entries)
                if (entry.mod.name == name && mod_fingerprint(entry.mod.directory) == exclusion.fingerprint)
                    result.excluded.emplace(name, exclusion.problems);
    } catch (const std::exception& failure) {
        if (result.issue.empty()) result.issue = failure.what();
    } catch (...) {
        if (result.issue.empty()) result.issue = "The Mods folder could not be read";
    }
    return result;
}

bool mod_disabled(const std::filesystem::path& mods_root, std::string_view name) noexcept {
    try {
        std::error_code error;
        const auto path = mods_root / order_file;
        if (!fs::is_regular_file(path, error)) return false;
        for (const auto& [listed, enabled] : read_order(path))
            if (lower(listed) == lower(name)) return !enabled;
    } catch (...) {}
    return false;
}

bool set_mod_enabled(const std::filesystem::path& mods_root, std::string_view name, bool enabled) noexcept {
    try {
        if (!valid_mod_name(name)) return false;
        std::error_code error;
        const auto path = mods_root / order_file;
        auto order = fs::is_regular_file(path, error) ? read_order(path) : std::vector<std::pair<std::string, bool>>{};
        const auto found = std::ranges::find_if(order, [&](const auto& row) { return lower(row.first) == lower(name); });
        if (found != order.end()) found->second = enabled;
        else order.emplace_back(std::string(name), enabled);
        std::vector<ModEntry> entries;
        for (const auto& [listed, on] : order) {
            ModEntry entry;
            entry.mod.name = listed;
            entry.enabled = on;
            entries.push_back(std::move(entry));
        }
        save_mod_order(mods_root, entries);
        return true;
    } catch (...) {
        return false;
    }
}

void save_mod_order(const std::filesystem::path& mods_root, const std::vector<ModEntry>& entries) {
    auto rows = Json::array();
    for (const auto& entry : entries) {
        if (!valid_mod_name(entry.mod.name))
            throw std::runtime_error("\"" + entry.mod.name + "\" is not a valid mod folder name");
        auto row = Json::object();
        row["name"] = entry.mod.name;
        row["enabled"] = entry.enabled;
        rows.push_back(std::move(row));
    }
    auto root = Json::object();
    root["schema"] = 1;
    root["mods"] = std::move(rows);
    const auto text = root.dump(2) + "\n";

    fs::create_directories(mods_root);
    const auto target = mods_root / order_file;
    auto temporary = target;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size())) || !output.flush())
            throw std::runtime_error("Cannot write " + path_utf8(temporary));
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("Cannot replace " + path_utf8(target) + " (Windows error " +
            std::to_string(GetLastError()) + ")");
    }
}

} // namespace dingosdk::mods
