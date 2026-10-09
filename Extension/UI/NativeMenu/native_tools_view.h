#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include "Engine/Game/World/world_names.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace dingosdk::native_tools {
inline constexpr std::string_view root_asset = "Levels/Game/DingoLevel_Root/DingoLevel_Root";
inline constexpr std::array<const char*, 5> sections{"World", "Custom Maps", "Parks", "Player", "Visuals"};
enum Section : unsigned { world_section, custom_section, parks_section, player_section, visuals_section };
// The ReSkate splash level only hosts the startup screen; it is never a destination.
inline constexpr std::string_view splash_asset = "DingoLevel_Splash";
struct State {
    std::string destination, feedback;
    unsigned lot{};
    ParkChoices parks, seen_parks;
};
struct Row {
    std::string id, title, command, argument;
    bool button{}, primary{};
    bool input{}; // title: placeholder, argument: initial text.
};
struct Page { std::vector<Row> main, side; };
inline bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}
inline const overlay::Level* level(const overlay::Model& model, std::string_view asset) {
    const auto it = std::find_if(model.levels.begin(), model.levels.end(),
        [&](const auto& entry) { return same(entry.asset, asset); });
    return it == model.levels.end() ? nullptr : &*it;
}
inline bool listed(const overlay::Level& entry) {
    if (same(entry.asset, root_asset)) return false;
    const auto name = std::string_view(entry.asset).substr(entry.asset.find_last_of("/\\") + 1);
    return !same(name, splash_asset);
}
inline std::string level_name(const overlay::Level& entry) {
    return entry.display_name.empty() ? world_level_name(entry.asset) : entry.display_name;
}
inline std::vector<std::string> layouts(unsigned lot) {
    std::vector<std::string> result{"empty"};
    for (unsigned family = 0; family < park_families.size(); ++family)
        for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant)
            result.push_back(park_id(family, variant));
    return result;
}
inline void sync(State& s, const overlay::Model& model) {
    if (!level(model, s.destination)) s.destination.clear();
    for (unsigned i = 0; i < park_lots.size(); ++i) {
        if (s.parks[i].empty() || s.seen_parks[i] != model.parks.choices[i]) {
            s.seen_parks[i] = model.parks.choices[i];
            s.parks[i] = valid_park(i, s.seen_parks[i]) && !s.seen_parks[i].empty() ? s.seen_parks[i] : "empty";
        }
    }
}
inline bool can_load(const State& s, const overlay::Model& m, const overlay::CallbacksV3& cb) {
    const auto* root = level(m, root_asset);
    const auto* selected = level(m, s.destination);
    return root && selected && root != selected && root->native_registered && root->can_load &&
        selected->can_load && m.can_queue_load && !multiplayer_controls_level(m.multiplayer) && cb.queue_load;
}
// Layers and parks the host controls are locked, except for a dedicated
// server's admins: their changes go to the server, which applies them to everyone.
inline bool layers_locked(const overlay::Model& m) { return m.world.controlled_by_host && !m.multiplayer.server_admin; }
inline bool parks_locked(const overlay::Model& m) { return m.parks.controlled_by_host && !m.multiplayer.server_admin; }
inline bool can_park(const overlay::Model& m, const overlay::CallbacksV3& cb) {
    return m.world.map == WorldMap::bam && m.parks.available && !parks_locked(m) && cb.queue_console_command;
}
// Time of day is a group of seven world layers per map, "<map>_tod_<n>". The
// selector forces one on and the others off; Default returns all seven to the
// level's own choice.
inline constexpr std::array<const char*, 7> time_of_day_keys{
    "1_morning", "2_noon", "3_afternoon", "4_evening", "5_night", "6_weatherday", "7_weathernight"};
inline constexpr std::array<const char*, 8> time_of_day_labels{
    "Default", "Morning", "Noon", "Afternoon", "Evening", "Night", "Weather (day)", "Weather (night)"};
inline std::size_t time_of_day_layer(WorldMap map, unsigned slot) {
    const auto key = std::string(world_map_key(map)) + "_tod_" + time_of_day_keys[slot];
    for (std::size_t i = 0; i < world_layers().size(); ++i) if (world_layers()[i].key == key) return i;
    return world_layers().size();
}
inline bool time_of_day_available(const overlay::Model& m, const overlay::CallbacksV3& cb) {
    if (!m.world.available || !m.world.ready || layers_locked(m) || !cb.queue_console_command) return false;
    for (unsigned slot = 0; slot < time_of_day_keys.size(); ++slot) {
        const auto layer = time_of_day_layer(m.world.map, slot);
        if (layer == world_layers().size() || !m.world.supported[layer]) return false;
    }
    return true;
}
// 0 while no single time is forced, otherwise 1..7.
inline unsigned time_of_day(const overlay::Model& m) {
    unsigned forced{};
    for (unsigned slot = 0; slot < time_of_day_keys.size(); ++slot) {
        const auto layer = time_of_day_layer(m.world.map, slot);
        if (layer == world_layers().size()) return 0;
        const auto& choice = m.world.choices[layer];
        if (choice == "on") { if (forced) return 0; forced = slot + 1; }
        else if (choice != "off") return 0;
    }
    return forced;
}
// The console lines that select a time of day (0: the level's own). The others
// switch off before the new one comes on, so two never load together. A
// dedicated server's admin sends them to the server as one request.
inline std::vector<std::string> time_of_day_commands(const overlay::Model& m, unsigned next) {
    std::vector<std::string> commands, pairs;
    for (unsigned slot = 0; slot < time_of_day_keys.size(); ++slot) {
        const auto mode = !next ? "default" : slot + 1 == next ? "on" : "off";
        const auto& key = world_layers()[time_of_day_layer(m.world.map, slot)].key;
        const auto line = "world " + std::string(key) + " " + mode;
        if (slot + 1 == next) { commands.push_back(line); pairs.push_back(std::string(key) + "=" + mode); }
        else { commands.insert(commands.begin(), line); pairs.insert(pairs.begin(), std::string(key) + "=" + mode); }
    }
    if (m.multiplayer.server_admin && m.world.controlled_by_host) {
        std::string batch = "mp server layers";
        for (const auto& pair : pairs) batch += " " + pair;
        return {batch};
    }
    return commands;
}
inline void cycle(std::string& current, const std::vector<std::string>& choices) {
    if (choices.empty()) return;
    const auto at = std::find(choices.begin(), choices.end(), current);
    current = at == choices.end() || at + 1 == choices.end() ? choices.front() : *(at + 1);
}
// Run outside the native model lock. Every activation rechecks the latest
// runtime snapshot, including level transitions and multiplayer host ownership.
inline void activate(State& s, const overlay::Model& m, const overlay::CallbacksV3& cb,
                     std::string_view command, const std::string& argument) {
    sync(s, m);
    s.feedback.clear();
    std::array<char, 512> result{};
    const auto report = [&](bool accepted) {
        result.back() = '\0';
        s.feedback = accepted ? "" : "Couldn't apply that change. Try again when the game is ready.";
    };
    const auto console = [&](const std::string& text, bool enabled) {
        const bool accepted = enabled && cb.queue_console_command && cb.queue_console_command(cb.user, text.c_str(), result.data(), result.size());
        report(accepted);
        return accepted;
    };
    if (command == "select-level") {
        if (multiplayer_controls_level(m.multiplayer)) { s.feedback = "Only the lobby host can change levels."; return; }
        if (const auto* selected = level(m, argument); selected && listed(*selected)) {
            s.destination = selected->asset;
        }
    } else if (command == "load-level") {
        if (multiplayer_controls_level(m.multiplayer)) { s.feedback = "Only the lobby host can change levels."; return; }
        if (!can_load(s, m, cb)) { s.feedback = "Choose an available level once the game is ready."; return; }
        const auto* root = level(m, root_asset);
        const auto* selected = level(m, s.destination);
        const auto* point = selected->automatic_start_point();
        report(cb.queue_load(cb.user, root->asset.c_str(), "", selected->asset.c_str(), point ? point->c_str() : "", result.data(), result.size()));
    } else if (command == "traffic" || command == "pedestrians") {
        const auto current = command == "traffic" ? m.world_controls.choices.traffic : m.world_controls.choices.pedestrians;
        console("environment " + std::string(command) + " " + std::to_string(current >= 3 ? -1 : current + 1),
            m.world.ready && m.world_controls.population_available);
    } else if (command == "time-of-day") {
        if (!time_of_day_available(m, cb)) { s.feedback = "Time of day is unavailable on this map right now."; return; }
        const auto next = static_cast<unsigned>((time_of_day(m) + 1) % time_of_day_labels.size());
        for (const auto& line : time_of_day_commands(m, next)) console(line, true);
    } else if (command == "park-lot") {
        for (unsigned i = 0; i < park_lots.size(); ++i) if (park_lots[i].key == argument) s.lot = i;
    } else if (command == "park-layout") {
        cycle(s.parks[s.lot], layouts(s.lot));
    } else if (command == "park-family") {
        unsigned family = 0;
        for (unsigned i = 0; i < park_families.size(); ++i)
            if (s.parks[s.lot].starts_with(park_families[i])) family = i + 1;
        s.parks[s.lot] = family == park_families.size() ? "empty" : park_id(family, 1);
    } else if (command == "load-park") {
        console("park " + std::string(park_lots[s.lot].key) + " " + s.parks[s.lot], can_park(m, cb) && valid_park(s.lot, s.parks[s.lot]));
    } else if (command == "load-random-parks") {
        // Use the shared command path: hosts load locally, server admins request
        // one server roll, and guests cannot replace the host's layouts.
        if (console("park random", can_park(m, cb))) s.parks = {};
    } else if (command == "park-random-on-launch") {
        // This is a personal preference, including while following a host.
        console(m.parks.randomize_on_launch ? "park random-on-launch 0" : "park random-on-launch 1",
            m.parks.available);
    } else if (command == "noclip" || command == "no-bail" || command == "freecam" || command == "speed") {
        const auto& d = m.debug;
        overlay::DebugRequest request;
        bool enabled = false;
        if (command == "noclip") { request = {overlay::DebugAction::set_noclip, !d.noclip}; enabled = d.noclip_available || d.noclip; }
        if (command == "no-bail") { request = {overlay::DebugAction::set_no_bail, !d.no_bail}; enabled = d.no_bail_available || d.no_bail; }
        if (command == "freecam") { request = {overlay::DebugAction::set_free_camera, !d.free_camera}; enabled = d.available && d.camera_available; }
        if (command == "speed") {
            constexpr std::array<float, 7> speeds{.6f, 3.f, 5.f, 15.f, 60.f, 300.f, 1500.f};
            const auto next = std::upper_bound(speeds.begin(), speeds.end(), d.camera_speed);
            request = {overlay::DebugAction::set_camera_speed, false, next == speeds.end() ? speeds.front() : *next};
            enabled = d.camera_available && (d.free_camera || d.noclip);
        }
        report(enabled && cb.queue_debug && cb.queue_debug(cb.user, request, result.data(), result.size()));
    } else if (command == "board-wear") {
        const auto& wear = m.offline.board_wear;
        const overlay::OfflineFeatureRequest request{overlay::OfflineFeatureGroup::board_wear, !wear.effective};
        report(wear.available && cb.queue_offline_feature &&
               cb.queue_offline_feature(cb.user, request, result.data(), result.size()));
    } else if (command == "hall-of-meat") {
        console(m.hall_of_meat.enabled ? "hallofmeat 0" : "hallofmeat 1", m.hall_of_meat.available);
    } else if (command == "board-wear-reset") {
        console("boardwear reset", m.offline.board_wear.available && m.offline.board_wear.effective);
    } else if (command == "challenges") {
        console(std::string("challenges ") + (m.progression.challenges_hidden ? "1" : "0"), m.progression.challenges_enabled);
    } else if (command == "graphics") {
        for (unsigned i = 0; i < graphics_keys.size(); ++i) if (argument == graphics_keys[i]) {
            const auto choice = m.graphics.choices.effects[i];
            const bool on = choice < 0 ? !m.graphics.ready[i] || m.graphics.enabled[i] : choice != 0;
            console("graphics " + argument + (on ? " 0" : " 1"), m.graphics.available);
        }
    } else if (command == "graphics-reset") console("graphics reset -1", m.graphics.available);
}
inline Page render(State& s, const overlay::Model& m, const overlay::CallbacksV3& cb, unsigned section) {
    sync(s, m);
    Page p;
    const auto text = [](auto& rows, std::string id, std::string title) {
        if (!title.empty()) rows.push_back(Row{std::move(id), std::move(title), {}, {}, false, false});
    };
    const auto button = [](auto& rows, std::string id, std::string title, std::string command,
                           bool enabled = true, std::string argument = {}, bool primary = false) {
        rows.push_back(Row{std::move(id), std::move(title), enabled ? std::move(command) : "", std::move(argument), true, primary});
    };
    const auto toggle = [&](auto& rows, const char* id, const char* title, bool on, bool enabled) {
        button(rows, id, std::string(title) + (on ? ": On" : ": Off"), id, enabled);
    };
    // World lists the game's own levels, Custom Maps the ones mods declare.
    const auto destinations = [&](bool custom) {
        std::vector<const overlay::Level*> result;
        for (const auto& entry : m.levels) if (listed(entry) && entry.custom == custom) result.push_back(&entry);
        std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) { return level_name(*a) < level_name(*b); });
        for (const auto* entry : result)
            button(p.main, "level-" + entry->asset, level_name(*entry) + (same(entry->asset, s.destination) ? "  /  SELECTED" : ""),
                "select-level", !multiplayer_controls_level(m.multiplayer), entry->asset);
        return result.size();
    };
    const auto* selected = level(m, s.destination);
    const auto load_status = [&](const char* id) {
        if (multiplayer_controls_level(m.multiplayer))
            text(p.side, id, "Only the lobby host can change levels.");
        else if (!m.can_queue_load || (selected && !selected->can_load))
            text(p.side, id, "Travel is unavailable right now. Wait for the game to finish loading.");
    };
    if (section == world_section) {
        text(p.main, "levels-title", "FIND YOUR SPOT");
        if (!destinations(false)) text(p.main, "levels-empty", "Waiting for available levels...");
        text(p.side, "world-title", "TRAVEL & POPULATION");
        button(p.side, "load-level", "Load selected level", "load-level", can_load(s, m, cb), {}, true);
        for (unsigned i = 0; i < 2; ++i) {
            const auto choice = i == 0 ? m.world_controls.choices.traffic : m.world_controls.choices.pedestrians;
            button(p.side, i == 0 ? "traffic" : "pedestrians", std::string(i == 0 ? "Traffic: " : "Pedestrians: ") +
                population_labels[std::clamp(choice, -1, 3) + 1], i == 0 ? "traffic" : "pedestrians",
                m.world.ready && m.world_controls.population_available && cb.queue_console_command);
        }
        button(p.side, "time-of-day", std::string("Time of day: ") + time_of_day_labels[time_of_day(m)],
            "time-of-day", time_of_day_available(m, cb));
        if (m.world.controlled_by_host)
            text(p.side, "time-status", m.multiplayer.server_admin ? "You are an admin: time of day changes for the whole server."
                                                                   : "The lobby host controls world layers, including time of day.");
        load_status("load-status");
    } else if (section == custom_section) {
        text(p.main, "custom-levels-title", "CUSTOM MAPS");
        if (!destinations(true)) {
            text(p.main, "custom-levels-empty", "No custom maps installed.");
            text(p.main, "custom-levels-help", "Custom maps from the Mods folder appear here.");
        }
        text(p.side, "custom-title", "TRAVEL");
        text(p.side, "custom-selected", selected && selected->custom ? level_name(*selected) : "Choose a map to load.");
        button(p.side, "custom-load-level", "Load selected map", "load-level",
            selected && selected->custom && can_load(s, m, cb), {}, true);
        load_status("custom-load-status");
    } else if (section == parks_section) {
        text(p.main, "parks-title", "CHOOSE A PARK LOT");
        for (unsigned i = 0; i < park_lots.size(); ++i)
            button(p.main, "park-lot-" + std::to_string(i), std::string(park_lots[i].label) + (s.lot == i ? "  /  SELECTED" : ""),
                "park-lot", true, std::string(park_lots[i].key));
        text(p.main, "park-random-title", "ALL THREE PARK LOCATIONS");
        button(p.main, "load-random-parks", "Load Random Parks", "load-random-parks", can_park(m, cb));
        toggle(p.main, "park-random-on-launch", "Randomize on Launch", m.parks.randomize_on_launch,
            m.parks.available && cb.queue_console_command);
        text(p.side, "park-options", "PARK LAYOUT");
        const bool available = can_park(m, cb);
        std::string family = "Empty lot";
        for (unsigned i = 0; i < park_families.size(); ++i) if (s.parks[s.lot].starts_with(park_families[i])) family = park_family_labels[i];
        button(p.side, "park-family", "Style: " + family, "park-family", available);
        button(p.side, "park-layout", park_label(s.parks[s.lot]), "park-layout", available);
        button(p.side, "load-park", "Load park layout", "load-park", available, {}, true);
        text(p.side, "park-status", m.world.map != WorldMap::bam ? "Load San Vansterdam to change parks." :
            parks_locked(m) ? "The lobby host controls park layouts."
            : m.parks.controlled_by_host ? "You are an admin: layouts you load change for the whole server." : "");
    } else if (section == player_section) {
        text(p.main, "player-title", "SKATE YOUR WAY");
        toggle(p.main, "noclip", "Noclip", m.debug.noclip, (m.debug.noclip_available || m.debug.noclip) && cb.queue_debug);
        toggle(p.main, "no-bail", "No bail", m.debug.no_bail, (m.debug.no_bail_available || m.debug.no_bail) && cb.queue_debug);
        toggle(p.main, "freecam", "Freecam", m.debug.free_camera, m.debug.available && m.debug.camera_available && cb.queue_debug);
        toggle(p.main, "hall-of-meat", "Hall of Meat", m.hall_of_meat.enabled, m.hall_of_meat.available && cb.queue_console_command);
        toggle(p.main, "board-wear", "Board wear", m.offline.board_wear.effective,
            m.offline.board_wear.available && cb.queue_offline_feature);
        button(p.main, "board-wear-reset", "Reset board wear", "board-wear-reset",
            m.offline.board_wear.available && m.offline.board_wear.effective && cb.queue_console_command);
        text(p.side, "flight-title", "FREE FLIGHT");
        const auto speed = std::to_string(m.debug.camera_speed);
        button(p.side, "speed", "Flight speed: " + speed.substr(0, speed.find('.') + 2), "speed",
            (m.debug.free_camera || m.debug.noclip) && m.debug.camera_available && cb.queue_debug);
        text(p.side, "flight-help", "Close the menu to use flight controls.");
        // Local card only. The native menu reads this field under its model lock.
        text(p.side, "card-title", "PLAYER CARD NAME");
        p.side.push_back(Row{"card-name", m.multiplayer.local_name.empty() ? "Your Steam name" : m.multiplayer.local_name,
            {}, m.player_card.custom_name, false, false, true});
        button(p.side, "card-name-save", "Save card name", "card-name", m.player_card.available, {}, true);
        button(p.side, "card-name-reset", "Use Steam name", "card-name-reset",
            m.player_card.available && !m.player_card.custom_name.empty());
    } else {
        text(p.main, "visuals-title", "CAMERA EFFECTS");
        constexpr std::array<const char*, 3> names{"Film grain", "Vignette", "Chromatic aberration"};
        for (unsigned i = 0; i < names.size(); ++i) {
            const auto choice = m.graphics.choices.effects[i];
            const bool on = choice < 0 ? !m.graphics.ready[i] || m.graphics.enabled[i] : choice != 0;
            button(p.main, graphics_keys[i], std::string(names[i]) + (on ? ": On" : ": Off") + (choice < 0 ? " (Default)" : ""),
                "graphics", m.graphics.available && cb.queue_console_command, graphics_keys[i]);
        }
        button(p.main, "graphics-reset", "Restore default effects", "graphics-reset", m.graphics.available && cb.queue_console_command);
        text(p.side, "hud-title", "CHALLENGES");
        button(p.side, "challenges", m.progression.challenges_hidden ? "Challenges: Hidden" : "Challenges: Shown", "challenges",
            m.progression.challenges_enabled && cb.queue_console_command);
        text(p.side, "challenges-help", "Showing or hiding keeps your progress.");
    }
    text(p.side, "tools-feedback-" + std::to_string(section), s.feedback);
    return p;
}
} // namespace dingosdk::native_tools
