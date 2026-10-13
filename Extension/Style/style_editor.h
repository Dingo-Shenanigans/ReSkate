#pragma once
#include "Extension/Style/style_pose.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The style editor plays recorded clips on a stand-in in the menu's character room, with the current style.
namespace dingosdk::style_editor {
// The stand-in is a remote skater in the last remote-player slot, on this game only. Players take the lowest free
// slots, so a server reaches that slot only when it is full. The margin covers players who join before the editor's
// next tick.
inline constexpr std::size_t stand_in_slot = multiplayer::max_remote_players - 1, stand_in_margin = 8;
[[nodiscard]] constexpr bool stand_in_slot_free(std::size_t slots_in_use) noexcept {
    return slots_in_use + stand_in_margin < stand_in_slot;
}
[[nodiscard]] inline bool stand_in_slot_free() noexcept { return stand_in_slot_free(multiplayer_slots_in_use()); }
// Console adapters. Thread-safe. The console checks the range of each number.
// 0 clears the stand-in.
void request_show(std::uint8_t trick);
void request_hide();
// Records every trick that has no clip, a clip this version does not read, or an outdated clip, in one trip to the trick stage.
void request_record_tricks();
// Records one trick's clip on the trick stage.
void request_capture(std::uint8_t trick);
// Holds the clip at a timeline time (0 to 3). The stand-in eases to it.
void request_hold(float time) noexcept;
// Plays the clip in a loop from the shown moment, or pauses it there.
void request_play(bool play) noexcept;
// Moves a held clip by frames of 1/60 s. A playing clip is paused first.
void request_step(int frames) noexcept;
// Plays the clip at this fraction of its recorded speed, from the shown moment.
void request_speed(float speed) noexcept;
// Records the local skater for style_layer::recording_ms and keeps the jump in it as the trick's clip, if the skater did that trick.
// False with the reason.
bool request_record(std::uint8_t trick, std::string &error);
struct Recorded {
    std::uint8_t trick{};
    std::string failure;
};
// The end of the last recording of the local skater, once.
[[nodiscard]] std::optional<Recorded> take_recorded();
// 0 deletes all clips.
void request_forget(std::uint8_t trick);
// The menu room opens first, then the editor screen.
void open_editor();
// False when the close was not queued: try again.
bool close_editor();
// Closes the editor and hides the stand-in at once. Not from tick, which holds the editor's lock.
void close_and_hide();
// What runs each time the editor screen closes, by any path: the stand-in goes, the game leaves the menu room, no undo steps.
void screen_closed() noexcept;
void screen_open(bool open) noexcept;
// Thread-safe and lock-free.
[[nodiscard]] bool screen_open() noexcept;
// 0 keeps the current fov, as when no stand-in shows.
[[nodiscard]] float camera_fov() noexcept;
// Turns the editor camera around the stand-in (radians). Changes its distance and target height (metres).
void request_orbit(float yaw, float pitch, float distance, float height = 0) noexcept;
// Writes the editor camera pose (rows: right, up, backward, position). False when no stand-in shows. Client thread.
bool camera_pose(std::array<float, 16> &matrix) noexcept;
// Thread-safe.
[[nodiscard]] style::Playhead playhead() noexcept;
// Thread-safe.
void fill(style::StyleModel &model);
// Client thread only, after the native client tick.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::style_editor
