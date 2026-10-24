#include "Extension/Console/commands.h"
#include "Extension/AnimationStudio/anim_studio.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "ai_skaters.h"
#include "Engine/Game/Skater/velocity_boost.h"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <utility>
#include <vector>
namespace dingosdk::console {
void register_movement_commands(Commands &registry) {
    using Debug = overlay::DebugAction;
    struct Setting {
        const char *name;
        const char *alias;
        const char *description;
        Debug action;
        bool overlay::DebugModel::*value;
        bool overlay::DebugModel::*available;
    };
    const Setting settings[]{
        {"noclip", "debug.noclip", "Fly the local skater through the world", Debug::set_noclip,
         &overlay::DebugModel::noclip, &overlay::DebugModel::noclip_available},
        {"nobail", "debug.no_bail", "Prevent wipeouts; also activated automatically during noclip", Debug::set_no_bail,
         &overlay::DebugModel::no_bail, &overlay::DebugModel::no_bail_available},
        {"freecam", "debug.freecam", "Move the camera independently of the skater", Debug::set_free_camera,
         &overlay::DebugModel::free_camera, &overlay::DebugModel::camera_available},
        {"firstperson", "debug.first_person", "See through the skater's eyes", Debug::set_first_person,
         &overlay::DebugModel::first_person, &overlay::DebugModel::camera_available},
        {"hideui", "debug.game_ui_hidden", "Hide the game's interface", Debug::set_game_ui_hidden,
         &overlay::DebugModel::game_ui_hidden, &overlay::DebugModel::ui_available}};
    for (const auto &setting : settings) {
        auto entry = variable(setting.name, setting.description, Group::movement, argument("0|1", Type::boolean));
        entry.aliases = {setting.alias};
        entry.inspect = [setting](const Model &m) {
            auto result =
                boolean_state(m.debug.*setting.available, m.debug.*setting.value, "Waiting for local player controls.");
            if (setting.action == Debug::set_noclip && !result.available)
                result.reason = m.debug.noclip_unavailable;
            if (setting.action == Debug::set_free_camera && !result.available)
                result.reason = m.debug.camera_unavailable;
            if (setting.action == Debug::set_no_bail) {
                result.value = m.debug.no_bail ? "1" : "0";
                result.detail = m.debug.no_bail_active ? "Protection active" : "Protection inactive";
                if (m.debug.noclip && m.debug.no_bail_active)
                    result.detail += "; automatic during noclip";
            }
            return result;
        };
        entry.run = [setting](const Model &, const Values &args, const Output &) {
            request_debug(setting.action, std::get<bool>(args[0]));
        };
        entry.reset = [setting](const Model &, const Output &out) {
            if (setting.action == Debug::set_no_bail || setting.action == Debug::set_noclip)
                request_debug(setting.action, false);
            else {
                out("Restoring the shared movement overrides.");
                request_debug(Debug::restore_debug);
            }
        };
        registry.add(std::move(entry));
    }
    auto speed = argument("speed", Type::number);
    // Any speed in range; the usual ones are offered as completions.
    speed.minimum = 0.6;
    speed.maximum = 1500.0;
    speed.complete = [](const Model &, auto) {
        return std::vector<std::string>{"0.6", "3", "5", "15", "60", "300", "1500"};
    };
    auto flight = variable("flyspeed", "Free-flight speed in world units per second (0.6-1500)", Group::movement, speed);
    flight.aliases = {"debug.camera_speed"};
    flight.inspect = [](const Model &m) {
        return State{m.debug.available,
                     m.debug.available
                         ? std::optional<std::string>(value_text(static_cast<double>(m.debug.camera_speed)))
                         : std::nullopt,
                     "Waiting for local player controls.",
                     {},
                     false};
    };
    flight.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_camera_speed, false, static_cast<float>(std::get<double>(args[0])));
    };
    flight.reset = [](const Model &, const Output &) { request_debug(Debug::set_camera_speed, false, 15.0f); };
    registry.add(std::move(flight));
    auto fov_argument = argument("degrees", Type::number);
    fov_argument.minimum = 0;
    fov_argument.maximum = 120;
    auto free_fov = variable("freecamfov", "Freecam field of view in degrees (40-120; 0 = the game's own)",
        Group::movement, fov_argument);
    free_fov.inspect = [](const Model &m) {
        return State{m.debug.available, value_text(static_cast<double>(m.debug.free_camera_fov)),
                     "Waiting for local player controls.", {}, false};
    };
    free_fov.run = [](const Model &, const Values &args, const Output &out) {
        const auto degrees = std::get<double>(args[0]);
        if (degrees != 0 && degrees < 40) {
            out("error: Use 0 or 40-120.");
            return;
        }
        request_debug(Debug::set_free_camera_fov, false, static_cast<float>(degrees));
    };
    free_fov.reset = [](const Model &, const Output &) { request_debug(Debug::set_free_camera_fov, false, 0.0f); };
    registry.add(std::move(free_fov));
    // Dark Pop (see dark_pop_tick in client_debug.cpp); the same switches as the menu rows.
    auto dark_pop = variable("darkpop", "Dark Pop: catch a flip dark, press D-pad Left, Down or Right in the air, land it dark",
        Group::movement, argument("0|1", Type::boolean));
    dark_pop.inspect = [](const Model &m) {
        return State{m.debug.available, m.debug.dark_pop ? "1" : "0", "Waiting for local player controls.",
                     std::to_string(m.debug.dark_pop_count) + " pops", false};
    };
    dark_pop.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_dark_pop_enabled, std::get<bool>(args[0]));
    };
    dark_pop.reset = [](const Model &, const Output &) { request_debug(Debug::set_dark_pop_enabled, false); };
    registry.add(std::move(dark_pop));
    auto dark_probe = variable("darkpopprobe", "Dark pop probe: record the game's state choices to two files in logs",
        Group::movement, argument("0|1", Type::boolean));
    dark_probe.inspect = [](const Model &m) {
        return State{m.debug.available, m.debug.probe ? "1" : "0", "Waiting for local player controls.", {}, false};
    };
    dark_probe.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_probe_enabled, std::get<bool>(args[0]));
    };
    dark_probe.reset = [](const Model &, const Output &) { request_debug(Debug::set_probe_enabled, false); };
    registry.add(std::move(dark_probe));
    auto boost_speed_argument = argument("speed", Type::number);
    boost_speed_argument.minimum = 0; // 0: off, the stock setting
    boost_speed_argument.maximum = dingosdk::velocity_boost_speed_max; // the menu slider ends at 300
    auto boost_speed = variable("forwardvelocity", "Velocity added along the skater's forward direction", Group::movement, boost_speed_argument);
    boost_speed.aliases = {"debug.forward_velocity_speed"};
    boost_speed.inspect = [](const Model &m) {
        return State{m.debug.available,
                     m.debug.available
                         ? std::optional<std::string>(value_text(static_cast<double>(m.debug.forward_velocity_speed)))
                         : std::nullopt,
                     "Waiting for local player controls.", {}, false};
    };
    boost_speed.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_forward_velocity_speed, false, static_cast<float>(std::get<double>(args[0])));
    };
    boost_speed.reset = [](const Model &, const Output &) {
        request_debug(Debug::set_forward_velocity_speed, false, 0.0f);
    };
    registry.add(std::move(boost_speed));
    auto up_speed_argument = boost_speed_argument;
    auto up_speed = variable("upvelocity", "Velocity added along world up", Group::movement, up_speed_argument);
    up_speed.aliases = {"debug.up_velocity_speed"};
    up_speed.inspect = [](const Model &m) {
        return State{m.debug.available,
                     m.debug.available
                         ? std::optional<std::string>(value_text(static_cast<double>(m.debug.up_velocity_speed)))
                         : std::nullopt,
                     "Waiting for local player controls.", {}, false};
    };
    up_speed.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_up_velocity_speed, false, static_cast<float>(std::get<double>(args[0])));
    };
    up_speed.reset = [](const Model &, const Output &) {
        request_debug(Debug::set_up_velocity_speed, false, 0.0f);
    };
    registry.add(std::move(up_speed));
    // TEST ONLY (not for the PR): a virtual controller, RESKATE_TEST_PAD=1.
    auto test_pad = action("testpad", "Test builds: play a script on the virtual controller", Group::movement,
                           {argument("script", Type::text)});
    test_pad.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    test_pad.run = [](const Model &, const Values &args, const Output &out) {
        out(DingoSDKOverlayTestPad(std::get<std::string>(args[0]).c_str()) ? "Test pad: playing." : "Test pad: off.");
    };
    registry.add(std::move(test_pad));
    auto test_pad_reads = action("testpadreads", "Test builds: how often the game has read the virtual controller", Group::movement);
    test_pad_reads.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    test_pad_reads.run = [](const Model &, const Values &, const Output &out) {
        unsigned all{}, by_game{};
        DingoSDKOverlayTestPadReads(&all, &by_game);
        out("Test pad reads answered: " + std::to_string(all) + ", from the game's own code: " + std::to_string(by_game));
    };
    registry.add(std::move(test_pad_reads));
    auto anim_save = action("animsave", "Animator Studio: save the animation under a name", Group::movement, {argument("name", Type::text)});
    anim_save.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    anim_save.run = [](const Model &, const Values &args, const Output &out) {
        anim_studio::rename(std::get<std::string>(args[0]));
        std::string message;
        anim_studio::save(message);
        out(message);
    };
    registry.add(std::move(anim_save));
    auto anim_time = action("animtime", "Animator Studio: put the playhead at a time on the trick (seconds)", Group::movement,
                            {argument("seconds", Type::number)});
    anim_time.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    anim_time.run = [](const Model &, const Values &args, const Output &) {
        anim_studio::set_time(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(anim_time));
    auto anim_key = action("animkey", "Animator Studio: key a joint's turn here (joint index, degrees X Y Z)", Group::movement,
                           {argument("joint", Type::unsigned_integer), argument("x", Type::number), argument("y", Type::number),
                            argument("z", Type::number)});
    anim_key.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    anim_key.run = [](const Model &, const Values &args, const Output &out) {
        const auto joint = static_cast<std::uint16_t>(std::get<std::uint64_t>(args[0]));
        if (!anim_studio::editable(joint)) {
            out("That joint is not one the studio edits.");
            return;
        }
        anim_studio::begin_change();
        auto value = anim_studio::offset_of(joint);
        value.rotation = anim_studio::from_euler({static_cast<float>(std::get<double>(args[1])), static_cast<float>(std::get<double>(args[2])),
                                                  static_cast<float>(std::get<double>(args[3]))});
        anim_studio::set_offset(joint, value);
        out("Keyed.");
    };
    registry.add(std::move(anim_key));
    auto boost = action("forwardboost", "Add the configured velocity along the skater's forward direction", Group::movement);
    boost.inspect = [](const Model &m) {
        return State{m.debug.forward_velocity_available, {}, m.debug.forward_velocity_unavailable, {}, false};
    };
    boost.run = [](const Model &, const Values &, const Output &) {
        request_debug(Debug::add_forward_velocity);
    };
    registry.add(std::move(boost));
    auto mask = argument("button_or_key", Type::unsigned_integer);
    mask.minimum = 0;
    mask.maximum = UINT32_MAX;
    auto freecamcontroller =
        action("freecam_controller", "Block input and use controller for Freecam", Group::movement, {argument("enabled", Type::boolean)});
    freecamcontroller.inspect = [](const Model &m) {
        return boolean_state(m.bindings.available, m.bindings.freecam_controller, "Controller bindings are unavailable.");
    };
    freecamcontroller.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_freecam_controller(std::get<bool>(args[0]));
        out(saved ? "" : "error: Invalid setting or save failed.");
    };
    registry.add(std::move(freecamcontroller));

    auto freecamcontroller_bind =
        action("bind freecamcontroller", "Bind a controller combo or keyboard key to freecam controller toggle; 0 clears the binding", Group::movement, {mask});
    freecamcontroller_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    freecamcontroller_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_freecam_controller_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(freecamcontroller_bind));

    auto tp_to_freecam =
        action("tp_to_freecam", "Teleport to freecam position and disable freecam", Group::movement);
    tp_to_freecam.inspect = [](const Model &m) {
        return boolean_state(m.debug.camera_available && m.debug.camera_position_valid, m.debug.free_camera, 
            !m.debug.camera_position_valid ? "Camera position unavailable" : "Camera controls unavailable");
    };
    tp_to_freecam.run = [](const Model &m, const Values &, const Output &) {
        if (!m.debug.camera_position_valid) return;
        teleport_local_skater(m.debug.camera_position);
        if (m.debug.free_camera)
            request_debug(overlay::DebugAction::set_free_camera, false);
    };
    registry.add(std::move(tp_to_freecam));

    auto tp_to_freecam_bind =
        action("bind tptofreecam", "Bind a controller combo or keyboard key to TP to Freecam; 0 clears the binding", Group::movement, {mask});
    tp_to_freecam_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    tp_to_freecam_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_tp_to_freecam_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(tp_to_freecam_bind));
    auto freecam_bind =
        action("bind freecam", "Bind a controller combo or keyboard key to freecam; 0 clears the binding", Group::movement, {mask});
    freecam_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    freecam_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_freecam_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(freecam_bind));
    auto bind =
        action("bind noclip", "Bind a controller combo or keyboard key to noclip; 0 clears the binding", Group::movement, {mask});
    bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_noclip_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(bind));
    auto boost_bind =
        action("bind forwardvelocity", "Bind a controller combo or keyboard key to Forward Boost; 0 clears the binding", Group::movement, {mask});
    boost_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    boost_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_forward_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(boost_bind));
    auto up_bind =
        action("bind upvelocity", "Bind a controller combo or keyboard key to Up Boost; 0 clears the binding", Group::movement, {mask});
    up_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    up_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_up_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(up_bind));
    auto offboard_up_bind =
        action("bind offboardupvelocity", "Bind a controller combo or keyboard key to Off-board Up Boost; 0 clears the binding", Group::movement, {mask});
    offboard_up_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    offboard_up_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_offboard_up_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(offboard_up_bind));
    for (const bool yes : {true, false}) {
        auto vote_bind = action(yes ? "bind voteyes" : "bind voteno",
            yes ? "Bind a controller combo or keyboard key to Yes in a server's vote; 0 clears the binding"
                : "Bind a controller combo or keyboard key to No in a server's vote; 0 clears the binding", Group::movement, {mask});
        vote_bind.inspect = [](const Model &m) {
            return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
        };
        vote_bind.run = [yes](const Model &, const Values &args, const Output &out) {
            const bool saved = set_local_vote_binding(yes, static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
            out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
        };
        registry.add(std::move(vote_bind));
    }
    // The switches a player can put on a button (action_binds): each bind runs its command.
    for (std::size_t i = 0; i < action_binds.size(); ++i) {
        const auto &slot = action_binds[i];
        auto entry = action("bind " + std::string(slot.name),
            "Bind a controller combo or keyboard key to: " + std::string(slot.label) + " (" + std::string(slot.command) + "); 0 clears the binding",
            Group::movement, {mask});
        entry.inspect = [](const Model &m) {
            return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
        };
        entry.run = [i](const Model &, const Values &args, const Output &out) {
            const bool saved = set_local_action_binding(i, static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
            out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
        };
        registry.add(std::move(entry));
    }
}
void register_ai_commands(Commands &registry) {
    const auto ready = [](const Model &m) {
        return State{m.debug.available, {}, "Waiting for local player controls.", {}, false};
    };
    auto count = argument("count", Type::unsigned_integer, true);
    count.minimum = 1;
    count.maximum = 8;
    auto physics = argument("physics", Type::boolean, true);
    auto brain = argument("brain", Type::boolean, true);
    auto spawn = action("ai spawn", "Experimental: spawn native AI skaters beside your skater; results are logged",
                        Group::gameplay, {count, physics, brain});
    spawn.aliases = {"ai.spawn"};
    spawn.inspect = ready;
    spawn.run = [](const Model &, const Values &args, const Output &out) {
        const auto n = args.empty() ? 1u : static_cast<unsigned>(std::get<std::uint64_t>(args[0]));
        const bool enabled = args.size() < 2 || std::get<bool>(args[1]);
        const bool thinking = args.size() < 3 || std::get<bool>(args[2]);
        ai_skaters::request_spawn(n, enabled, thinking);
        out(std::format("Spawning {} AI skater(s) with physics {} and the brain {}.", n,
                        enabled ? "enabled" : "disabled", thinking ? "running" : "paused"));
    };
    registry.add(std::move(spawn));
    auto toggle = action("ai brain", "Start (1) or pause (0) the brain tree of every AI skater", Group::gameplay,
                         {argument("0|1", Type::boolean)});
    toggle.aliases = {"ai.brain"};
    toggle.inspect = ready;
    toggle.run = [](const Model &, const Values &args, const Output &out) {
        const bool enabled = std::get<bool>(args[0]);
        ai_skaters::request_brain(enabled);
        out(enabled ? "Starting AI skater brains." : "Pausing AI skater brains.");
    };
    registry.add(std::move(toggle));
    auto clear = action("ai clear", "Remove every spawned AI skater", Group::gameplay);
    clear.aliases = {"ai.clear"};
    clear.inspect = ready;
    clear.run = [](const Model &, const Values &, const Output &out) {
        ai_skaters::request_clear();
        out("Removing AI skaters.");
    };
    registry.add(std::move(clear));
    auto status = action("ai status", "Log skater sources, the AI blueprint and each AI skater's state",
                         Group::gameplay);
    status.aliases = {"ai.status"};
    status.inspect = ready;
    status.run = [](const Model &, const Values &, const Output &out) {
        ai_skaters::request_report();
        out(std::format("{} AI skater(s) active; details follow in the log.", ai_skaters::active_count()));
    };
    registry.add(std::move(status));
}
} // namespace dingosdk::console
