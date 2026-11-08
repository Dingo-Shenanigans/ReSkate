#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// skate.'s score HUD lists each trick as it is done, one entry of ScoringHUDViewModel's trick list
// per trick, written through the UI model's write of one value (hud_corner.cpp's hook). This is
// SmokeyDawg58's reading of it (found with his `mode tricklog`): an entry's
// name, side, landing, body and board spin, stance and modifiers, for plugins (reskate.tricks.feed).
namespace dingosdk::tricks::feed {
struct Note {
    std::uint64_t entry{};
    char kind{}; // 'n' name, 's' side, 'l' landing, 'b' body spin, 'r' board spin, 'k' stance, 'm' modifier
    std::string text;
};
// On while any owner asks (the model write hook is kept on with it).
void ask(const std::string& owner, bool on);
bool wanted() noexcept;
std::vector<Note> take();
// From the model write hook, on whichever thread the game writes from.
void tap(std::uint64_t handle, std::uintptr_t type, const void* value) noexcept;
// Entry ids of the SDK's own display models, never the game's trick list. The widget can write them
// outside OwnUiWrites; the score line publishes the root and its rows before showing them.
inline constexpr std::size_t most_owned = 8;
void own(const std::vector<std::uint64_t>& entries) noexcept;
// A song the game began, or the one a Spotify station shows (spotify_rows.cpp, spotify_station.cpp):
// the game's music writes its title to the trick list's name field in another model, so a name that
// reads as one of these is left out (trick_feed_songs.h). Any thread.
void song_playing(std::string_view artist, std::string_view title) noexcept;
} // namespace dingosdk::tricks::feed
