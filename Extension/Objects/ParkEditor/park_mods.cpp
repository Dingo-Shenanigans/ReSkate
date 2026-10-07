#include "park_mods.h"

#include "Engine/Vfs/mod_list.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dingosdk::editor {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t maximum_park_bytes = 2 * 1024 * 1024;
constexpr std::size_t maximum_info_bytes = 32 * 1024;

// Base map keys (bam, stadium_1, ...) become file names.
void require_map(std::string_view map) {
    if (map.empty() || map.size() > 32 ||
        !std::all_of(map.begin(), map.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
        throw std::runtime_error("Unsupported base map name.");
}

fs::path mod_directory(const fs::path &mods_root, std::string_view folder) {
    if (!mods::valid_mod_name(folder))
        throw std::runtime_error("That is not a mod folder name.");
    return mods_root / fs::path(std::string(folder));
}

void require_text(std::string_view text, std::size_t limit, const char *what) {
    if (text.size() > limit)
        throw std::runtime_error(std::string(what) + " is too long (" + std::to_string(limit) + " characters at most).");
    for (const auto c : text)
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n')
            throw std::runtime_error(std::string(what) + " contains control characters.");
}

void write_atomically(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
            throw std::runtime_error("Could not write " + path_utf8(path.filename()) + ". The previous copy was kept.");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not replace " + path_utf8(path.filename()) + ". The previous copy was kept.");
}

std::string read_file(const fs::path &path, std::size_t limit) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error)
        throw std::runtime_error("Could not open " + path_utf8(path.filename()) + ".");
    if (size > limit)
        throw std::runtime_error(path_utf8(path.filename()) + " is too large.");
    std::ifstream stream(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Could not read " + path_utf8(path.filename()) + ".");
    return text;
}

std::string folder_for(std::string_view title) {
    std::string name;
    for (const auto c : title) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '-' || c == ' ';
        name += plain ? c : '_';
    }
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() > 56) name.resize(56);
    return mods::valid_mod_name(name) ? name : "Park";
}
} // namespace

std::vector<ParkMod> list_park_mods(const fs::path &data_root) {
    std::vector<ParkMod> result;
    const auto list = mods::scan_mods(data_root);
    for (const auto &entry : list.entries) {
        const auto parks = entry.mod.directory / L"parks";
        std::error_code error;
        if (!fs::is_directory(parks, error)) continue;
        ParkMod mod;
        mod.folder = entry.mod.name;
        mod.details = {entry.mod.title, entry.mod.author, entry.mod.version, entry.mod.description};
        mod.enabled = entry.enabled;
        for (fs::directory_iterator it(parks, error), end; !error && it != end; it.increment(error)) {
            auto name = mods::ascii_path(it->path().filename());
            if (!it->is_regular_file(error) || !name.ends_with(".park.json")) continue;
            name.resize(name.size() - 10);
            try { require_map(name); } catch (...) { continue; }
            mod.maps.push_back(std::move(name));
        }
        std::sort(mod.maps.begin(), mod.maps.end());
        result.push_back(std::move(mod));
    }
    return result;
}

fs::path park_mod_file(const fs::path &mods_root, std::string_view folder, std::string_view map) {
    require_map(map);
    return mod_directory(mods_root, folder) / L"parks" / fs::path(std::string(map) + ".park.json");
}

ParkDocument load_mod_park(const fs::path &mods_root, std::string_view folder, std::string_view map) {
    const auto path = park_mod_file(mods_root, folder, map);
    std::error_code error;
    if (!fs::is_regular_file(path, error))
        throw std::runtime_error("This mod has no park for " + std::string(map) + ".");
    auto park = decode_park(read_file(path, maximum_park_bytes));
    if (park.map != map)
        throw std::runtime_error("The park file for " + std::string(map) + " is for " + park.map + " instead.");
    return park;
}

void save_mod_park(const fs::path &mods_root, std::string_view folder, const ParkDocument &park) {
    std::error_code error;
    if (!fs::is_directory(mod_directory(mods_root, folder), error))
        throw std::runtime_error("The mod " + std::string(folder) + " is not installed.");
    write_atomically(park_mod_file(mods_root, folder, park.map), encode_park(park));
}

static void validate(const ParkModDetails &details) {
    if (details.title.empty())
        throw std::runtime_error("Give the mod a name.");
    require_text(details.title, 64, "The name");
    require_text(details.author, 64, "The author");
    require_text(details.version, 32, "The version");
    require_text(details.description, 512, "The description");
}

void save_park_mod_details(const fs::path &mods_root, std::string_view folder, const ParkModDetails &details) {
    validate(details);
    const auto path = mod_directory(mods_root, folder) / mods::manifest_file;
    auto root = Json::object();
    std::error_code error;
    if (fs::is_regular_file(path, error)) {
        try {
            auto text = read_file(path, maximum_info_bytes);
            if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
            if (auto existing = Json::parse(text); existing.is_object()) root = std::move(existing);
        } catch (...) { /* A broken file is replaced. */ }
    }
    root["name"] = details.title;
    root["author"] = details.author;
    root["version_number"] = details.version;
    root["description"] = details.description;
    write_atomically(path, root.dump(2) + "\n");
}

std::string create_park_mod(const fs::path &mods_root, const ParkModDetails &details) {
    validate(details);
    const auto base = folder_for(details.title);
    std::string folder = base;
    std::error_code error;
    for (int suffix = 2; fs::exists(mods_root / fs::path(folder), error); ++suffix) {
        if (suffix > 99) throw std::runtime_error("Too many mods share that name.");
        folder = base + " " + std::to_string(suffix);
    }
    fs::create_directories(mods_root / fs::path(folder) / L"parks");
    save_park_mod_details(mods_root, folder, details);
    return folder;
}

} // namespace dingosdk::editor
