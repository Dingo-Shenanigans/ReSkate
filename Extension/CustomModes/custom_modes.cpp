#include "custom_modes.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Extension/Multiplayer/Session/session.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>

namespace dingosdk::custom_modes {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto host_patience = std::chrono::minutes(4);

struct PendingHost {
    std::string mode, name;
    bool public_lobby{};
    unsigned capacity{};
    Clock::time_point asked;
};
struct State {
    std::mutex mutex;
    std::vector<Mode> modes;
    std::optional<PendingHost> pending;
    std::string status;
    std::string advertising;        // the mode this player's hosted game advertises
    std::uint64_t handled_lobby{};  // the joined game whose mode was told `joined` already
};
State &state() {
    static auto *value = new State;
    return *value;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::runtime, "Custom modes: " + text); }

const Mode *find(const State &s, std::string_view id) {
    for (const auto &mode : s.modes)
        if (mode.id == id) return &mode;
    return nullptr;
}
Panel panel_of(const Mode &mode) {
    try {
        return mode.panel();
    } catch (const std::exception &error) {
        Panel panel;
        panel.available = false;
        panel.unavailable = std::string("This mode could not show its panel: ") + error.what();
        return panel;
    }
}
} // namespace

bool add(Mode mode) noexcept {
    try {
        if (!valid_mode_id(mode.id) || mode.name.empty() || !mode.panel) return false;
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (find(s, mode.id)) return false;
        s.modes.push_back(std::move(mode));
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t count() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.modes.size();
}

std::vector<Listed> list() {
    std::vector<Mode> modes;
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        modes = s.modes; // panels are asked for outside the lock: a mode may take its own
    }
    std::vector<Listed> result;
    for (const auto &mode : modes) result.push_back({mode.id, mode.name, mode.tagline, panel_of(mode)});
    return result;
}

std::string host(std::string_view id, bool public_lobby, unsigned capacity, std::string_view name) {
    auto &s = state();
    std::unique_lock lock(s.mutex);
    // (A refusal is also the hosting status, so the menu that asked says why.)
    const auto refuse = [&](std::string why) {
        s.status = why;
        return "error: " + why;
    };
    const auto *mode = find(s, id);
    if (!mode) return refuse("No custom mode " + std::string(id) + " (modes list shows them).");
    if (launcher::offline_mode()) return refuse("Multiplayer is off in offline mode.");
    const auto m = multiplayer::model();
    if (m.active && !m.hosting) return refuse("You are in someone else's game; leave it first.");
    if (m.active && m.hosting) {
        // Already hosting: the game advertises this mode from now on.
        s.advertising = mode->id;
        multiplayer::set_advertised_mode(mode->id);
        s.status = "Your game now advertises " + mode->name + ".";
        return s.status;
    }
    capacity = std::clamp(capacity, 2u, static_cast<unsigned>(multiplayer_lobby_player_limit));
    s.pending = PendingHost{mode->id, std::string(name), public_lobby, capacity, Clock::now()};
    const auto mode_copy = *mode;
    lock.unlock();
    const auto panel = panel_of(mode_copy);
    lock.lock();
    s.status = panel.map.empty() || panel.map_here ? "Hosting a " + mode_copy.name + " game..."
                                                   : "Hosting a " + mode_copy.name + " game once " + panel.map_name + " has loaded...";
    say(logging::Level::info, s.status);
    return s.status + (panel.map.empty() || panel.map_here ? "" : " (load " + panel.map + ")");
}

std::string cancel_host() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.pending) return "Nothing is waiting to be hosted.";
    s.pending.reset();
    s.status = "Hosting cancelled.";
    return s.status;
}

HostState host_state() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return {s.pending ? s.pending->mode : s.advertising, s.status, s.pending.has_value()};
}

void tick() noexcept {
    try {
        auto &s = state();
        std::unique_lock lock(s.mutex);
        if (s.modes.empty()) return;
        const auto m = multiplayer::model();
        // A game was hosted for a mode: once the session is over, nothing is advertised.
        if (!m.active && !s.advertising.empty() && !s.pending) {
            s.advertising.clear();
            multiplayer::set_advertised_mode({});
        }
        if (!m.active) s.handled_lobby = 0;
        // Waiting to host: once the mode's map is in and the player is in game.
        if (s.pending) {
            auto pending = *s.pending;
            const auto *mode = find(s, pending.mode);
            if (!mode || Clock::now() - pending.asked > host_patience) {
                s.pending.reset();
                s.status = "Hosting gave up: the map did not load in time.";
                return;
            }
            const auto mode_copy = *mode;
            lock.unlock();
            const auto panel = panel_of(mode_copy);
            const bool ready = (panel.map.empty() || panel.map_here) && m.local_ready && !m.active;
            if (!ready) return;
            const auto tps = m.saved_host.tps ? m.saved_host.tps : 30u;
            const auto name = !pending.name.empty() ? pending.name
                            : !m.saved_host.lobby_name.empty() ? m.saved_host.lobby_name : mode_copy.name + " game";
            multiplayer::set_advertised_mode(mode_copy.id);
            const bool queued = multiplayer::queue_command(
                "host-config", std::format("{} {} {} {}", pending.public_lobby ? "public" : "code", pending.capacity, tps, name), {});
            lock.lock();
            s.pending.reset();
            s.advertising = queued ? mode_copy.id : std::string();
            s.status = queued ? (pending.public_lobby ? "Hosting a public " : "Hosting a join-code ") + mode_copy.name + " game."
                              : "Hosting failed: the multiplayer queue is busy; try again.";
            say(logging::Level::info, s.status);
            return;
        }
        // Joined a game advertised for a mode: tell the mode, once.
        if (m.active && !m.hosting && m.connected && m.public_lobby && m.public_lobby != s.handled_lobby) {
            const auto row = std::ranges::find_if(m.lobbies, [&](const MultiplayerLobby &lobby) { return lobby.id == m.public_lobby; });
            if (row == m.lobbies.end()) return;
            s.handled_lobby = m.public_lobby;
            if (const auto *mode = find(s, row->mode); mode && mode->joined) {
                const auto joined = mode->joined;
                const auto name = mode->name;
                lock.unlock();
                say(logging::Level::info, "joined a " + name + " game");
                joined();
            }
        }
    } catch (...) {
    }
}
}
