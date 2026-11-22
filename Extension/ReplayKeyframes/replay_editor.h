#pragma once
#include "replay_clock.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The replay editor as plugins see it (reskate.replay): the replay's time the picture shows, the editor's timeline and
// trim, the camera's keyframes (read, and copied to be put back on a later visit: the game wipes them when the editor
// closes), and where the replay has the skater. Read from the game on the client thread (the game's UI model under its
// lock, the replay's chunks, the keyframe manager), with one hook of the game's: its keyframe player, whose time goes as
// the keys' playback speed has it (slow motion and all), installed the first time the state is asked for. Exports are
// followed by the frames their encoder takes (Extension/Rendering/replay_export.h).
namespace dingosdk::replay_editor {
using Chunk = replay_keyframes::Chunk;
struct State {
    // The replay's time the picture shows, on the replay's clock: the editor's playhead, and while a video is exported, the
    // time of the frame it films (as the camera's keyframes play it while they do, their playback speed and all; else by
    // its frames from the trim's start).
    double time{};
    double playhead{};   // the editor's playhead on the clock (it stands still while a video is exported)
    double from{}, to{}; // the replay's footage on its clock
    // The editor's timeline: its bar's start on the clock and its length (seconds), the playhead's share of it, the trim
    // (shares of the whole replay, as the chunks'), and the part of the whole replay the bar shows. Not read yet: length 0.
    replay_keyframes::Timeline timeline;
    float at{};
    bool keys_playing{}; // the camera's keyframes played in the last few frames
    bool exporting{};
    std::uint64_t frame{}; // the export's frames encoded so far
    float fps{};           // its frame rate: the MP4 setup's, else 60
    bool ui_hidden{};      // the game's UI is hidden (the editor's Hide UI)
};
// Client thread: the editor's state, or nothing outside it.
std::optional<State> state(std::uintptr_t base) noexcept;

// The camera's keyframes, every session's, as the editor has them now.
struct Key {
    int session{};
    double time{};
    int camera{}; // 0 orbit, 1 free
    float fov{}, speed{};
};
std::vector<Key> keys(std::uintptr_t base) noexcept;
// A copy of every camera keyframe as the game keeps it, kept here for `owner` (a plugin holds only its number, never the
// game's bytes): its number (from 1), or 0 when there are none to copy (or no editor). At most 8 kept for an owner; the
// oldest goes.
std::uint64_t copy_keys(std::uintptr_t base, std::string_view owner) noexcept;
// That copy put back: each key made by the game's own add (its node, its place in the map), then given its kept value
// byte for byte; only into sessions with no keys of their own, only those on footage the replay still has and within what
// the editor shows. How many went back, or why none could.
std::variant<std::size_t, std::string> put_keys(std::uintptr_t base, std::string_view owner, std::uint64_t copy) noexcept;
// The owner's going (its plugin stopped): its copies go.
void forget(std::string_view owner) noexcept;

// Where the replay has the skater now (world): its body, its upper body and its board; nothing outside the editor.
struct Skater {
    std::array<float, 3> body{}, upper{}, board{};
};
std::optional<Skater> skater(std::uintptr_t base) noexcept;
} // namespace dingosdk::replay_editor
