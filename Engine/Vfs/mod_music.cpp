#include "Engine/Vfs/mod_music.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <stdexcept>

namespace dingosdk::mods {
namespace {
bool text_ok(const std::string& value) {
    return !value.empty() && value.size() <= 255 &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
}

bool music_artwork_path(const std::string& path) {
    if (!text_ok(path) || path.front() == '/' || path.find_first_of("\\:%?#") != std::string::npos ||
        !path.ends_with(".png")) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == ' ' || part.back() == '.') return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::vector<MusicPlaylist> parse_music_playlists(const std::string& mod, const std::string& json) {
    std::vector<MusicPlaylist> result;
    const auto root = Json::parse(json);
    if (!root.contains("schema") || root.at("schema").get<int>() != 1 || !root.contains("playlists") ||
        !root.at("playlists").is_array())
        throw std::runtime_error("expected schema 1 with a playlists array");
    const auto& list = root.at("playlists");
    std::map<std::string, std::string> covers;
    if (root.contains("song_artwork") && root.at("song_artwork").is_object()) {
        for (const auto& [id, value] : root.at("song_artwork").items()) {
            if (covers.size() >= 4096) break;
            if (text_ok(id) && value.is_string() && music_artwork_path(value.string())) covers.emplace(id, value.string());
        }
    }
    for (std::size_t i = 0; i < list.size() && i < 64; ++i) {
        const auto& entry = list.at(i);
        if (!entry.contains("name") || !entry.at("name").is_string() || !text_ok(entry.at("name").string()) ||
            !entry.contains("songs") || !entry.at("songs").is_array())
            throw std::runtime_error("a playlist needs a name and a songs array");
        MusicPlaylist playlist{"mod:" + mod + ":" + entry.at("name").string(), entry.at("name").string(), {}};
        if (entry.contains("artwork") && entry.at("artwork").is_string() && music_artwork_path(entry.at("artwork").string()))
            playlist.artwork = entry.at("artwork").string();
        const auto& songs = entry.at("songs");
        for (std::size_t s = 0; s < songs.size() && s < 1024; ++s)
            if (songs.at(s).is_string() && text_ok(songs.at(s).string())) playlist.songs.push_back(songs.at(s).string());
        for (const auto& id : playlist.songs)
            if (const auto found = covers.find(id); found != covers.end()) playlist.song_artwork.emplace(*found);
        if (text_ok(playlist.id)) result.push_back(std::move(playlist));
    }
    return result;
}

} // namespace dingosdk::mods
