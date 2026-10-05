// Checks how a mod's reskate-music.json is read: the playlist ids and names, which song entries are
// kept, the limits, and that a document of the wrong shape is refused rather than half-read.
#include "Engine/Vfs/mod_music.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

using dingosdk::mods::parse_music_playlists;

bool refused(const std::string& json) {
    try { parse_music_playlists("mod", json); } catch (const std::exception&) { return true; }
    return false;
}

std::string playlist_of(const std::string& songs) {
    return R"({"schema":1,"playlists":[{"name":"Mix","songs":)" + songs + "}]}";
}
}

int main() try {
    {
        const auto lists = parse_music_playlists("KevinMacLeod", R"({
            "schema": 1,
            "playlists": [
                {"name": "Kevin MacLeod", "songs": ["Kevin MacLeod - Cipher", "Kevin MacLeod - Sneaky Snitch"]},
                {"name": "Second", "songs": []}
            ]})");
        check(lists.size() == 2, "both playlists are read");
        check(lists.at(0).id == "mod:KevinMacLeod:Kevin MacLeod", "id is mod:<mod>:<name>");
        check(lists.at(0).name == "Kevin MacLeod", "name is the shown name");
        check(lists.at(0).songs == std::vector<std::string>{"Kevin MacLeod - Cipher", "Kevin MacLeod - Sneaky Snitch"},
            "songs keep their order and spelling");
        check(lists.at(1).songs.empty(), "an empty playlist is kept (the catalog drops it when nothing matches)");
    }
    {
        const auto lists = parse_music_playlists("mod", playlist_of(
            R"(["A - One", "", 7, null, "B - Tw\to", ")" + std::string(256, 'x') + R"(", "C - Three"])"));
        check(lists.at(0).songs == std::vector<std::string>{"A - One", "C - Three"},
            "empty, non-string, control-character and over-long entries are dropped, the rest kept");
    }
    {
        std::string songs = "[";
        for (int i = 0; i < 1100; ++i) songs += (i ? "," : "") + std::string("\"A - ") + std::to_string(i) + "\"";
        const auto lists = parse_music_playlists("mod", playlist_of(songs + "]"));
        check(lists.at(0).songs.size() == 1024, "at most 1024 songs per playlist");

        std::string many = R"({"schema":1,"playlists":[)";
        for (int i = 0; i < 70; ++i) many += (i ? "," : "") + std::string(R"({"name":"P)") + std::to_string(i) + R"(","songs":[]})";
        check(parse_music_playlists("mod", many + "]}").size() == 64, "at most 64 playlists");
    }
    check(parse_music_playlists("mod", R"({"schema":1,"playlists":[]})").empty(), "no playlists is fine");
    {
        const auto lists = parse_music_playlists("mod", R"({"schema":1,"playlists":[
            {"name":"Mix","artwork":"artwork/playlist.png","songs":["A - One","B - Two"]}],
            "song_artwork":{"A - One":"artwork/track.png","B - Two":"../outside.png","Unlisted":"artwork/other.png"}})");
        check(lists[0].artwork == "artwork/playlist.png", "playlist cover is read");
        check(lists[0].song_artwork.size() == 1 && lists[0].song_artwork.at("A - One") == "artwork/track.png",
              "song covers are limited to members and safe paths");
        for (const auto* path : {"../cover.png", "/cover.png", "C:/cover.png", "a\\cover.png", "a/../cover.png",
                                "a//cover.png", "a/./cover.png", "cover.png:stream", "a%2fb.png", "cover.jpg"})
            check(!dingosdk::mods::music_artwork_path(path), std::string("unsafe artwork path: ") + path);
        const auto bad = parse_music_playlists("mod", R"({"schema":1,"playlists":[
            {"name":"Mix","artwork":42,"songs":["A - One"]}],"song_artwork":[]})");
        check(bad[0].artwork.empty() && bad[0].songs.size() == 1, "bad optional artwork does not discard music");
    }
    check(refused(R"({"schema":2,"playlists":[]})"), "another schema version is refused");
    check(refused(R"({"playlists":[]})"), "a missing schema is refused");
    check(refused(R"({"schema":1})"), "missing playlists is refused");
    check(refused(R"({"schema":1,"playlists":{}})"), "playlists that is not an array is refused");
    check(refused(R"({"schema":1,"playlists":[{"songs":[]}]})"), "a playlist without a name is refused");
    check(refused(R"({"schema":1,"playlists":[{"name":"","songs":[]}]})"), "an empty name is refused");
    check(refused(R"({"schema":1,"playlists":[{"name":"X"}]})"), "a playlist without songs is refused");
    check(refused(R"({"schema":1,"playlists":[{"name":"X","songs":"A - B"}]})"), "songs that is not an array is refused");
    check(refused("not json"), "text that is not JSON is refused");

    if (failures == 0) std::cout << "mod music tests passed\n";
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "FAIL: uncaught: " << error.what() << '\n';
    return 1;
}
