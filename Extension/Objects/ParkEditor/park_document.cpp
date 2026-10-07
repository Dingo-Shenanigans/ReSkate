#include "park_document.h"
#include "Engine/Vfs/mod_list.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dingosdk::editor {
namespace {
std::filesystem::path park_path(const std::filesystem::path &directory, std::string_view name) {
    if (!valid_park_name(name))
        throw std::runtime_error(
            "Use 1-64 letters, numbers, spaces, underscores or hyphens for the park name.");
    return directory / (std::string(name) + ".park.json");
}
} // namespace
bool valid_park_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ')
        return false;
    for (const auto c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
              c == '_' || c == '-'))
            return false;
    std::string upper(name);
    for (auto &c : upper)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 32);
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
        return false;
    if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '1' &&
        upper[3] <= '9')
        return false;
    return true;
}
std::string encode_park(const ParkDocument &park) {
    profile::PlacementSnapshot snapshot;
    snapshot.maps.emplace(park.map, park.objects);
    snapshot.extensions["format"] = "reskate-park";
    return profile::encode_placements(snapshot);
}
ParkDocument decode_park(std::string_view text) {
    if (text.size() > 2 * 1024 * 1024)
        throw std::runtime_error("Park file exceeds 2 MB.");
    const auto doc = profile::decode_placements(text);
    if (doc.maps.size() != 1 || doc.extensions.value("format", std::string{}) != "reskate-park")
        throw std::runtime_error("Expected a ReSkate park containing exactly one base map.");
    return {doc.maps.begin()->first, doc.maps.begin()->second};
}
void save_park(const std::filesystem::path &directory, std::string_view name, const ParkDocument &park) {
    const auto path = park_path(directory, name);
    const auto text = encode_park(park);
    std::filesystem::create_directories(directory);
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
            throw std::runtime_error("Could not write park. Previous save retained.");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not replace park file. Previous save retained.");
}
ParkDocument load_park(const std::filesystem::path &directory, std::string_view name) {
    const auto path = park_path(directory, name);
    if (std::filesystem::file_size(path) > 2 * 1024 * 1024)
        throw std::runtime_error("Park file exceeds 2 MB.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Could not open park.");
    std::string text(2 * 1024 * 1024 + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    return decode_park(text);
}
std::vector<std::string> list_parks(const std::filesystem::path &directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end && names.size() < 256; it.increment(error)) {
        if (!it->is_regular_file(error))
            continue;
        auto name = mods::ascii_path(it->path().filename());
        if (!name.ends_with(".park.json"))
            continue;
        name.resize(name.size() - 10);
        if (valid_park_name(name))
            names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}
} // namespace dingosdk::editor
