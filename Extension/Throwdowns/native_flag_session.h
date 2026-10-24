#pragma once
#include <array>
#include <cstdint>
#include <string>

// The native flag registration behind kit::flag (throwdown_kit.h): one plugin mode at a time
// drives the stock Spot Battle setup page, native flag placement and the native waiting card.
// These are the hooks the native layers feed it (throwdown_lab, native_throwdowns, the native
// menu); nothing here knows a particular mode.
namespace dingosdk::multiplayer::menu_data { class Context; }
namespace dingosdk::multiplayer::native_flag {
// ---- queries (any thread) -----------------------------------------------------------------------
// The setup page, flag placement or its teardown is under way (not the waiting card).
bool placement_active() noexcept;
// Includes the placed registration while its native waiting card owns the flag.
bool registration_owned() noexcept;
// Only a confirmed, handed-off flag owns the waiting card's Start/Leave.
bool registration_waiting() noexcept;
bool registration_retiring() noexcept;
bool owns_registration(std::uint32_t mmid) noexcept;
// The local player placed the registration's flag (it was not adopted through Join); true until
// the registration ends, through the match it started.
bool hosting() noexcept;
// The stock setup page is showing for the registration's mode.
bool setup_active() noexcept;
// The registration's mode ("<plugin>/<id>") and game; empty / 0 without one.
std::string mode();
std::uint32_t game() noexcept;
// > 0 while the placement cursor may rise this far (Options::cursor_height), else 0.
float cursor_height() noexcept;
struct Limits { unsigned seconds{}, players{}; };
// The registration's selected limits (the setup page's, once changed there).
Limits limits() noexcept;
std::string status();

// ---- what the native layers report ----------------------------------------------------------
// The setup page's Players / timer changed (posted to the plugin as setup_row by the page itself).
void set_setup_limits(unsigned seconds, unsigned players) noexcept;
// A fresh grid selection supersedes an unconfirmed setup. No placed flag is discarded.
void retire_unconfirmed_setup() noexcept;
// The waiting card's Start / Leave: posted to the owner's plugin as start / leave. Leave also
// ends the registration. False without a waiting registration.
bool start() noexcept;
bool leave() noexcept;
// The pause menu's / native Quit while a plugin match runs (kit::active()): its plugin hears
// quit. False when no plugin match is running.
bool quit_match() noexcept;
// The native setup navigation, placement teardown and the owner's documents. UI thread.
void tick(const menu_data::Context& context);
void observe_graph(std::uint32_t graph) noexcept;
void observe_created() noexcept;
// Keep custom queue identities through their asynchronous native teardown: late joins of those
// queues must not revive a stock Spot Battle notification.
void observe_queue(std::uint32_t mmid) noexcept;
bool known_queue(std::uint32_t mmid) noexcept;
void observe_position(std::uint32_t mmid, const std::array<float,3>& spawn, const std::array<float,16>* transform=nullptr) noexcept;
void observe_exit(bool cancelling) noexcept;
void observe_destroyed(std::uint32_t mmid) noexcept;
// The level unloads every native queue.
void abandon() noexcept;
}
