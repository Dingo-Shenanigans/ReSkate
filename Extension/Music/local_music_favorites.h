#pragma once
#include "Extension/Profile/local_profile.h"
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace dingosdk::music_favorites {
inline constexpr std::string_view profile_key = "ReSkate.MusicFavoriteSongs";
inline constexpr std::string_view playlist_profile_key = "ReSkate.MusicLikedPlaylists";
using Songs = std::vector<std::string>;

inline bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 255 &&
        std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

inline Songs load(const profile::Store& store, std::string_view key = profile_key) {
    Songs songs;
    const auto saved = store.user_value(key);
    if (!saved) return songs;
    if (!saved->is_array()) throw std::runtime_error("Invalid saved music favorites");
    for (const auto& value : *saved) {
        if (!value.is_string())
            throw std::runtime_error("Invalid saved favorite song identity");
        const auto id = value.get<std::string>();
        if (!valid_id(id)) throw std::runtime_error("Invalid saved favorite song identity");
        if (std::find(songs.begin(), songs.end(), id) == songs.end()) songs.push_back(id);
    }
    return songs;
}

// Change one identity, retaining favorites whose mod is temporarily unavailable.
// A malformed saved value is never silently replaced by an empty selection.
inline void save_change(profile::Store& store, const std::string& id, bool favorite, std::string_view key = profile_key) {
    if (!valid_id(id)) throw std::runtime_error("Invalid favorite song identity");
    auto songs = load(store, key);
    const auto found = std::find(songs.begin(), songs.end(), id);
    const bool changed = favorite ? found == songs.end() : found != songs.end();
    if (changed) {
        if (favorite) songs.push_back(id);
        else songs.erase(found);
    }
    if (changed) store.set_user_value(key, Json(songs));
}

template<class Apply, class Verify>
auto apply_and_save(profile::Store& store, const std::string& id, bool favorite, Apply apply, Verify verify,
    std::string_view key = profile_key) {
    const auto result = apply();
    if (!verify()) throw std::runtime_error("Native favorite selection readback failed");
    save_change(store, id, favorite, key);
    return result;
}

// Resolve fresh model handles for each catalog generation. Restoration uses only
// the native apply operation; it neither saves nor queues service changes.
template<class Resolve, class Apply>
std::size_t restore(const Songs& songs, Resolve resolve, Apply apply) {
    std::size_t restored{};
    for (const auto& id : songs)
        if (const auto handle = resolve(id); handle && apply(handle)) ++restored;
    return restored;
}
}
