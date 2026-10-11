#include "Extension/Music/local_music_favorites.h"
#include <Windows.h>
#include <iostream>
#include <map>

namespace {
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int run_tests() {
    using namespace dingosdk;
    namespace favorites = music_favorites;
    const auto folder = std::filesystem::temp_directory_path() /
        ("reskate-music-favorites-tests-" + std::to_string(GetCurrentProcessId()));
    const auto save = folder / "reskate.sqlite3";
    const std::string built_in = "Artist - Track", mod = "Mod Artist - Mod Track";
    const std::string playlist = "playlist:z-last", mod_playlist = "playlist:a-mod";
    const auto likes_key = favorites::playlist_profile_key;
    {
        profile::Store store(save);
        check(favorites::load(store).empty(), "old profiles start without favorites");
        bool rejected{}, applied{};
        try {
            favorites::apply_and_save(store, built_in, true, [&] { applied = true; return -2; }, [] { return false; });
        } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && applied && favorites::load(store).empty(), "failed native readback never saves a favorite");
        const auto result = favorites::apply_and_save(store, built_in, true, [&] {
            check(favorites::load(store).empty(), "the native change happens before the profile save");
            return -1;
        }, [] { return true; });
        check(result == -1 && favorites::load(store) == favorites::Songs{built_in},
              "native add success preserves its negative return value and saves the identity");
        favorites::save_change(store, mod, true);
        const auto changes = profile::Store::changes();
        favorites::save_change(store, built_in, true);
        check(profile::Store::changes() == changes, "repeated favorite action does not rewrite the profile");
        favorites::save_change(store, built_in, false);
        favorites::save_change(store, built_in, true); // Move it after the mod song, deliberately not alphabetical.
        store.set_user_value("ReSkate.MusicShuffle", true);
        store.set_user_value("Unrelated", 17);
        const auto saved_songs = favorites::load(store);
        check(favorites::load(store, likes_key).empty(), "old profiles have no saved playlist likes");
        rejected = false;
        try {
            favorites::apply_and_save(store, playlist, true, [] { return -2; }, [] { return false; }, likes_key);
        } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && favorites::load(store, likes_key).empty(), "failed playlist readback does not save a like");
        check(favorites::apply_and_save(store, playlist, true, [] { return -1; }, [] { return true; }, likes_key) == -1,
              "playlist like preserves native add success");
        favorites::save_change(store, mod_playlist, true, likes_key);
        const auto like_changes = profile::Store::changes();
        favorites::save_change(store, playlist, true, likes_key);
        check(profile::Store::changes() == like_changes && favorites::load(store) == saved_songs,
              "repeated playlist likes do not rewrite the save or change song favorites");
    }
    {
        profile::Store store(save);
        const auto saved = favorites::load(store);
        check(saved == favorites::Songs{mod, built_in}, "song identities and selection order survive a store restart");
        const auto liked = favorites::load(store, likes_key);
        check(liked == favorites::Songs{playlist, mod_playlist}, "playlist likes and their order survive a restart");
        std::map<std::string, std::uint64_t> playlist_contexts{{playlist, 700}};
        std::vector<std::uint64_t> liked_handles;
        const auto restore_likes = [&] {
            return favorites::restore(favorites::load(store, likes_key), [&](const std::string& id) {
                const auto found = playlist_contexts.find(id);
                return found == playlist_contexts.end() ? std::uint64_t{} : found->second;
            }, [&](std::uint64_t handle) { liked_handles.push_back(handle); return true; });
        };
        const auto like_changes = profile::Store::changes();
        check(restore_likes() == 1 && liked_handles == std::vector<std::uint64_t>{700},
              "playlist restoration resolves the current generation and skips an absent mod");
        check(favorites::load(store, likes_key) == liked && profile::Store::changes() == like_changes,
              "restoring playlist likes retains absent mods without writing the profile");
        playlist_contexts = {{playlist, 800}, {mod_playlist, 801}};
        liked_handles.clear();
        check(restore_likes() == 2 && liked_handles == std::vector<std::uint64_t>{800, 801},
              "the next map restores new playlist handles in selection order");
        favorites::save_change(store, playlist, false, likes_key);
        check(favorites::load(store, likes_key) == favorites::Songs{mod_playlist} && favorites::load(store) == saved,
              "unliking a playlist preserves other likes and song favorites");
        // A new map supplies new model handles. An absent mod must not be purged.
        std::map<std::string, std::uint64_t> current{{built_in, 100}};
        std::vector<std::uint64_t> applied;
        const auto changes = profile::Store::changes();
        const auto restore = [&] {
            return favorites::restore(favorites::load(store), [&](const std::string& id) {
                const auto found = current.find(id);
                return found == current.end() ? std::uint64_t{} : found->second;
            }, [&](std::uint64_t handle) { applied.push_back(handle); return true; });
        };
        check(restore() == 1 && applied == std::vector<std::uint64_t>{100}, "restoration uses the current song handle only");
        check(favorites::load(store) == saved && profile::Store::changes() == changes,
              "restoration retains absent mod songs and performs no profile writes");
        current = {{built_in, 201}, {mod, 202}};
        applied.clear();
        check(restore() == 2 && applied == std::vector<std::uint64_t>{202, 201},
              "the next catalog generation restores fresh handles and a returning mod");
        check(favorites::restore(saved, [](const std::string&) { return 999ULL; },
              [](std::uint64_t) { return false; }) == 0, "failed native application is not counted as restored");
        favorites::save_change(store, built_in, false);
        check(favorites::load(store) == favorites::Songs{mod}, "unfavorite removes only the selected song");
        check(store.user_value("ReSkate.MusicShuffle") == Json(true) && store.user_value("Unrelated") == Json(17),
              "favorite changes preserve shuffle and unrelated settings");
    }
    {
        profile::Store store(save);
        check(favorites::load(store) == favorites::Songs{mod}, "unfavorite survives another restart");
        check(favorites::load(store, likes_key) == favorites::Songs{mod_playlist}, "playlist unlike survives another restart");
        favorites::save_change(store, mod, false);
        favorites::save_change(store, mod_playlist, false, likes_key);
    }
    {
        profile::Store store(save);
        check(favorites::load(store).empty() && store.user_value(favorites::profile_key) == Json::array(),
              "removing the last favorite persists an explicit empty selection");
        check(favorites::load(store, likes_key).empty() && store.user_value(likes_key) == Json::array(),
              "unliking the last playlist persists an explicit empty selection");
        const Json damaged = Json::array({"Artist - Track", 5});
        bool rejected{};
        try { store.set_user_value(favorites::profile_key, damaged); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && favorites::load(store).empty(), "malformed favorite data rolls back without overwriting the save");
        rejected = false;
        try { store.set_user_value(likes_key, damaged); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && favorites::load(store, likes_key).empty(), "malformed playlist data rolls back without overwriting the save");
        rejected = false;
        try { store.set_user_value("Unrelated", Json::array({1, 2})); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected && store.user_value("Unrelated") == Json(17), "array support is limited to the favorites key");
        for (const auto& invalid : {std::string{}, std::string(256, 'x'), std::string("bad\nidentity")}) {
            rejected = false;
            try { favorites::save_change(store, invalid, true); } catch (const std::runtime_error&) { rejected = true; }
            check(rejected && favorites::load(store).empty(), "invalid identities never change the save");
        }
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (failures) return 1;
    std::cout << "music favorites: ok\n";
    return 0;
}

int main() {
    try { return run_tests(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
