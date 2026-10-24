#include "plugin_game_runtime.h"
#include "plugin_session_feed.h"
#include "plugin_content.h"
#include "Runtime/runtime_internal.h"
#include "plugin_ui.h"
#include "Engine/Game/World/park_rotation.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Game/World/world_layer_catalog.h"
#include "Engine/Game/World/world_layer_types.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/skater_animation.h"
#include "Extension/AnimationStudio/game_clips.h"
#include "Extension/Plugins/plugin_joystick.h"
#include "plugin_sound.h"
#include "Extension/Skater/skater_events.h"
#include "Extension/Skater/outfit_overlay.h"
#include "Extension/Skater/board_pose.h"
#include "Extension/Customization/board_material.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Core/Log/logging.h"
#include <mutex>
#include "Extension/Assets/live_mods.h"
#include "Extension/Skater/game_speed.h"
#include "Extension/Skater/launches.h"
#include "Extension/Skater/pad_hide.h"
#include "Engine/Game/Skater/first_person_cycle.h"
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include "Extension/Skater/xray.h"
#include "Extension/UI/hud_corner.h"
#include "Extension/Throwdowns/custom_modes.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "plugin_camera.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Music/external_player.h"
#include "Extension/Music/spotify_station.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Settings/named_settings.h"
#include "Engine/Game/Settings/multiplayer_settings_lock.h"
#include "Extension/Skater/ai_skaters.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Throwdowns/throwdown_debug_text.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/Travel/local_location_travel.h"
#include "Extension/World/world_entities.h"
#include "Extension/Skater/bone_cam.h"
#include "Extension/Skater/pad_flight.h"
#include "Extension/Skater/trick_feed.h"
#include "Extension/Skater/skater_push.h"
#include "Extension/Skater/trick_gestures.h"
#include "Extension/Skater/trick_rewrite.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Console/commands.h"
#include "Extension/Settings/local_gameplay_settings.h"
#include "Extension/Skater/physics_tuning.h"
#include "Engine/Game/Multiplayer/session_competition.h"
#include "Extension/UI/NativeMenu/throwdown_menu.h"
#include <filesystem>
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/screen_look.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include "Extension/World/character_room.h"
#include "Extension/Rendering/Capture/capture_runtime.h"
#include "Extension/Customization/item_browser.h"
#include "Extension/Customization/worn_items.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace dingosdk::plugins {
// Game settings plugins hold (reskate.settings.set): what each found and wrote. Client thread (the
// game's update thread, which named settings need).
namespace held_settings {
namespace {
struct Held {
    std::string plugin, name, found, ours;
};
std::vector<Held>& list() {
    static auto* value = new std::vector<Held>; // held settings may still be out at exit
    return *value;
}
bool failed(const std::string& result) { return result.starts_with("error: "); }
std::string why_of(const std::string& result) { return failed(result) ? result.substr(7) : result; }
// Put back what was found, unless the setting no longer holds ours (changed elsewhere: it stays).
void put_back(const Held& h) {
    if (named_setting_value(h.name) == h.ours) (void)change_named_setting(h.name, h.found, false);
}
}
std::string set(std::string_view plugin, std::string_view name, std::optional<std::string> value) {
    auto& held = list();
    const auto at = std::find_if(held.begin(), held.end(), [&](const Held& h) { return console::equal(h.name, name); });
    if (!value) {
        if (at != held.end() && at->plugin == plugin) {
            put_back(*at);
            held.erase(at);
        }
        return {};
    }
    const auto now = named_setting_value(name);
    if (!now) return "no game setting is called \"" + std::string(name) + "\"";
    if (at != held.end() && at->plugin != plugin) return at->plugin + " holds " + at->name;
    if (multiplayer_settings_locked() && locked_in_multiplayer(name))
        return std::string(name) + " changes how the game plays, so it cannot be changed during a multiplayer session";
    if (at == held.end() && std::count_if(held.begin(), held.end(), [&](const Held& h) { return h.plugin == plugin; }) >= 32)
        return "a plugin holds at most 32 settings";
    const auto result = change_named_setting(name, *value, false);
    if (failed(result)) return why_of(result);
    const auto ours = named_setting_value(name).value_or(*value);
    if (at != held.end()) at->ours = ours;
    else held.push_back({std::string(plugin), std::string(name), *now, ours});
    return {};
}
void release(std::string_view plugin) {
    auto& held = list();
    for (auto it = held.end(); it != held.begin();) {
        --it;
        if (it->plugin == plugin) {
            put_back(*it);
            it = held.erase(it);
        }
    }
}
}
void tick_plugin_settings() noexcept {
    try {
        if (!multiplayer_settings_locked()) return;
        auto& held = held_settings::list();
        for (auto it = held.begin(); it != held.end();) {
            if (locked_in_multiplayer(it->name)) {
                held_settings::put_back(*it);
                it = held.erase(it);
            } else ++it;
        }
    } catch (...) {}
}

namespace {
// A request on the queue the console's own debug commands use; its outcome shows in the console's
// model a tick later, so "" here means it was queued.
std::string queue(overlay::DebugAction action, bool enabled = false, float value = 0) {
    std::array<char, 512> reply{};
    const overlay::DebugRequest request{action, enabled, value};
    if (runtime::detail::queue_debug(nullptr, request, reply.data(), reply.size())) return {};
    reply.back() = '\0';
    return reply[0] ? std::string(reply.data()) : std::string("the game is busy; try again");
}

// The game's folder (the executable's), where Mods is.
std::filesystem::path game_folder() {
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) throw std::runtime_error("The game folder is unavailable.");
    return std::filesystem::path(executable.data()).parent_path();
}

class RealGame final : public Game {
public:
    explicit RealGame(std::uintptr_t base) : base_(base) {}

    // ---- the local skater ----------------------------------------------------------------------
    std::optional<std::uint32_t> physics_state() override {
        const auto t = trainer::telemetry();
        if (!t.skater) return std::nullopt;
        return t.physics_state;
    }
    std::uint64_t wipeouts() override { return watched_physics_state().wipeouts; }
    std::string control(std::string_view action) override {
        using overlay::DebugAction;
        if (action == "noclip_on" || action == "noclip_off") {
            const bool on = action == "noclip_on";
            if (on && !session_noclip_allowed()) return "the host has turned noclip off";
            return queue(DebugAction::set_noclip, on);
        }
        if (!session_boosts_allowed()) return "the host has turned boosts off";
        if (action == "boost_forward") return queue(DebugAction::add_forward_velocity, true);
        if (action == "boost_up") return queue(DebugAction::add_up_velocity, true);
        if (action == "boost_offboard") return queue(DebugAction::add_offboard_up_velocity, true);
        return "there is no such control";
    }
    // The game's whole HUD, as `hideui` hides it, while any plugin asks; as it was when none does.
    void hide_hud(std::string_view plugin, bool hidden) override {
        const bool before = !hud_hiders_.empty();
        if (hidden) hud_hiders_.insert(std::string(plugin));
        else hud_hiders_.erase(std::string(plugin));
        const bool now = !hud_hiders_.empty();
        if (now == before) return;
        if (now) hud_was_hidden_ = base_ && multiplayer::sample_game_ui_state(base_).ui_hidden;
        if (!hud_was_hidden_) queue(overlay::DebugAction::set_game_ui_hidden, now);
    }

    // ---- the world -------------------------------------------------------------------------------
    std::vector<CatalogItem> object_catalog() override {
        std::vector<CatalogItem> result;
        const auto model = local_park_editor();
        if (model.assets)
            for (const auto& asset : *model.assets) result.push_back({asset.key, asset.title, asset.category});
        return result;
    }
    std::vector<GameObject> objects() override {
        std::vector<GameObject> result;
        for (const auto& o : local_park_editor().objects) result.push_back({o.id, o.item, o.position, o.rotation, o.scale});
        return result;
    }
    // Through the park editor's own edits (park_editor_runtime.h), its limits and the session's
    // object rules included. The editor need not be open.
    std::string edit_object(std::string_view operation, const GameObject& object) override {
        const auto model = local_park_editor();
        if (!model.available) return "objects cannot be placed here now";
        if (model.busy) return "the last edit is still being applied";
        profile::PlacedObject placed;
        placed.id = object.id;
        placed.item = object.item;
        placed.position = object.position;
        placed.rotation = object.rotation;
        placed.scale = object.scale;
        const auto status = edit_local_park(operation, model.map, model.generation, model.revision, {}, placed);
        return status.starts_with("error: ") ? status.substr(7) : std::string{};
    }
    std::string entity_set(std::string_view plugin, std::uint32_t id, std::string_view asset, const std::array<float, 16>& matrix, bool hidden,
                           std::optional<EntityAttach> attach, bool solid) override {
        register_world_physics();
        if (!attach) return world_entities::set(plugin, id, std::string(asset), matrix, hidden, std::nullopt, solid);
        world_entities::set_joint_source(&joint_matrix);
        return world_entities::set(plugin, id, std::string(asset), matrix, hidden,
                                   world_entities::Attach{attach->player, attach->joint, attach->upright, attach->turn, attach->ground}, solid);
    }
    // Where a skater's joint is (world_entities' attach): the local skater's as its pose is evaluated,
    // another player's as the session draws it.
    static std::optional<world_entities::Matrix> joint_matrix(std::uint64_t player, std::uint16_t joint) noexcept {
        try {
            std::array<float, 3> position{};
            std::array<float, 4> q{0, 0, 0, 1};
            float scale = 1;
            if (!player) {
                const auto at = skater_animation::joint(joint);
                if (!at) return std::nullopt;
                position = at->position, q = at->rotation, scale = at->scale[0];
            } else {
                const auto at = plugin_feed::joint_place(player, joint);
                if (!at) return std::nullopt;
                position = at->position, q = at->rotation, scale = at->scale;
            }
            // The rotation's axes as rows (right, up, forward), scaled, then the position.
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            return world_entities::Matrix{(1 - 2 * (y * y + z * z)) * scale, 2 * (x * y + w * z) * scale, 2 * (x * z - w * y) * scale, 0,
                                          2 * (x * y - w * z) * scale, (1 - 2 * (x * x + z * z)) * scale, 2 * (y * z + w * x) * scale, 0,
                                          2 * (x * z + w * y) * scale, 2 * (y * z - w * x) * scale, (1 - 2 * (x * x + y * y)) * scale, 0,
                                          position[0], position[1], position[2], 1};
        } catch (...) {
            return std::nullopt;
        }
    }
    void entity_remove(std::string_view plugin, std::uint32_t id) override { world_entities::remove(plugin, id); }
    void entity_clear(std::string_view plugin) override { world_entities::clear(plugin); }
    int entity_status(std::string_view plugin, std::uint32_t id) override { return static_cast<int>(world_entities::status(plugin, id)); }
    bool entities_ready(std::string_view plugin) override { return world_entities::ready(plugin); }
    std::vector<std::string> entity_blueprints(std::string_view prefix, bool meshes_only) override {
        return world_entities::blueprints(base_, prefix, 4096, meshes_only);
    }
    std::optional<std::array<Vec3, 2>> entity_bounds(std::string_view blueprint) override {
        const auto box = world_entities::blueprint_bounds(base_, blueprint);
        if (!box) return std::nullopt;
        return std::array<Vec3, 2>{(*box)[0], (*box)[1]};
    }
    std::optional<WorldPick> world_pick(const Vec3& from, const Vec3& to) override {
        const auto pick = local_world_pick(from, to);
        if (!pick) return std::nullopt;
        WorldPick out{pick->at, pick->entity != 0 || !pick->blueprint.empty(), std::nullopt, pick->blueprint};
        if (pick->pose) {
            // The pose: scale xyz, -, rotation xyzw, position xyz. Rows: the rotated x, y and z axes
            // times their scale (the same rotation objects.list's heading reads), then position.
            const auto& p = *pick->pose;
            const float x = p[4], y = p[5], z = p[6], w = p[7];
            const std::array<std::array<float, 3>, 3> axes{{{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
                                                           {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
                                                           {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}}};
            std::array<float, 16> m{};
            for (int row = 0; row < 3; ++row)
                for (int k = 0; k < 3; ++k) m[row * 4 + k] = axes[row][k] * p[row];
            m[12] = p[8], m[13] = p[9], m[14] = p[10], m[15] = 1;
            out.matrix = m;
        }
        return out;
    }
    std::string set_environment(std::string_view kind, std::string_view key, std::string_view value) override {
        if (kind == "tod") return time_of_day(key);
        if (kind == "control") {
            float number{};
            try {
                number = std::stof(std::string(value));
            } catch (...) {
                return "not a number";
            }
            return set_local_world_control(key, number) ? std::string{} : "the game refused it (no map loaded, or not that control)";
        }
        if (kind == "park") {
            const auto lot = std::find_if(park_lots.begin(), park_lots.end(), [&](const auto& l) { return l.key == key; });
            if (lot == park_lots.end() || !valid_park(static_cast<unsigned>(lot - park_lots.begin()), value))
                return "that is not a lot and one of its layouts";
            if (set_local_park(key, value)) return {};
            return "the host chooses the parks, or that is not a lot and layout";
        }
        return "there is no such setting";
    }
    std::vector<Destination> destinations() override {
        std::vector<Destination> result;
        for (const auto& [id, d] : profile_runtime::location_travel_runtime().policy.destinations) result.push_back({id, d.name, d.map});
        return result;
    }
    std::vector<LevelInfo> levels() override {
        std::vector<LevelInfo> result;
        auto& r = runtime::detail::runtime();
        std::lock_guard lock(r.mutex);
        for (const auto& level : r.model.levels) result.push_back({level.asset, level.display_name, level.can_load});
        return result;
    }
    std::string travel(std::string_view kind, std::string_view target) override {
        if (multiplayer_controls_level(multiplayer::model())) return "only the session's host changes where everyone is";
        if (kind == "location") {
            const auto& destinations = profile_runtime::location_travel_runtime().policy.destinations;
            const auto found = destinations.find(target);
            if (found == destinations.end()) return "there is no destination " + std::string(target);
            return runtime::detail::queue_location_travel(found->second.map.c_str()) ? std::string{} : "it cannot travel now";
        }
        std::array<char, 512> reply{};
        const std::string asset(target);
        if (runtime::detail::queue_load(nullptr, asset.c_str(), "", "", "", reply.data(), reply.size())) return {};
        reply.back() = '\0';
        return reply[0] ? std::string(reply.data()) : std::string("that level cannot be loaded now");
    }
    std::string ai(std::string_view operation, int count) override {
        if (operation == "clear") {
            ai_skaters::request_clear();
            return {};
        }
        if (multiplayer_session_active()) return "AI skaters are left out of sessions: the other players would never see them";
        if (static_cast<int>(ai_skaters::active_count()) + count > 16) return "at most 16 AI skaters";
        ai_skaters::request_spawn(static_cast<unsigned>(count), true, true);
        return {};
    }
    int ai_count() override { return static_cast<int>(ai_skaters::active_count()); }

    // ---- the session ------------------------------------------------------------------------------
    std::vector<PeerSkater> peer_skaters() override {
        std::vector<PeerSkater> result;
        for (const auto& p : plugin_feed::latest_peers()) {
            const auto& q = p.rotation;
            // Where the skater faces: the rotation's +Z about world up, as the local skater's heading.
            const float heading = std::atan2(2 * (q[0] * q[2] + q[3] * q[1]), 1 - 2 * (q[0] * q[0] + q[1] * q[1]));
            result.push_back({p.id, p.position, heading, p.age_ms});
        }
        return result;
    }
    std::vector<Body> bodies() override {
        std::vector<Body> result;
        for (const auto& b : plugin_feed::latest_bodies()) {
            Body body{b.player, {}};
            for (const auto& c : b.capsules) body.capsules.push_back({c.part, c.a, c.b, c.radius});
            result.push_back(std::move(body));
        }
        return result;
    }
    std::string hide_player(std::string_view plugin, std::uint64_t player, bool hidden) override {
        return plugin_feed::hide(std::string(plugin), player, hidden) ? std::string{} : "at most 256 players are hidden";
    }
    std::vector<std::uint64_t> hidden_players(std::string_view plugin) override { return plugin_feed::hidden_by(std::string(plugin)); }
    void set_nametag(std::string_view plugin, std::uint64_t player, std::string text, std::uint32_t colour) override {
        plugin_feed::set_tag(std::string(plugin), player, std::move(text), colour);
    }
    void hide_nametags(std::string_view plugin, bool hidden) override {
        if (hidden) nametag_hiders_.emplace(plugin);
        else if (const auto at = nametag_hiders_.find(plugin); at != nametag_hiders_.end()) nametag_hiders_.erase(at);
        multiplayer::nametags_held_off.store(!nametag_hiders_.empty(), std::memory_order_relaxed);
    }
    std::string voice_volume(std::uint64_t player, float volume) override {
        const auto id = std::to_string(player);
        const auto mute = multiplayer::command("voice-mute", id + (volume <= 0 ? " on" : " off"));
        if (volume <= 0) return reply(mute);
        return reply(multiplayer::command("voice-volume", id + " " + std::format("{:.2f}", volume)));
    }
    std::vector<ChatLine> chat_since(std::uint64_t sequence) override {
        std::vector<ChatLine> result;
        const auto chat = multiplayer::chat();
        if (chat.latest <= sequence) return result;
        for (const auto& line : chat.lines)
            if (line.sequence > sequence && line.tag != "Plugin") result.push_back({line.sequence, line.sender, line.name, line.text, line.local, line.server});
        return result;
    }
    std::string send_chat(std::string_view text) override {
        if (!multiplayer_session_active()) return "not in a session";
        const auto answer = multiplayer::command("chat", text);
        return answer == "Message sent." ? std::string{} : answer;
    }
    std::string local_chat(std::string_view plugin, std::string_view text, std::uint32_t colour) override {
        return multiplayer::plugin_chat(plugin, text, colour);
    }
    ThrowdownState throwdowns() override {
        ThrowdownState state{multiplayer::local_throwdown_active(), multiplayer::local_throwdown_host(), {}};
        for (const auto& p : multiplayer::skate_hud().players) state.skate.push_back({p.name, p.letters, p.up, p.out});
        return state;
    }

    // ---- the rest -------------------------------------------------------------------------------------
    Track track() override {
        if (!music_started_) {
            music_started_ = true;
            external_player::start();
        }
        const auto now = external_player::now();
        return {now.available, now.playing, now.app, now.title, now.artist, now.album, now.revision};
    }
    std::string music(std::string_view command) override {
        using external_player::Command;
        if (!music_started_) {
            music_started_ = true;
            external_player::start();
        }
        if (command == "play") external_player::send(Command::play);
        else if (command == "pause") external_player::send(Command::pause);
        else if (command == "next") external_player::send(Command::next);
        else if (command == "previous") external_player::send(Command::previous);
        else return "there is no such command";
        return {};
    }
    std::optional<Track> game_track() override {
        const auto song = spotify_station::game_song();
        if (!song) return std::nullopt;
        return Track{true, true, "skate.", song->title, song->artist, {}, 0};
    }
    std::string game_music(std::string_view command) override {
        if (command != "next") return "there is no such command";
        return spotify_station::game_next() ? std::string{} : "the game's music cannot skip now (a Spotify station is on, or no song has started)";
    }
    Progress progress() override {
        const auto model = local_profile_progression();
        Progress p;
        p.available = model.available;
        for (const auto& d : model.districts) p.districts.push_back({d.id, d.name, static_cast<int>(d.rank)});
        for (const auto& c : model.challenges)
            p.challenges.push_back({c.id, c.type, c.neighborhood, static_cast<int>(c.attempts), static_cast<int>(c.completed_goals.size()),
                                    static_cast<int>(c.goal_count)});
        p.score = static_cast<double>(model.score);
        p.score_cap = static_cast<double>(model.score_cap);
        p.score_level = model.score_level;
        return p;
    }
    std::optional<std::string> setting(std::string_view name) override { return named_setting_value(name); }
    std::string set_setting(std::string_view plugin, std::string_view name, std::optional<std::string> value) override {
        return held_settings::set(plugin, name, std::move(value));
    }
    std::vector<std::string> setting_names() override { return named_setting_names(); }

    // ---- input -------------------------------------------------------------------------------------------
    std::string bind(std::string_view plugin, std::string_view action, std::uint32_t combo) override {
        if (combo && !valid_action_binding(combo)) return "that is not a key or pad combo"; // (0: none of its own)
        binds_[{std::string(plugin), std::string(action)}].own = combo;
        publish_binds();
        return {};
    }
    // Each bind's press, once, while the game itself has the input (no menu, console or text box).
    // The player's own combo from the Binds page wins over the plugin's.
    std::vector<std::pair<std::string, std::string>> take_presses() override {
        apply_rebinds();
        std::vector<std::pair<std::string, std::string>> presses;
        if (binds_.empty()) return presses;
        ControllerInput input;
        DingoSDKOverlayReadControllerInput(&input, false);
        for (auto& [key, bind] : binds_)
            if (bind.latch.update(combo_in_use(key, bind), input, false)) presses.push_back(key);
        return presses;
    }
    std::string host_throwdown(std::string_view plugin, std::string_view series, std::vector<std::uint64_t> players, int most) override {
        if (!multiplayer_session_active()) return "not in a session";
        if (!thrower_.empty() && thrower_ != plugin) {
            const auto status = multiplayer::throwdown_relay_host_status();
            if (status == "asked" || status == "spawning" || status == "open") return thrower_ + " is hosting a drop";
        }
        auto why = multiplayer::throwdown_relay_host(std::string(series), std::move(players), static_cast<std::uint32_t>(std::max(most, 0)));
        if (why.empty()) thrower_ = plugin;
        return why;
    }
    void cancel_throwdown(std::string_view plugin) override {
        if (thrower_ != plugin) return;
        multiplayer::throwdown_relay_cancel_host(); // its status says "cancelled" from the relay's next tick
    }
    std::string throwdown_status(std::string_view plugin) override {
        return thrower_ == plugin ? multiplayer::throwdown_relay_host_status() : std::string{};
    }
    // ---- board -----------------------------------------------------------------------------------------
    std::optional<std::array<BoardBone, 16>> board_bones() override {
        const auto drawn = board_pose::read();
        if (!drawn) return std::nullopt;
        std::array<BoardBone, 16> out{};
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = {(*drawn)[i].position, (*drawn)[i].rotation, (*drawn)[i].scale};
        return out;
    }
    std::string board_set(std::string_view plugin, int bone, const BoardBone& value, bool world) override {
        return board_pose::set(plugin, bone, {value.position, value.rotation, value.scale}, world ? board_pose::Space::world : board_pose::Space::local);
    }
    std::string board_hide(std::string_view plugin, int bone, bool everyone) override { return board_pose::hide(plugin, bone, everyone); }
    void board_clear(std::string_view plugin, int bone) override { board_pose::clear(plugin, bone); }
    void board_park(std::string_view plugin, std::vector<int> bones) override { board_pose::park(plugin, std::move(bones)); }
    std::string board_toss(std::string_view plugin, std::vector<int> bones, const Vec3& velocity, const Vec3& spin, float ground) override {
        return board_pose::toss(plugin, std::move(bones), velocity, spin, ground);
    }
    std::string board_fit(std::string_view plugin, std::optional<BoardFit> fit) override {
        if (!fit) return board_pose::fit(plugin, std::nullopt);
        return board_pose::fit(plugin, board_pose::Fit{fit->plates, fit->axle});
    }
    std::optional<std::vector<BoardParameter>> board_parameters(int part) override {
        const auto found = board_material::read(static_cast<board_material::Part>(part));
        if (!found) return std::nullopt;
        std::vector<BoardParameter> out;
        for (const auto& p : *found) {
            using board_material::Kind;
            const char* type = p.kind == Kind::colour ? "colour" : p.kind == Kind::number ? "number" : p.kind == Kind::integer ? "integer"
                             : p.kind == Kind::toggle ? "switch" : "other";
            out.push_back({p.key, type, p.value, p.integer});
        }
        return out;
    }
    std::string board_set_parameter(std::string_view plugin, int part, std::uint64_t key, std::array<float, 3> value, bool colour) override {
        return board_material::set(plugin, static_cast<board_material::Part>(part), key, value, colour);
    }
    void board_clear_parameter(std::string_view plugin, int part, std::optional<std::uint64_t> key) override {
        board_material::clear(plugin, part < 0 ? std::nullopt : std::optional(static_cast<board_material::Part>(part)), key);
    }
    // ---- variants ------------------------------------------------------------------------------------
    // The mod's folder is read again at most every 2 s: a menu page asks every frame.
    std::optional<VariantState> variants(std::string_view plugin, const std::filesystem::path& package) override {
        const auto now = GetTickCount64();
        auto& kept = variant_reads_[std::string(plugin)];
        if (!kept.at || now - kept.at > 2000) {
            std::vector<std::string> notes;
            const auto mod = mods::describe_mod(package, std::string(plugin), notes);
            kept.declared = mod.variants.has_value();
            kept.installed = kept.declared ? mods::installed_variants(mod) : std::vector<std::string>{};
            kept.at = now;
        }
        if (!kept.declared) return std::nullopt;
        return VariantState{kept.installed, mods::chosen_variant(plugin), mods::variant_in_play(plugin)};
    }
    std::string choose_variant(std::string_view plugin, const std::filesystem::path& package, std::string_view variant) override {
        const auto state = variants(plugin, package);
        if (!state) return "its mod declares no variants";
        std::string name(variant);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::find(state->installed.begin(), state->installed.end(), name) == state->installed.end()) return "its mod has no variant " + name;
        if (live_mods::busy()) return "the mods are being applied";
        if (!mods::save_variant(plugin, name)) return "the choice could not be saved";
        if (name != state->in_play) (void)live_mods::apply(true);
        return {};
    }
    // ---- outfit --------------------------------------------------------------------------------------
    std::string outfit_set(std::string_view plugin, std::string_view slot, std::string_view item, std::vector<float> placement, bool covers) override {
        return outfit_overlay::set(std::string(plugin), std::string(slot), std::string(item), std::move(placement), covers);
    }
    void outfit_clear(std::string_view plugin, std::string_view slot) override { outfit_overlay::clear(plugin, slot); }
    std::string outfit_find(std::string_view ending) override { return outfit_overlay::find_item(ending); }
    std::vector<OutfitSlot> outfit_slots() override {
        std::vector<OutfitSlot> out;
        for (auto& s : outfit_overlay::slots()) out.push_back({std::move(s.name), std::move(s.item), std::move(s.parameters)});
        return out;
    }
    std::string outfit_status() override { return outfit_overlay::status(); }
    std::string outfit_on_top(std::string_view plugin, std::vector<std::string> items) override {
        return outfit_overlay::set_layers(std::string(plugin), std::move(items));
    }
    OnTop outfit_on_top_state(std::string_view plugin) override {
        auto layers = outfit_overlay::layers(plugin);
        return {std::move(layers.items), std::move(layers.worn), std::move(layers.note)};
    }
    // ---- the skater's physics ------------------------------------------------------------------------
    SkaterNews take_skater_news() override {
        skater_events::want();
        SkaterNews news;
        for (const auto& l : skater_events::take_landings()) news.landings.push_back({l.down_speed, l.speed, l.rail});
        for (const auto& h : skater_events::take_hits())
            news.hits.push_back({skater_body::names[std::min<std::size_t>(h.bone, skater_body::count - 1)], h.impact, h.vehicle, h.board});
        for (const auto& f : skater_events::take_falls()) {
            SkaterNews::Fall fall{f.seconds, f.left, f.right, {}};
            for (std::size_t i = 1; i < skater_body::count; ++i)
                if (f.impact[i] > 0 || f.slide[i] > 0) fall.bones.push_back({skater_body::names[i], f.impact[i], f.slide[i]});
            news.falls.push_back(std::move(fall));
        }
        return news;
    }
    std::vector<PhysicsStep> take_physics_steps() override {
        skater_events::want_steps();
        std::vector<PhysicsStep> out;
        for (const auto& news : skater_events::take_steps()) {
            const auto& s = news.step;
            PhysicsStep step{news.at, s.seconds, s.wipeout, s.airborne, s.ragdoll, std::nullopt, s.spin, {}};
            if (s.velocity) step.velocity = Vec3{(*s.velocity)[0], (*s.velocity)[1], (*s.velocity)[2]};
            for (std::size_t i = 1; i < skater_body::count; ++i) {
                const auto& c = s.body.bodies[i];
                if (!c.touching) continue;
                const float slide = std::sqrt(c.slide[0] * c.slide[0] + c.slide[1] * c.slide[1] + c.slide[2] * c.slide[2]);
                step.bodies.push_back({skater_body::names[i], c.impact, std::isfinite(slide) ? slide : 0.0f, c.hit.board, c.hit.vehicle,
                                      c.hit.world || c.hit.kind_5 || c.hit.kind_11});
            }
            out.push_back(std::move(step));
        }
        return out;
    }
    std::string xray(std::string_view plugin, bool on) override {
        if (!xray::available()) return "the x-ray is unavailable for this game build";
        xray::prepare(plugin, on);
        return {};
    }
    std::string hide_skater(std::string_view plugin, bool hidden) override {
        if (!xray::hide(plugin, hidden)) return "hiding the skater is unavailable for this game build";
        return {};
    }
    std::string hud_corner(std::string_view plugin, bool hidden) override {
        hud_corner::ask(plugin, hidden);
        return {};
    }
    std::string game_speed(std::string_view plugin, float speed) override {
        if (speed < 1.0f && multiplayer_session_active()) return "never in a multiplayer session";
        game_speed::ask(plugin, speed);
        return {};
    }
    float game_speed_now() override { return game_speed::current(); }
    std::string shape_launch(std::string_view plugin, std::string_view from, std::optional<LaunchShape> shape) override {
        const auto kind = launches::from_name(from);
        if (!kind) return "a launch is out of a grind, ollie, no_comply or boneless";
        std::optional<launches::Shape> s;
        if (shape) s = launches::Shape{shape->across, shape->along, shape->up};
        return launches::shape(plugin, *kind, s);
    }
    std::string set_throwdown_mode(std::string_view plugin, const ThrowdownMode& m) override {
        multiplayer::custom_throwdowns::Mode mode;
        mode.plugin = plugin;
        mode.id = m.id;
        mode.name = m.name;
        mode.description = m.description;
        mode.art = m.art;
        mode.flag = m.flag;
        mode.replaces = m.replaces;
        mode.card = m.card;
        mode.order = m.order;
        mode.color = m.color;
        mode.min_players = m.min_players;
        mode.max_players = m.max_players;
        mode.players = m.players;
        mode.cursor_height = m.cursor_height;
        return multiplayer::custom_throwdowns::set_mode(std::move(mode));
    }
    // ---- the throwdown kit -------------------------------------------------------------------------
    std::string publish_throwdown_ui(std::string_view plugin, const Json& document) override { return throwdowns::kit::publish(plugin, document); }
    bool throwdown_flag_begin(std::string_view mode, std::uint32_t game, const throwdowns::kit::flag::Options& options) override {
        return throwdowns::kit::flag::begin(mode, game, options);
    }
    throwdowns::kit::flag::State throwdown_flag_state() override { return throwdowns::kit::flag::state(); }
    // Only the plugin whose mode the registration is may let it go.
    void throwdown_flag_leave(std::string_view plugin) override {
        if (throwdowns::kit::flag::state().mode.starts_with(std::string(plugin) + "/")) throwdowns::kit::flag::leave();
    }
    // ---- files: <game>/Mods/.reskate_plugin_data/<plugin>/ (mod_list.h plugin_data_folder) -------------
    static constexpr std::size_t most_file_bytes = 1u << 20, most_files = 1000;
    // Before 2026-10-20 the folder was Mods/.reskate/data/<plugin>/, which a full merge deletes. The
    // merge moves what is still there first (mods::keep_plugin_data); a merge that kept its patch did
    // not run it, so the first look for a plugin's folder does.
    static void move_old_data() {
        static std::once_flag once;
        std::call_once(once, [] {
            for (const auto& plugin : mods::keep_plugin_data(game_folder() / mods::mods_folder))
                logging::write(logging::Level::info, logging::Channel::plugins,
                    "Plugins: " + plugin + "'s data folder moved to Mods/" + mods::plugin_data_folder);
        });
    }
    // The plugin's folder and the path in it, or why not. A path that resolves outside the folder
    // (through a junction someone made in it) is refused.
    static std::string data_file(std::string_view plugin, std::string_view path, std::filesystem::path& root, std::filesystem::path& file) {
        try {
            root = game_folder() / mods::mods_folder / mods::plugin_data_folder / std::filesystem::path(std::string(plugin));
            move_old_data();
            file = path.empty() ? root : root / std::filesystem::path(std::string(path));
            std::error_code error;
            const auto real_root = std::filesystem::weakly_canonical(root, error);
            const auto real_file = error ? std::filesystem::path{} : std::filesystem::weakly_canonical(file, error);
            if (error) return "the data folder cannot be read";
            const auto relative = real_file.lexically_relative(real_root);
            if (relative.empty() || *relative.begin() == "..") return "outside the data folder";
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    }
    std::string files_list(std::string_view plugin, std::string_view folder, std::vector<std::string>& names) override {
        std::filesystem::path root, at;
        if (auto why = data_file(plugin, folder, root, at); !why.empty()) return why;
        std::error_code error;
        if (!std::filesystem::exists(at, error)) return folder.empty() ? std::string{} : std::string("not found");
        if (!std::filesystem::is_directory(at, error)) return "not a folder";
        for (std::filesystem::directory_iterator it(at, error), end; !error && it != end && names.size() < most_files; it.increment(error)) {
            auto name = it->path().filename().string();
            if (name.ends_with(".tmp")) continue; // a write going on
            if (it->is_directory(error)) name += "/";
            names.push_back(std::move(name));
        }
        if (error) return "the folder cannot be read";
        std::sort(names.begin(), names.end());
        return {};
    }
    std::string files_read(std::string_view plugin, std::string_view path, std::string& text) override {
        return files_read_bytes(plugin, path, text, most_file_bytes);
    }
    std::string files_read_bytes(std::string_view plugin, std::string_view path, std::string& text, std::size_t most) override {
        std::filesystem::path root, file;
        if (auto why = data_file(plugin, path, root, file); !why.empty()) return why;
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) return "not found";
        const auto size = std::filesystem::file_size(file, error);
        if (error) return "the file cannot be read";
        if (size > most) return std::format("the file is larger than {} MB", most >> 20);
        std::ifstream in(file, std::ios::binary);
        text.resize(static_cast<std::size_t>(size));
        if (!in || !in.read(text.data(), static_cast<std::streamsize>(size))) return "the file cannot be read";
        return {};
    }
    std::string files_write(std::string_view plugin, std::string_view path, std::string_view text) override {
        return files_write_bytes(plugin, path, text, most_file_bytes);
    }
    std::string files_write_bytes(std::string_view plugin, std::string_view path, std::string_view text, std::size_t most) override {
        if (text.size() > most) return std::format("a file is at most {} MB", most >> 20);
        std::filesystem::path root, file;
        if (auto why = data_file(plugin, path, root, file); !why.empty()) return why;
        std::error_code error;
        if (std::filesystem::is_directory(file, error)) return "a folder is there";
        if (!std::filesystem::exists(file, error)) {
            std::size_t count = 0;
            if (std::filesystem::exists(root, error))
                for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
                    if (it->is_regular_file(error) && ++count >= most_files) return "the data folder holds 1000 files already";
        }
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) return "the folder cannot be made";
        auto temporary = file;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) return "the file cannot be written";
        }
        std::filesystem::rename(temporary, file, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return "the file cannot be written";
        }
        return {};
    }
    std::string files_remove(std::string_view plugin, std::string_view path) override {
        std::filesystem::path root, file;
        if (auto why = data_file(plugin, path, root, file); !why.empty()) return why;
        std::error_code error;
        if (!std::filesystem::exists(file, error)) return "not found";
        if (std::filesystem::is_directory(file, error) && !std::filesystem::is_empty(file, error)) return "the folder is not empty";
        if (!std::filesystem::remove(file, error) || error) return "it cannot be removed";
        return {};
    }
    // ---- ui.dialog ----------------------------------------------------------------------------------
    std::string dialog_open(std::string_view plugin, const DialogSpec& spec) override { return overlay::open_plugin_dialog(plugin, spec); }
    void dialog_close(std::string_view plugin, std::string_view id) override { overlay::close_plugin_dialog(plugin, id); }
    // ---- skater.push / hold --------------------------------------------------------------------------
    // A velocity the next on-board physics step writes (Extension/Skater/skater_push.h), and a hold
    // that keeps the board still. Both are written on the game's stock physics only (client_noclip.cpp):
    // without it they would be dropped unseen, so they say so.
    static constexpr char needs_stock_physics[] = "needs the game's stock physics: reskate.physics.competition(true) first";
    std::string skater_push(std::string_view, const Vec3& velocity) override {
        if (!::dingosdk::skater_push::valid(velocity)) return "a push is horizontal (y 0) and at most 120 m/s";
        if (!competition_stock_physics()) return std::string("a push ") + needs_stock_physics;
        ::dingosdk::skater_push::request(velocity);
        return {};
    }
    std::string skater_push_wait(std::string_view) override {
        ::dingosdk::skater_push::wait();
        return {};
    }
    std::string skater_push_reset(std::string_view) override {
        ::dingosdk::skater_push::reset();
        return {};
    }
    std::string skater_hold(std::string_view plugin, bool on) override {
        if (on && !competition_stock_physics()) return std::string("a hold ") + needs_stock_physics;
        if (on) hold_owner_ = plugin;
        else if (hold_owner_ != plugin) return {};
        else hold_owner_.clear();
        ::dingosdk::skater_push::hold(on);
        return {};
    }
    void remove_throwdown_mode(std::string_view plugin, std::string_view id) override {
        multiplayer::custom_throwdowns::remove_mode(plugin, id);
    }
    std::vector<LaunchNews> take_launches() override {
        std::vector<LaunchNews> out;
        for (auto& l : launches::take()) {
            LaunchNews news{launches::name(l.from), l.game, l.velocity, std::nullopt, l.travel_speed, std::move(l.reason), std::move(l.by)};
            if (l.travel[0] != 0 || l.travel[1] != 0) news.travel = l.travel;
            out.push_back(std::move(news));
        }
        return out;
    }
    // A speed boost: off where the session's host turned boosts off, and while a throwdown runs.
    std::string cruise(std::string_view, float speed, bool may_slow) override {
        if (speed > 0 && multiplayer_session_active() && !session_boosts_allowed()) return "the host has turned boosts off";
        if (speed > 0 && multiplayer::local_throwdown_active()) return "not while a throwdown runs";
        set_plugin_cruise(speed, may_slow);
        return {};
    }
    std::string set_velocity(std::string_view, const Vec3& velocity, bool add) override {
        if (multiplayer_session_active() && !session_boosts_allowed()) return "the host has turned boosts off";
        if (multiplayer::local_throwdown_active() && !competition_stock_physics()) return "not while a throwdown runs";
        set_plugin_velocity(velocity, add);
        return {};
    }
    std::string hide_pad(std::string_view plugin, const PadHidden& h) override {
        if (!pad_hide::available()) return "hiding pad controls is unavailable for this game build";
        pad_hide::hide(plugin, {h.left_trigger, h.right_trigger, h.left_x_grinding, h.right_stick, h.left_stick_click, h.right_stick_click});
        return {};
    }
    std::optional<Vec3> board_spin() override { return skater_events::board_spin(); }
    std::optional<Vec3> board_velocity() override {
        skater_events::want();
        return skater_events::board_velocity();
    }
    std::optional<SlamNews> last_slam() override {
        const auto slam = skater_events::last_slam();
        if (!slam.at) return std::nullopt;
        return SlamNews{slam.force, slam.cause, static_cast<double>(GetTickCount64() - slam.at)};
    }
    std::string wipeout(float force) override {
        if (multiplayer::local_throwdown_active()) return "not while a throwdown runs";
        no_bail_request_wipeout(std::clamp(force, 0.5f, 3.0f));
        return {};
    }
    // ---- audio ---------------------------------------------------------------------------------------
    std::uint64_t play_sound(std::string_view plugin, SoundStart sound) override {
        plugin_sound::Voice voice;
        voice.owner = plugin;
        voice.wave = std::move(sound.wave);
        voice.volume = sound.volume;
        voice.pitch = sound.pitch;
        voice.range = sound.range;
        voice.throttle = sound.throttle;
        voice.speed = sound.speed;
        voice.loop = sound.loop;
        voice.position = sound.position;
        return plugin_sound::start(std::move(voice));
    }
    bool change_sound(std::string_view plugin, std::uint64_t id, const SoundChange& change) override {
        auto* voice = plugin_sound::find(id, plugin);
        if (!voice) return false;
        if (change.volume) voice->volume = *change.volume;
        if (change.pitch) voice->pitch = *change.pitch;
        if (change.range) voice->range = *change.range;
        if (change.throttle) voice->throttle = *change.throttle;
        if (change.speed) voice->speed = *change.speed;
        if (change.position) voice->position = *change.position;
        return true;
    }
    void stop_sound(std::string_view plugin, std::uint64_t id) override { plugin_sound::stop(id, plugin); }
    bool sound_playing(std::string_view plugin, std::uint64_t id) override { return plugin_sound::find(id, plugin) != nullptr; }
    // ---- input -------------------------------------------------------------------------------------
    bool camera_mine(std::string_view plugin) const { return !camera_holder_.empty() && camera_holder_ == plugin && camera_shown_; }
    std::optional<FlightInput> flight_input(std::string_view plugin) override {
        if (!camera_mine(plugin)) return std::nullopt;
        // Read once a tick: the mouse's movement is used up as it is read.
        const auto now = GetTickCount64();
        if (now - flight_read_at_ > 4) {
            flight_read_at_ = now;
            overlay::FlightInput in;
            DingoSDKOverlayReadFlightInput(&in, true, false);
            flight_ = in.active ? std::optional<FlightInput>(FlightInput{in.right, in.up, in.forward, in.look_x, in.look_y, in.boost})
                                : std::nullopt;
        }
        return flight_;
    }
    std::optional<PadInput> gamepad() override {
        if (!joystick::game_in_foreground()) return std::nullopt;
        overlay::Gamepad pad;
        {
            joystick::InputAccess access; // seen even while captured from the game
            pad = overlay::read_gamepad();
        }
        if (!pad.valid) return std::nullopt;
        return PadInput{pad.left_x, pad.left_y, pad.right_x, pad.right_y, pad.left_trigger, pad.right_trigger, pad.buttons, pad.playstation};
    }
    std::optional<StickInput> joystick() override {
        if (!joystick::game_in_foreground()) return std::nullopt;
        const auto sample = joystick::poll();
        if (!sample.valid) return std::nullopt;
        overlay::note_joystick(sample.buttons, sample.axes);
        return StickInput{sample.axes, sample.buttons, sample.name};
    }
    std::string capture_input(std::string_view plugin, bool on) override {
        if (!on) {
            if (capturer_ == plugin) capturer_.clear();
            return {};
        }
        if (!camera_mine(plugin)) return "only while the plugin's camera is shown";
        capturer_ = plugin;
        capture_until_ = GetTickCount64() + 250;
        return {};
    }
    bool capturing() const noexcept {
        return !capturer_.empty() && camera_mine(capturer_) && GetTickCount64() < capture_until_;
    }
    // ---- skater.animation ---------------------------------------------------------------------------
    // The game's own clips, read in the background from the first ask.
    static std::shared_ptr<const anim_studio::game_clips::Library> game_clips_library(std::string& why) {
        auto library = anim_studio::game_clips::library();
        if (!library) {
            const auto failed = anim_studio::game_clips::failure();
            why = failed.empty() ? "the game's animations are still being read" : "the game's animations cannot be read: " + failed;
        }
        return library;
    }
    std::vector<std::string> game_clip_names(std::string_view containing, std::size_t most, std::string& why) override {
        const auto library = game_clips_library(why);
        return library ? library->names(containing, most) : std::vector<std::string>{};
    }
    std::optional<bool> clip_goofy(const ClipRef& clip) override {
        std::string why;
        const auto library = game_clips_library(why);
        return library && clip ? library->goofy(*clip) : std::nullopt;
    }
    ClipRef mirror_clip(const ClipRef& clip, std::string& why) override {
        const auto library = game_clips_library(why);
        if (!library || !clip) return nullptr;
        try {
            return std::make_shared<const anim_studio::Clip>(library->mirror(*clip));
        } catch (const std::exception& e) {
            why = e.what();
            return nullptr;
        }
    }
    ClipRef game_clip(std::string_view name, std::string& why) override {
        const auto library = game_clips_library(why);
        if (!library) return nullptr;
        try {
            return std::make_shared<const anim_studio::Clip>(library->load(name));
        } catch (const std::exception& e) {
            why = e.what();
            return nullptr;
        }
    }
    std::string play_clip(std::string_view plugin, ClipRef clip, float speed, float blend, bool loop, float at, bool paused, bool floor) override {
        if (multiplayer::local_throwdown_active()) return "not while a throwdown runs";
        if (!player_.empty() && player_ != plugin && skater_animation::playing()) return player_ + " is playing a clip";
        player_ = plugin;
        skater_animation::play(skater_animation::Play{std::move(clip), speed, blend, loop, at, paused, floor});
        return {};
    }
    void seek_clip(std::string_view plugin, float seconds) override {
        if (player_ == plugin) skater_animation::seek(seconds);
    }
    void pause_clip(std::string_view plugin, bool paused) override {
        if (player_ == plugin) skater_animation::pause(paused);
    }
    std::optional<float> clip_time(std::string_view plugin) override {
        if (player_ != plugin || !skater_animation::playing()) return std::nullopt;
        return skater_animation::play_time();
    }
    void stop_clip(std::string_view plugin) override {
        if (player_ != plugin) return;
        skater_animation::play(std::nullopt);
        player_.clear();
    }
    bool clip_playing(std::string_view plugin) override { return player_ == plugin && skater_animation::playing(); }
    std::string screen_look(std::string_view plugin, std::optional<ScreenLook> look) override {
        const auto now = GetTickCount64();
        if (!looker_.empty() && looker_ != plugin && now < look_at_ + 300) return look ? looker_ + " has the screen's look" : std::string{};
        if (!look) {
            if (looker_ == plugin) {
                screen_look::clear();
                looker_.clear();
            }
            return {};
        }
        looker_ = plugin;
        look_at_ = now;
        screen_look::Settings s;
        s.bend = look->bend, s.fov = look->fov, s.age = look->age;
        s.bars = look->bars, s.hood = look->hood, s.grade = look->grade, s.soft = look->soft, s.scanlines = look->scanlines, s.grain = look->grain;
        s.dropouts = look->dropouts, s.head_switching = look->head_switching, s.tracking = look->tracking, s.smear = look->smear, s.iris = look->iris;
        s.rolling = look->rolling;
        s.text = look->text;
        screen_look::set(s);
        return {};
    }
    std::string screen_slap(std::string_view plugin) override {
        if (looker_ != plugin || GetTickCount64() >= look_at_ + 300) return "only over this plugin's screen look";
        screen_look::slap();
        return {};
    }
    static skater_animation::Shown shown_of(Shown shown) {
        return shown == Shown::others ? skater_animation::Shown::others : shown == Shown::me ? skater_animation::Shown::me : skater_animation::Shown::everyone;
    }
    std::string skater_pose(std::string_view plugin, const std::vector<JointOffset>& offsets, float weight, Shown shown) override {
        const auto now = GetTickCount64();
        if (!poser_.empty() && poser_ != plugin && now < pose_at_ + 300) return offsets.empty() ? std::string{} : poser_ + " poses the skater";
        if (offsets.empty()) {
            if (poser_ == plugin) {
                skater_animation::pose({}, 1);
                poser_.clear();
            }
            return {};
        }
        poser_ = plugin;
        pose_at_ = now;
        std::vector<skater_animation::PoseOffset> out;
        for (const auto& o : offsets) out.push_back({o.joint, o.rotation, o.scale, o.offset});
        skater_animation::pose(std::move(out), weight, shown_of(shown));
        return {};
    }
    std::string camera_cycle(std::string_view plugin, CycleCamera state) override {
        const auto now = GetTickCount64();
        auto& link = first_person::plugin_link;
        if (!cycler_.empty() && cycler_ != plugin && now < cycle_at_ + 300)
            return state == CycleCamera::out ? std::string{} : cycler_ + " is in the camera cycle";
        if (state == CycleCamera::out) {
            if (cycler_ == plugin) {
                link.flags.store(0, std::memory_order_release);
                cycler_.clear();
            }
            return {};
        }
        if (cycler_ != plugin) link.request.store(0, std::memory_order_release); // (a step asked of another plugin)
        cycler_ = plugin;
        cycle_at_ = now;
        link.flags.store(static_cast<std::uint8_t>(first_person::plugin_flag_enabled |
                                                   (state == CycleCamera::active ? first_person::plugin_flag_on : first_person::plugin_flag_can_enter)),
                         std::memory_order_release);
        link.published.store(now, std::memory_order_release);
        return {};
    }
    std::vector<CycleNews> take_camera_cycle() override {
        const auto step = static_cast<first_person::CycleStep>(first_person::plugin_link.request.exchange(0, std::memory_order_acq_rel));
        if (step == first_person::CycleStep::none || cycler_.empty() || GetTickCount64() >= cycle_at_ + 300) return {};
        return {{cycler_, step == first_person::CycleStep::enter}};
    }
    std::string skater_carry(std::string_view plugin, std::optional<Carry> goal) override {
        const auto now = GetTickCount64();
        if (!carrier_.empty() && carrier_ != plugin && now < carry_at_ + 300) {
            if (!goal) return {};
            return carrier_ + " has the skater carrying";
        }
        if (!goal) {
            if (carrier_ == plugin) {
                skater_animation::carry(std::nullopt);
                carrier_.clear();
            }
            return {};
        }
        carrier_ = plugin;
        carry_at_ = now;
        carry::Goal g;
        g.hand = goal->left ? arm_reach::Hand::left : arm_reach::Hand::right;
        g.both = goal->both;
        g.front = goal->front;
        g.aim = goal->aim;
        g.front_from_grip = goal->front_from_grip;
        if (goal->worn) g.worn = carry::Grip{goal->worn->first, goal->worn->second};
        g.weight = goal->weight;
        g.crouch = goal->crouch;
        g.hunch = goal->hunch;
        g.still = goal->still;
        g.turn = goal->turn;
        g.step = goal->step;
        g.step_phase = goal->step_phase;
        skater_animation::carry(g, goal->push_straightens, shown_of(goal->shown));
        return {};
    }
    std::string skater_reach(std::string_view plugin, std::optional<Reach> goal) override {
        const auto now = GetTickCount64();
        if (!reacher_.empty() && reacher_ != plugin && now < reach_at_ + 300) {
            if (!goal) return {};
            return reacher_ + " holds the skater's arm";
        }
        if (!goal) {
            if (reacher_ == plugin) {
                skater_animation::reach(std::nullopt);
                reacher_.clear();
            }
            return {};
        }
        reacher_ = plugin;
        reach_at_ = now;
        arm_reach::Goal g;
        g.hand = goal->left ? arm_reach::Hand::left : arm_reach::Hand::right;
        g.at = goal->at;
        if (goal->rotation) g.rotation = *goal->rotation;
        g.weight = goal->weight;
        skater_animation::reach(g, shown_of(goal->shown));
        return {};
    }
    static Tuning tuning_of(const trainer::TuningValue& v) {
        const char* kind = v.kind == trainer::Kind::integer ? "integer"
                         : v.kind == trainer::Kind::flag ? "flag"
                         : v.kind == trainer::Kind::curve || v.kind == trainer::Kind::graph ? "multiplier" : "number";
        return {v.id, v.label, v.group, kind, v.value, v.stock, v.tuner};
    }
    std::optional<Tuning> tuning(std::string_view id, std::string& why) override {
        const auto v = trainer::tuning_value(id, &why);
        if (!v) return std::nullopt;
        return tuning_of(*v);
    }
    std::vector<Tuning> find_tuning(std::string_view text, std::size_t most) override {
        std::vector<Tuning> out;
        for (const auto& v : trainer::find_tuning(text, most)) out.push_back(tuning_of(v));
        return out;
    }
    std::string tune(std::string_view plugin, std::string_view id, std::optional<double> value) override {
        return trainer::plugin_tune(plugin, id, value);
    }
    std::optional<JointPlace> skater_joint(std::uint64_t player, std::uint16_t joint) override {
        if (!player) {
            const auto at = skater_animation::joint(joint);
            if (!at) return std::nullopt;
            return JointPlace{at->position, at->rotation, at->scale[0]};
        }
        const auto at = plugin_feed::joint_place(player, joint);
        if (!at) return std::nullopt;
        return JointPlace{at->position, at->rotation, at->scale};
    }
    std::optional<HandPlace> skater_hand(bool left) override {
        const auto placed = skater_animation::hand(left ? arm_reach::Hand::left : arm_reach::Hand::right);
        if (!placed) return std::nullopt;
        return HandPlace{placed->position, placed->rotation};
    }
    std::string replace_trick(std::string_view plugin, std::string trick, std::string pop, ClipRef clip, float speed) override {
        const auto key = std::make_pair(trick, pop);
        if (const auto found = replacements_.find(key); found != replacements_.end() && found->second.first != plugin)
            return found->second.first + " replaces that trick already";
        if (!clip) replacements_.erase(key);
        else {
            const auto mine = std::count_if(replacements_.begin(), replacements_.end(), [&](const auto& r) { return r.second.first == plugin; });
            if (!replacements_.contains(key) && mine >= 32) return "a plugin replaces at most 32 tricks";
            replacements_[key] = {std::string(plugin), skater_animation::Replacement{std::move(trick), std::move(pop), std::move(clip), speed}};
        }
        publish_replacements();
        return {};
    }
    std::string record_clip(std::string_view plugin, float seconds) override {
        if (!recorder_.empty() && recorder_ != plugin && skater_animation::recording()) return recorder_ + " is recording";
        if (!skater_animation::record(seconds)) return "a recording is going on";
        recorder_ = plugin;
        return {};
    }
    ClipRef take_recording(std::string_view plugin) override {
        if (recorder_ != plugin || skater_animation::recording()) return nullptr;
        auto clip = skater_animation::take_recording();
        if (clip) recorder_.clear();
        return clip;
    }
    std::string current_trick() override { return skater_animation::current_trick(); }
    void tick_animation(std::uintptr_t base, std::uintptr_t client) {
        xray::tick();
        hud_corner::tick();
        game_speed::tick();
        launches::tick(client);
        outfit_overlay::tick();
        board_pose::tick(base, local_skater_component(base, client));
        board_material::tick(base, client);
        const bool allowed = !multiplayer::local_throwdown_active();
        const auto component = local_skater_component(base, client);
        skater_animation::tick(base, component, allowed);
        // The character room's walk: its scripted pad moves on by game ticks; once there, the skater shows in it.
        overlay::pad_tick();
        character_room::tick(base, component, component != 0, false);
        const auto room = character_room::phase();
        skater_animation::show_in_room(!room_owner_.empty() && room == character_room::Phase::arrived);
    }
    // ---- general services for a game layer in Lua ------------------------------------------------------
    std::optional<SkaterTelemetry> skater_telemetry(std::string_view plugin) override {
        const auto t = trainer::telemetry();
        const auto w = watched_physics_state();
        SkaterTelemetry m;
        m.skater = t.skater;
        m.airborne = t.airborne;
        m.position = t.position;
        m.heading = t.heading;
        m.speed = t.speed;
        m.vertical = t.vertical;
        m.air_time = t.air_time;
        m.height = t.height;
        m.top_speed = t.top_speed;
        m.physics_state = t.physics_state;
        const auto& j = t.last;
        m.last = {j.serial, j.takeoff_speed, j.takeoff_angle, j.air_time, j.height, j.distance, j.drop, j.landing_speed,
                  j.spin, j.spin_rate, j.flip, j.board_turn, j.state, j.takeoff, j.landing};
        m.watch = {w.valid, w.ragdoll_known, w.ragdoll, w.state, w.previous, w.previous_seconds, w.changes, w.wipeouts, w.runouts};
        m.teleport_pending = local_skater_teleport_pending();
        // Each plugin hears of a slam or a strike once, whoever else reads the telemetry; its first
        // read only takes its place.
        const auto slams = bonecam::bone_cam_slams(), strikes = bonecam::bone_cam_strikes();
        auto seen = telemetry_seen_.find(plugin);
        if (seen == telemetry_seen_.end()) seen = telemetry_seen_.emplace(std::string(plugin), std::pair{slams, strikes}).first;
        m.slam = slams != seen->second.first;
        m.struck = strikes != seen->second.second;
        seen->second = {slams, strikes};
        m.body = bonecam::bail_body_position();
        return m;
    }
    std::optional<std::uint32_t> pad_bits(bool menu) override {
        ControllerInput pad;
        DingoSDKOverlayReadControllerInput(&pad, menu);
        if (!pad.available) return std::nullopt;
        return pad.buttons;
    }
    bool key_down(int virtual_key) override { return overlay::key_down(virtual_key); }
    int mouse_wheel() override { return overlay::take_mouse_wheel(); }
    // One read of the overlay's mouse a call, shared out: every plugin that read it in the last two
    // seconds gets the movement, so two plugins never take it from each other.
    std::string typed_text(bool capture) override { return overlay::take_plugin_text(capture); }
    std::string room_open(std::string_view plugin) override {
        if (!room_owner_.empty() && room_owner_ != plugin) return room_owner_ + " has the character room";
        if (multiplayer::local_throwdown_active()) return "not while a throwdown runs";
        const auto phase = character_room::phase();
        if (room_owner_ == plugin && (phase == character_room::Phase::opening || phase == character_room::Phase::arrived)) return {};
        room_owner_ = plugin;
        character_room::open();
        return {};
    }
    void room_leave(std::string_view plugin) override {
        if (room_owner_ != plugin) return;
        room_owner_.clear();
        skater_animation::show_in_room(false);
        character_room::leave();
    }
    std::string room_turn(std::string_view, float value, int stick) override {
        if (value != 0 && !multiplayer::sample_game_ui_state(base_).in_menu) return "the game's menu is not open";
        overlay::hold_stick(stick, value, 300);
        return {};
    }
    GameMenu game_menu() override {
        GameMenu menu;
        menu.open = multiplayer::sample_game_ui_state(base_).in_menu;
        if (!menu.open) return menu;
        const auto buttons = character_room::menu_buttons(base_);
        menu.skaters = character_room::is_skaters_page(buttons);
        std::size_t start = 0;
        while (start < buttons.size() && menu.buttons.size() < 64) {
            const auto end = std::min(buttons.find(' ', start), buttons.size());
            if (end > start) menu.buttons.push_back(buttons.substr(start, end - start));
            start = end + 1;
        }
        return menu;
    }
    std::optional<MenuGrid> menu_grid() override {
        const auto view = item_browser::view();
        if (!view.open) return std::nullopt;
        const auto prompts = overlay::item_browser_prompts();
        return MenuGrid{view.focused_key, view.focused_title, view.focused_category, view.grid_keys, view.shown, view.total,
                        view.showing, view.held, prompts.left, prompts.top, prompts.width, prompts.unit};
    }
    std::string grid_show(std::string_view plugin, std::optional<std::vector<std::string>> keys) override {
        return item_browser::show_items(std::string(plugin), std::move(keys));
    }
    void grid_mark(std::string_view plugin, std::vector<std::string> keys) override { item_browser::mark_items(std::string(plugin), std::move(keys)); }
    // ---- capture: one plugin records at a time (capture_runtime.h) --------------------------------
    std::string capture_start(std::string_view plugin, const CaptureStart& request) override {
        auto& recorder = capture::recorder();
        const auto now = recorder.status().state;
        const bool busy = now == capture::State::recording || now == capture::State::saving;
        if (busy && !capture_owner_.empty() && capture_owner_ != plugin) return capture_owner_ + " is capturing";
        capture::RecordRequest r;
        r.area = {request.left, request.top, request.right, request.bottom, request.menu};
        r.depth = request.depth;
        r.height = request.height;
        r.skip = request.skip;
        r.most = request.most;
        r.seconds = request.seconds;
        std::string why;
        if (!recorder.start(r, why)) return why;
        capture_owner_ = plugin;
        capture_file_.clear();
        return {};
    }
    void capture_stop(std::string_view plugin) override {
        if (capture_owner_ == plugin) capture::recorder().stop();
    }
    void capture_discard(std::string_view plugin) override {
        // While it saves, the save goes on and is still this plugin's to read.
        if (capture_owner_ != plugin || capture::recorder().status().state == capture::State::saving) return;
        capture::recorder().discard();
        capture_owner_.clear();
    }
    CaptureStatus capture_status(std::string_view plugin) override {
        if (capture_owner_ != plugin) return {"idle"};
        static constexpr const char* names[] = {"idle", "recording", "stopped", "saving", "saved", "failed"};
        const auto s = capture::recorder().status();
        CaptureStatus out{names[static_cast<int>(s.state)], s.taken, s.kept, s.depth, s.message, {}};
        if (s.state == capture::State::saved) out.file = capture_file_;
        return out;
    }
    float capture_compare(std::string_view plugin, std::uint32_t a, std::uint32_t b) override {
        return capture_owner_ == plugin ? capture::recorder().compare(a, b) : -1.0f;
    }
    std::vector<std::uint32_t> capture_kept(std::string_view plugin) override {
        return capture_owner_ == plugin ? capture::recorder().kept() : std::vector<std::uint32_t>{};
    }
    std::string capture_save(std::string_view plugin, const CaptureSave& request) override {
        if (capture_owner_ != plugin) return "nothing was recorded";
        std::filesystem::path root, file;
        if (auto why = data_file(plugin, request.path, root, file); !why.empty()) return why;
        std::error_code error;
        if (std::filesystem::is_directory(file, error)) return "a folder is there";
        capture::SaveRequest r;
        r.frames = request.frames;
        r.format = request.format == "apng" ? capture::Format::apng : request.format == "png" ? capture::Format::png
                   : request.format == "webm" ? capture::Format::webm : capture::Format::gif;
        r.fps = request.fps;
        r.size = request.size;
        if (request.background)
            r.background = capture::Rgb{static_cast<std::uint8_t>(*request.background >> 16), static_cast<std::uint8_t>(*request.background >> 8),
                                        static_cast<std::uint8_t>(*request.background)};
        r.cutout = request.cutout == "depth" ? capture::Cutout::depth : request.cutout == "colour" ? capture::Cutout::colour
                   : request.cutout == "none" ? capture::Cutout::none : capture::Cutout::automatic;
        r.solid = request.solid;
        r.card_title = request.card_title;
        for (const auto& [label, value] : request.card) r.card.push_back({label, value});
        r.fonts = capture::menu_fonts();
        r.file = file;
        std::string why;
        capture::recorder().stop();
        if (!capture::recorder().save(std::move(r), why)) return why;
        capture_file_ = request.path;
        return {};
    }
    RoomState room_state(std::string_view plugin) override {
        using P = character_room::Phase;
        const auto phase = character_room::phase();
        if (room_owner_ != plugin) return {phase == P::leaving ? "leaving" : "idle", {}};
        switch (phase) {
        case P::opening: return {"opening", {}};
        case P::arrived: return {"open", {}};
        case P::leaving: return {"leaving", {}};
        case P::failed: return {"failed", character_room::failure()};
        default: return {"idle", {}};
        }
    }
    std::optional<Cursor> cursor() override {
        overlay::PluginCursor c;
        if (!overlay::read_plugin_cursor(c)) return std::nullopt;
        return Cursor{c.x, c.y, c.wheel, c.left, c.right, c.middle, c.over_screen};
    }
    std::optional<MouseMotion> mouse_motion(std::string_view plugin) override {
        overlay::MouseLook look;
        const bool live = overlay::read_mouse_look(look);
        const auto now = GetTickCount64();
        std::erase_if(mouse_readers_, [&](const auto& entry) { return now - entry.second.at > 2000; });
        for (auto& [who, reader] : mouse_readers_) {
            reader.motion.x += look.x;
            reader.motion.y += look.y;
            reader.motion.wheel += look.wheel / 120.0;
        }
        auto& mine = mouse_readers_[std::string(plugin)];
        if (!mine.at) mine.motion = {static_cast<double>(look.x), static_cast<double>(look.y), look.wheel / 120.0};
        mine.at = now;
        const auto out = std::exchange(mine.motion, {});
        if (!live) return std::nullopt;
        return out;
    }
    void game_buttons(std::string_view plugin, std::uint16_t hidden, std::uint16_t held, bool paused) override {
        buttons_owner_ = plugin;
        overlay::hide_game_buttons(hidden);
        overlay::hold_game_buttons(held);
        if (paused) overlay::pause_game_input(true);
    }
    void game_keys(std::string_view plugin, std::vector<std::uint8_t> keys) override {
        if (keys.empty() && keys_owner_ != plugin) return;
        keys_owner_ = keys.empty() ? std::string{} : std::string(plugin);
        overlay::hide_game_keys(keys);
    }
    bool input_allowed() override {
        DWORD process{};
        const auto window = GetForegroundWindow();
        const bool focused = window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
        return focused && !overlay::interface_open() && multiplayer::native_throwdown_gameplay_input_allowed();
    }
    std::string free_camera(std::string_view plugin, bool on) override {
        if (on) free_camera_owner_ = plugin;
        else if (free_camera_owner_ != plugin) return {};
        else free_camera_owner_.clear();
        console::request_debug(overlay::DebugAction::set_free_camera, on);
        return {};
    }
    std::string pad_flight(std::string_view plugin, bool on) override {
        if (on) {
            if (!pad_flight_owner_.empty() && pad_flight_owner_ != plugin) return pad_flight_owner_ + " flies the camera from the pad";
            pad_flight_owner_ = plugin;
        } else if (pad_flight_owner_ != plugin) return {};
        else pad_flight_owner_.clear();
        ::dingosdk::pad_flight::request(on);
        return {};
    }
    void competition(std::string_view plugin, bool on) override {
        if (on) competition_owner_ = plugin;
        else if (competition_owner_ != plugin) return;
        else competition_owner_.clear();
        if (on) physics_tuning::prepare();
        set_competition_stock_physics(on);
    }
    bool stock_physics_ready() override { return physics_tuning::game_tuning() != nullptr; }
    std::string world_map() override { return local_profile_object_persistence().map; }
    std::uint64_t world_id() override {
        // The session's world in a session, else the native throwdown world (the one native flag
        // placement and the flags echo back).
        const auto world = multiplayer_session_active() ? multiplayer::throwdown_relay_world() : 0;
        return world ? world : multiplayer::native_throwdown_world();
    }
    bool world_barred() override { return multiplayer::throwdown_relay_barred(); }
    std::optional<bool> stance_goofy() override { return profile_runtime::game_stance_goofy(); }
    void trick_feed(std::string_view plugin, bool on) override { tricks::feed::ask(std::string(plugin), on); }
    std::vector<TrickNote> take_trick_notes() override {
        std::vector<TrickNote> out;
        for (auto& n : tricks::feed::take()) out.push_back({n.entry, n.kind, std::move(n.text)});
        return out;
    }
    void prepare_physics() override { physics_tuning::prepare(); }
    InputFocus input_focus() override {
        DWORD process{};
        const auto window = GetForegroundWindow();
        return {window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId(), overlay::interface_open(),
                multiplayer::native_throwdown_gameplay_input_allowed()};
    }
    void spectate(std::uint64_t player) override { multiplayer::spectate_party_member(player); }
    std::vector<std::array<float, 2>> trick_gesture(std::string_view trick) override {
        tricks::prepare_trick_gestures();
        return tricks::trick_gesture(trick);
    }
    // ---- content: <mod>/<folder>/ of every enabled mod (plugin_content.h) --------------------------
    // The enabled mods, read again at most every 2 s: a page may list them every frame.
    std::vector<content::Mod> enabled_mods() {
        const auto now = GetTickCount64();
        if (enabled_read_at_ && now - enabled_read_at_ < 2000) return enabled_mods_;
        enabled_read_at_ = now;
        enabled_mods_.clear();
        const auto list = mods::scan_mods(mods::engine_data_root());
        if (list.issue.empty())
            for (const auto& entry : list.entries)
                if (entry.enabled) enabled_mods_.push_back({entry.mod.name, entry.mod.title.empty() ? entry.mod.name : entry.mod.title, entry.mod.directory});
        return enabled_mods_;
    }
    std::string content_list(std::string_view folder, std::vector<ContentFile>& files) override {
        content::list(enabled_mods(), folder, files);
        return {};
    }
    std::string content_read(std::string_view mod, std::string_view folder, std::string_view path, std::string& text) override {
        return content::read(enabled_mods(), mod, folder, path, text);
    }
    std::string trick_label(std::string_view plugin, std::string_view from, std::string_view to) override {
        return trick_rewrite::label(plugin, from, to);
    }
    // A trick recorded as another changes the score: not while a throwdown runs, as trick replacements.
    std::string trick_record_as(std::string_view plugin, std::vector<std::uint32_t> from, std::uint32_t to, int within_ms) override {
        if (multiplayer::local_throwdown_active()) return "not while a throwdown runs";
        return trick_rewrite::record_as(plugin, std::move(from), to, within_ms);
    }
    std::string bone_cam(std::string_view plugin, const std::vector<std::string>& words) override {
        if (words.empty()) return bonecam::bone_cam_setting();
        const auto& setting = words.front();
        if (words.size() == 1 && setting == "status") return bonecam::bone_cam_command({});
        if (words.size() == 1 && setting == "bail") {
            bonecam::bone_cam_bail();
            return {};
        }
        if (words.size() == 1 && setting == "ringing") return bonecam::bone_cam_ringing() ? "1" : "0";
        // "game on|off": this plugin's game asks for the Bone Cam (the "game" setting).
        if (words.size() == 2 && setting == "game" && (words[1] == "on" || words[1] == "off")) {
            bonecam::bone_cam_game(plugin, words[1] == "on");
            return {};
        }
        return bonecam::bone_cam_command(words);
    }
    // ---- the lobby host's authority ------------------------------------------------------------------
    std::string kick(std::uint64_t player, std::string_view why) override { return multiplayer::host_kick(player, why); }
    // The host's own chat, under the plugin's name: a lobby has no server to speak for it.
    std::string announce(std::string_view plugin, std::string_view text) override {
        if (!multiplayer_session_active()) return "not in a session";
        const auto answer = multiplayer::command("chat", "[" + std::string(plugin) + "] " + std::string(text));
        return answer == "Message sent." ? std::string{} : answer;
    }
    std::string move_player(std::uint64_t player, const Vec3& at) override { return multiplayer::host_teleport(player, at); }
    std::string tell(std::string_view, std::uint64_t, std::string_view) override {
        return "a lobby has no private chat: send that player's copy a message (reskate.net.send) and show it there";
    }
    // ---- camera ------------------------------------------------------------------------------------
    std::optional<CameraView> camera_view() override {
        const auto view = plugin_camera::camera_now();
        if (!view || !(view->vertical_fov > 1 && view->vertical_fov < 175)) return std::nullopt;
        const auto& m = view->world;
        const auto free = client_free_camera_view();
        return CameraView{{m[12], m[13], m[14]}, {-m[8], -m[9], -m[10]}, view->vertical_fov, {m[0], m[1], m[2]}, {m[4], m[5], m[6]},
                          free != 0 && view->camera == free};
    }
    std::string set_camera(std::string_view plugin, const std::array<float, 16>& pose, float fov) override {
        if (!camera_holder_.empty() && camera_holder_ != plugin) return camera_holder_ + " has the camera";
        if (!follower_.empty() && follower_ != plugin) return follower_ + " is following a player with it";
        camera_holder_ = plugin;
        camera_pose_ = pose;
        camera_fov_ = fov;
        camera_releasing_ = false;
        return {};
    }
    std::string follow_player(std::string_view plugin, std::uint64_t player) override {
        if (!camera_holder_.empty() && camera_holder_ != plugin) return camera_holder_ + " has the camera";
        if (!player) {
            if (follower_ == plugin) {
                multiplayer::spectate_party_member(0);
                follower_.clear();
            }
            return {};
        }
        if (!multiplayer_session_active()) return "not in a session";
        if (camera_holder_ == plugin) release_camera(plugin);
        follower_ = plugin;
        multiplayer::spectate_party_member(player);
        return {};
    }
    void release_camera(std::string_view plugin) override {
        if (follower_ == plugin) follow_player(plugin, 0);
        if (camera_holder_ != plugin) return;
        camera_holder_.clear();
        camera_releasing_ = true;
        camera_status_.clear();
    }
    std::string camera_status(std::string_view plugin) override {
        if (follower_ == plugin) return multiplayer::party_spectating() ? std::string{} : "Spectate is starting";
        if (camera_holder_ != plugin) return "it does not hold the camera";
        return camera_shown_ ? std::string{} : camera_status_.empty() ? "waiting for the game's camera" : camera_status_;
    }
    void apply_camera(std::uintptr_t base, std::uintptr_t client, bool ready, bool phase) {
        if (multiplayer::party_spectating()) {
            // Spectate has the lease now, and gives the game's camera back itself.
            camera_shown_ = camera_releasing_ = false;
            if (!camera_holder_.empty()) camera_status_ = "Spectate has the camera";
            return;
        }
        std::string detail;
        if (!camera_holder_.empty()) {
            camera_shown_ = update_party_camera(base, client, ready, phase, &camera_pose_, detail, camera_fov_);
            if (!camera_shown_ && !detail.empty()) camera_status_ = detail;
        } else if (camera_releasing_ && update_party_camera(base, client, ready, phase, nullptr, detail)) {
            camera_releasing_ = false;
        }
    }
    std::string freeze(std::string_view plugin, bool on) override {
        const bool before = !freezers_.empty();
        if (on) {
            if (!before && !session_noclip_allowed()) return "the host has turned noclip off";
            freezers_.insert(std::string(plugin));
        } else freezers_.erase(std::string(plugin));
        const bool now = !freezers_.empty();
        if (now == before) return {};
        const auto why = queue(overlay::DebugAction::set_noclip, now);
        if (!why.empty() && now) freezers_.erase(std::string(plugin));
        return why;
    }
    bool frozen() const noexcept { return !freezers_.empty(); }
    std::vector<WornItem> outfit_worn() override {
        std::vector<WornItem> out;
        for (auto& w : worn_items::worn_outfit()) out.push_back({std::move(w.slot), std::move(w.item), std::move(w.title)});
        return out;
    }
    std::vector<WornItem> board_parts() override {
        std::vector<WornItem> out;
        for (auto& w : worn_items::worn_board()) out.push_back({std::move(w.slot), std::move(w.item), std::move(w.title)});
        return out;
    }
    std::string item_title(std::string_view key) override { return worn_items::item_title(std::string(key)); }
    std::string files_reveal(std::string_view plugin, std::string_view folder) override {
        std::filesystem::path root, at;
        if (auto why = data_file(plugin, folder, root, at); !why.empty()) return why;
        std::error_code error;
        std::filesystem::create_directories(at, error);
        if (error || !std::filesystem::is_directory(at, error)) return "not a folder";
        const auto done = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", at.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        return done > 32 ? std::string{} : std::string("the folder could not be opened");
    }
    void release(std::string_view plugin) override {
        if (capture_owner_ == plugin) {
            capture::recorder().discard();
            capture_owner_.clear();
        }
        if (hold_owner_ == plugin) (void)skater_hold(plugin, false);
        plugin_feed::release_hidden(std::string(plugin));
        world_entities::clear(plugin);
        throwdown_flag_leave(plugin);
        dialog_close(plugin, {});
        tricks::feed::ask(std::string(plugin), false);
        bonecam::bone_cam_release(plugin);
        if (keys_owner_ == plugin) {
            overlay::hide_game_keys({});
            keys_owner_.clear();
        }
        if (buttons_owner_ == plugin) {
            overlay::hide_game_buttons(0);
            overlay::hold_game_buttons(0);
            buttons_owner_.clear();
        }
        if (free_camera_owner_ == plugin) (void)free_camera(plugin, false);
        if (pad_flight_owner_ == plugin) (void)pad_flight(plugin, false);
        if (competition_owner_ == plugin) competition(plugin, false);
        stop_clip(plugin);
        plugin_sound::stop_all(plugin);
        outfit_overlay::clear(plugin);
        item_browser::release_owner(plugin);
        (void)outfit_overlay::set_layers(std::string(plugin), {});
        board_pose::clear(plugin);
        board_pose::park(plugin, {});
        (void)board_pose::fit(plugin, std::nullopt);
        board_material::clear(plugin);
        cancel_throwdown(plugin);
        multiplayer::custom_throwdowns::remove_plugin(plugin);
        if (capturer_ == plugin) capturer_.clear();
        if (std::erase_if(replacements_, [&](const auto& r) { return r.second.first == plugin; })) publish_replacements();
        if (recorder_ == plugin) recorder_.clear();
        release_camera(plugin);
        (void)freeze(plugin, false);
        hide_hud(plugin, false);
        xray::prepare(plugin, false);
        xray::hide(plugin, false);
        hud_corner::release(plugin);
        game_speed::ask(plugin, 1.0f);
        held_settings::release(plugin);
        launches::release(plugin);
        pad_hide::release(plugin);
        trick_rewrite::release(plugin);
        trainer::release_plugin_tunes(plugin);
        if (reacher_ == plugin) {
            skater_animation::reach(std::nullopt);
            reacher_.clear();
        }
        if (looker_ == plugin) {
            screen_look::clear();
            looker_.clear();
        }
        if (poser_ == plugin) {
            skater_animation::pose({}, 1);
            poser_.clear();
        }
        if (carrier_ == plugin) {
            skater_animation::carry(std::nullopt);
            carrier_.clear();
        }
        if (cycler_ == plugin) {
            first_person::plugin_link.flags.store(0, std::memory_order_release);
            cycler_.clear();
        }
        hide_nametags(plugin, false);
        if (room_owner_ == plugin) room_leave(plugin);
        if (const auto seen = telemetry_seen_.find(plugin); seen != telemetry_seen_.end()) telemetry_seen_.erase(seen);
        plugin_feed::release_tags(std::string(plugin));
        if (std::erase_if(binds_, [&](const auto& item) { return item.first.first == plugin; })) publish_binds();
    }
    void set_storage(const std::filesystem::path& storage) {
        if (!binds_file_.empty() || storage.empty()) return;
        binds_file_ = storage / "binds.json";
        load_overrides();
    }

private:
    void publish_replacements() {
        std::vector<skater_animation::Replacement> list;
        for (const auto& [key, value] : replacements_) list.push_back(value.second);
        skater_animation::set_replacements(std::move(list));
    }
    std::string player_, recorder_, thrower_, capturer_;
    std::string keys_owner_; // the plugin whose keys the game does not see
    std::string buttons_owner_, free_camera_owner_, pad_flight_owner_, competition_owner_, hold_owner_;
    std::uint64_t capture_until_{}, flight_read_at_{};
    std::optional<FlightInput> flight_;
    // By (trick, pop): the plugin and its clip.
    std::map<std::pair<std::string, std::string>, std::pair<std::string, skater_animation::Replacement>> replacements_;
    // What each plugin's mod held of variants when last read (variants).
    struct VariantRead {
        std::uint64_t at{};
        bool declared{};
        std::vector<std::string> installed;
    };
    std::map<std::string, VariantRead, std::less<>> variant_reads_;
    std::vector<content::Mod> enabled_mods_;
    std::string reacher_; // the plugin holding the skater's arm (skater_reach)
    std::string poser_;   // the plugin turning the skater's joints (skater_pose)
    std::string carrier_; // the plugin having the skater carry something (skater_carry)
    std::uint64_t carry_at_{};
    std::set<std::string, std::less<>> nametag_hiders_; // the plugins holding the nametags off (hide_nametags)
    std::string cycler_; // the plugin whose camera is in the camera height cycle (camera_cycle)
    std::uint64_t cycle_at_{};
    std::string room_owner_; // the plugin in the character room (room_open)
    std::uint64_t pose_at_{};
    std::string looker_;  // the plugin whose look is on the screen (screen_look)
    std::uint64_t look_at_{};
    std::uint64_t reach_at_{};
    struct MouseReader {
        MouseMotion motion;
        std::uint64_t at{};
    };
    std::map<std::string, MouseReader, std::less<>> mouse_readers_;
    std::uint64_t enabled_read_at_{};
    // The Bone Cam's slam and strike counts each plugin last read with its telemetry.
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>, std::less<>> telemetry_seen_;
    using BindKey = std::pair<std::string, std::string>;
    struct Bind {
        std::uint32_t own{};
        ControllerComboLatch latch;
    };
    std::uint32_t combo_in_use(const BindKey& key, const Bind& bind) const {
        const auto found = overrides_.find(key);
        return found != overrides_.end() ? found->second : bind.own;
    }
    void publish_binds() {
        std::vector<plugin_ui::Bind> list;
        for (const auto& [key, bind] : binds_) list.push_back({key.first, key.second, bind.own, combo_in_use(key, bind)});
        plugin_ui::publish_binds(std::move(list));
    }
    void apply_rebinds() {
        auto changes = plugin_ui::take_rebinds();
        if (changes.empty()) return;
        for (auto& change : changes) {
            BindKey key{std::move(change.plugin), std::move(change.action)};
            if (!change.combo) overrides_.erase(key);
            else if (!*change.combo || valid_action_binding(*change.combo)) overrides_[std::move(key)] = *change.combo;
        }
        save_overrides();
        publish_binds();
    }
    // binds.json: {"<plugin>": {"<action>": combo}}, the player's choices only.
    void load_overrides() {
        try {
            std::ifstream in(binds_file_, std::ios::binary);
            if (!in) return;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto json = Json::parse(text, {256 * 1024, 4, 4096});
            if (!json.is_object()) return;
            for (const auto& [plugin, actions] : json.items()) {
                if (!actions.is_object()) continue;
                for (const auto& [action, combo] : actions.items())
                    if (combo.is_number_unsigned() && combo.get<std::uint64_t>() <= 0xffffffffu)
                        overrides_[{plugin, action}] = static_cast<std::uint32_t>(combo.get<std::uint64_t>());
            }
        } catch (const std::exception&) {
            overrides_.clear(); // a broken file is the plugins' own binds again
        }
    }
    void save_overrides() {
        if (binds_file_.empty()) return;
        auto json = Json::object();
        for (const auto& [key, combo] : overrides_) {
            if (!json.contains(key.first)) json[key.first] = Json::object();
            json[key.first][key.second] = combo;
        }
        try {
            std::filesystem::create_directories(binds_file_.parent_path());
            auto temporary = std::filesystem::path(binds_file_) += ".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                const auto text = json.dump(2);
                if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) return;
            }
            std::filesystem::rename(temporary, binds_file_);
        } catch (const std::exception&) {
        }
    }
    static std::string reply(const std::string& answer) {
        return answer.starts_with("error") || answer.find("not") != std::string::npos ? answer : std::string{};
    }
    // As the console's `tod`: each map has seven time layers, "<map>_tod_<n>_<name>"; one is
    // forced on and the rest off, or all seven go back to the level's own choice.
    std::string time_of_day(std::string_view wanted) {
        static constexpr std::array<std::string_view, 8> times{"default", "morning", "noon", "afternoon", "evening", "night",
                                                               "weatherday", "weathernight"};
        const auto found = std::find(times.begin(), times.end(), wanted);
        if (found == times.end()) return "a time of day is default, morning, noon, afternoon, evening, night, weatherday or weathernight";
        const auto slot = static_cast<std::size_t>(found - times.begin());
        const auto world = local_profile_world_layers();
        if (world.controlled_by_host) return "the host chooses the time of day";
        if (world.map == WorldMap::none) return "no map is loaded";
        std::vector<std::size_t> layers;
        for (unsigned n = 1; n < times.size(); ++n) {
            const auto prefix = std::string(world_map_key(world.map)) + "_tod_" + std::to_string(n) + "_";
            const auto layer = std::find_if(world_layers().begin(), world_layers().end(),
                                            [&](const auto& item) { return item.key.starts_with(prefix); });
            if (layer == world_layers().end()) return "this map has no time of day";
            layers.push_back(static_cast<std::size_t>(layer - world_layers().begin()));
        }
        // The others switch off before the new one comes on, so two never load together.
        bool saved = true;
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (i + 1 != slot) saved = saved && set_local_world_layer(world_layers()[layers[i]].key, slot ? "off" : "default");
        if (slot) saved = saved && set_local_world_layer(world_layers()[layers[slot - 1]].key, "on");
        return saved ? std::string{} : local_profile_world_layers().feedback;
    }

    std::uintptr_t base_;
    std::string capture_owner_, capture_file_; // the plugin whose capture it is, and where it saved to
    std::string camera_holder_, follower_, camera_status_;
    std::array<float, 16> camera_pose_{};
    float camera_fov_{};
    bool camera_shown_{}, camera_releasing_{};
    std::set<std::string, std::less<>> freezers_;
    std::set<std::string, std::less<>> hud_hiders_;
    bool hud_was_hidden_{};
    bool music_started_{};
    std::map<BindKey, Bind> binds_;
    std::map<BindKey, std::uint32_t> overrides_;
    std::filesystem::path binds_file_;
};
RealGame* the_game{};
}

Game& real_game(std::uintptr_t image_base, const std::filesystem::path& storage) {
    static auto* game = new RealGame(image_base); // the process's lifetime, as the host's
    the_game = game;
    game->set_storage(storage);
    return *game;
}
void apply_plugin_camera(std::uintptr_t image_base, std::uintptr_t client, bool ready, bool phase) noexcept {
    try {
        if (the_game) the_game->apply_camera(image_base, client, ready, phase);
    } catch (...) {}
}
bool plugin_freeze_active() noexcept { return the_game && the_game->frozen(); }
bool plugin_input_capture() noexcept { return the_game && the_game->capturing(); }
void tick_plugin_sounds(std::uintptr_t image_base, std::uintptr_t client, bool ready) noexcept {
    plugin_sound::tick(image_base, client, ready);
}
void tick_plugin_animation(std::uintptr_t image_base, std::uintptr_t client) noexcept {
    try {
        if (the_game) the_game->tick_animation(image_base, client);
    } catch (...) {}
}
}
