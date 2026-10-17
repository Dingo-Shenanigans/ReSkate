#include "map_teleports.h"
#include "map_teleports_model.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace dingosdk::map_teleports {
namespace {
// How close to its spot the skater must be before the teleport counts as arrived, and how
// long the game may take to send and finish it.
constexpr float arrived_within = 4.0f;
constexpr std::uint64_t arrive_ms = 6000;

struct Pending {
    Exit exit;
    std::string zone;
    std::uint64_t sent{};
};
struct State {
    std::string level;
    std::vector<Zone> zones;
    Tracker tracker;
    std::optional<Pending> pending;
    std::uint64_t next_look{};
};
State& state() {
    static auto* value = new State;
    return *value;
}
void say(logging::Level level, const std::string& text) { logging::write(level, logging::Channel::world, text); }

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
std::filesystem::path game_directory() {
    std::vector<wchar_t> exe(32768);
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    return std::filesystem::path(exe.data()).parent_path();
}
std::optional<Json> read_json(const std::filesystem::path& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 1024 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}
std::optional<Vec3> read_vec3(const Json& row, std::string_view key) {
    if (!row.contains(key)) return std::nullopt;
    const auto& value = row.at(key);
    if (!value.is_array() || value.size() != 3) return std::nullopt;
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number()) return std::nullopt;
        result[i] = value.at(i).get<float>();
    }
    return finite(result) ? std::optional<Vec3>(result) : std::nullopt;
}
float read_number(const Json& row, std::string_view key, float fallback) {
    return row.contains(key) && row.at(key).is_number() ? row.at(key).get<float>() : fallback;
}
// A zone the file gives as centre, full size, a turn, a spot and a heading. A flat panel (a
// size of 0) is given a few centimetres so a line through it still meets it.
std::optional<Zone> read_zone(const Json& row) {
    if (!row.is_object()) return std::nullopt;
    const auto center = read_vec3(row, "center"), size = read_vec3(row, "size"), target = read_vec3(row, "target");
    if (!center || !size || !target) return std::nullopt;
    Zone zone;
    zone.name = row.contains("name") && row.at("name").is_string() ? row.at("name").string() : "Teleport";
    zone.center = *center;
    for (std::size_t i = 0; i < 3; ++i) zone.half[i] = std::max((*size)[i] * 0.5f, 0.05f);
    zone.yaw = read_number(row, "yaw", 0.0f);
    zone.target = *target;
    zone.heading = wrap_heading(read_number(row, "heading", 0.0f));
    const auto flag = [&](std::string_view key) { return !row.contains(key) || !row.at(key).is_boolean() || row.at(key).get<bool>(); };
    zone.keep_speed = flag("keep_speed");
    zone.orient = flag("orient");
    return valid(zone) ? std::optional<Zone>(std::move(zone)) : std::nullopt;
}
// The zones of the mod whose teleports.json names this level, or whose reskate-levels.json adds it.
std::vector<Zone> read_zones(const std::string& level) {
    std::vector<Zone> zones;
    std::error_code error;
    for (std::filesystem::directory_iterator it(game_directory() / L"Mods", error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto json = read_json(it->path() / L"teleports.json");
        if (!json || !json->is_object()) continue;
        try {
            bool mine = false;
            if (json->contains("level") && json->at("level").is_string()) mine = lower(json->at("level").string()) == level;
            else if (const auto levels = read_json(it->path() / L"reskate-levels.json"); levels && levels->contains("levels"))
                for (const auto& row : levels->at("levels"))
                    if (row.is_object() && lower(row.value("asset", "")) == level) mine = true;
            if (!mine || !json->contains("zones") || !json->at("zones").is_array()) continue;
            std::size_t skipped = 0;
            for (const auto& row : json->at("zones")) {
                if (zones.size() >= max_zones) break;
                if (auto zone = read_zone(row)) zones.push_back(std::move(*zone));
                else ++skipped;
            }
            if (skipped)
                say(logging::Level::warning, std::format("Map teleports: {} zones in {} were left out (missing or bad values).",
                    skipped, it->path().filename().string()));
            return zones;
        } catch (...) {}
    }
    return zones;
}

// The file is read on a thread of its own and taken by the tick when it is there, as the
// trainer reads trainer.json: not on the client thread while the player spawns.
struct FileRead {
    std::mutex mutex;
    std::string level;
    std::optional<std::vector<Zone>> zones;
};
FileRead& file_read() {
    static auto* read = new FileRead; // process lifetime, as the threads that fill it
    return *read;
}
void start_read(const std::string& level) {
    auto& r = file_read();
    {
        std::lock_guard lock(r.mutex);
        r.level = level;
        r.zones.reset();
    }
    if (level.empty()) return;
    try {
        std::thread([level] {
            std::vector<Zone> zones;
            try { zones = read_zones(level); } catch (...) {}
            auto& r = file_read();
            std::lock_guard lock(r.mutex);
            if (r.level == level) r.zones = std::move(zones);
        }).detach();
    } catch (...) {}
}
void take_zones() {
    auto& s = state();
    auto& r = file_read();
    {
        std::lock_guard lock(r.mutex);
        if (!r.zones || r.level != s.level) return;
        s.zones = std::move(*r.zones);
        r.zones.reset();
    }
    s.tracker.reset();
    if (!s.zones.empty()) say(logging::Level::info, std::format("Map teleports: the map ships {} zones.", s.zones.size()));
}
void enter_level(const std::string& level) {
    auto& s = state();
    s.level = level;
    s.zones.clear();
    s.tracker.reset();
    s.pending.reset();
    start_read(level);
}

void report_carry() {
    switch (take_carry_speed_result()) {
    case -1: say(logging::Level::warning, "Map teleports: the game did not keep the carried speed."); break;
    case -2: say(logging::Level::info, "Map teleports: the skater came out off the board; no speed carried."); break;
    case -3: say(logging::Level::warning, "Map teleports: carrying the speed failed or timed out."); break;
    default: break;
    }
}
}

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string& level) noexcept {
    try {
        auto& s = state();
        if (const auto key = lower(playing ? level : std::string{}); key != s.level) enter_level(key);
        take_zones();
        report_carry();
        if (!playing || s.zones.empty()) return;
        const auto now = GetTickCount64();
        if (now < s.next_look) return;
        overlay::DebugModel skater;
        std::array<float, 16> matrix{};
        try {
            matrix = client_source::detail::debug_skater(base, client, skater);
        } catch (...) {
            skater.skater_position_valid = false;
        }
        if (!skater.skater_position_valid) {
            s.next_look = now + 250; // no skater: menus, loading, a respawn
            s.tracker.reset();
            return;
        }
        const Vec3 position = skater.skater_position;
        if (s.pending) {
            // Wait for the game to put the skater at the spot, then give its speed back.
            const auto& exit = s.pending->exit;
            const float away = std::hypot(position[0] - exit.position[0], position[2] - exit.position[2]);
            if (away <= arrived_within && std::abs(position[1] - exit.position[1]) <= arrived_within * 2) {
                if (exit.carry && !queue_carry_speed(client, skater.skater_identity, exit.velocity))
                    say(logging::Level::warning, "Map teleports: the speed could not be carried.");
                s.tracker.teleported(s.zones, exit.position, now);
                s.pending.reset();
            } else if (now > s.pending->sent + arrive_ms) {
                say(logging::Level::warning, std::format("Map teleports: \"{}\" did not arrive.", s.pending->zone));
                s.pending.reset();
                s.tracker.reset();
            }
            return;
        }
        const auto hit = s.tracker.step(s.zones, position, now);
        if (!hit) return;
        const auto& zone = s.zones[*hit];
        // Rows: right, up, forward, position.
        const float heading = wrap_heading(degrees(std::atan2(matrix[8], matrix[10])));
        const auto velocity = read_board_velocity(client, skater.skater_identity).value_or(Vec3{});
        const auto exit = exit_for(zone, heading, velocity);
        if (!teleport_local_skater(exit.position, exit.heading)) {
            say(logging::Level::warning, std::format("Map teleports: \"{}\" could not teleport right now.", zone.name));
            s.tracker.teleported(s.zones, position, now); // try again once the skater has left it
            return;
        }
        s.pending = Pending{exit, zone.name, now};
        s.tracker.reset();
        logging::log(logging::Level::info, logging::Channel::world, "Map teleports: \"{}\" at {:.1f} m/s, out facing {:.0f}.",
            zone.name, ground_speed(velocity), exit.heading);
    } catch (...) {}
}
}
