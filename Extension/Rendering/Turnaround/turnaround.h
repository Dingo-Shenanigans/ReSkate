#pragma once
#include "turnaround_subject.h"
#include <string>
#include <string_view>

namespace dingosdk::overlay { struct FrameGrabHost; }
// Board turnarounds: a spinning picture of the board on the Board screen, saved as a GIF, APNG or
// WebM, optionally with a card of the parts it is made of.
namespace dingosdk::turnaround {
struct Options;
// What the overlay asks before drawing each frame (overlay::set_frame_grab); process lifetime.
const overlay::FrameGrabHost* frame_grab() noexcept;
// The options the next turnaround is made with, as the player left them (kept between sessions).
// set_option changes one (`turnaround set <key> <value>`) and says what it did.
Options& options() noexcept;
Options current_options();
std::string set_option(std::string_view key, std::string_view value);
// The last thing a turnaround said (its progress while one is being made), for the Addons page.
std::string last_message();
void open_folder();
// How a console command is run on the game thread (installed at startup): for engine settings.
void set_console_queue(bool (*queue)(const char* command)) noexcept;
void queue_console(const char* command) noexcept;
// Starts one with options(): of the board worn now (the Board screen) or of the skater in what they
// wear (the Skaters screen); what to tell the player.
std::string start_export(Subject subject);
// Which one V starts: the board on a Board tab's grid, the skater with no grid open in a game menu
// (the Skaters screen; other grids frame only part of the skater). False: neither.
bool v_turnaround(Subject& subject);
// Client thread: whether a game menu is up, for v_turnaround.
void set_game_menu(bool up) noexcept;
void cancel_export() noexcept;
bool exporting() noexcept;
} // namespace dingosdk::turnaround
