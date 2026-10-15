#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "ai_skaters.h"
#include "drone_flight.h"
#include "drone_outfit.h"
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
    auto drone_mode = variable("dronemode",
        "Fly the free camera as an FPV drone. Easiest from the DRONE page of the ReSkate menu (Insert): Start flying",
        Group::movement, argument("0|1", Type::boolean));
    drone_mode.inspect = [](const Model &m) {
        return boolean_state(m.debug.available, drone::is_enabled(), "Waiting for local player controls.",
                             "Flies while freecam is on");
    };
    drone_mode.run = [](const Model &m, const Values &args, const Output &out) {
        const bool on = std::get<bool>(args[0]);
        drone::set_enabled(on);
        if (on) { drone::set_armed(true); drone::reset_flag().store(false); }
        if (!on) out("Drone mode off. Freecam flies normally again (freecam 0 takes you back to your skater).");
        else if (!m.debug.free_camera) out("Drone mode on. Turn on freecam (freecam 1) to take off.");
        else out("Drone mode on. W/S: tilt forward/back. A/D: roll left/right. E/Q: throttle up/down. Mouse: turn.");
    };
    drone_mode.reset = [](const Model &, const Output &) { drone::set_enabled(false); };
    registry.add(std::move(drone_mode));
    auto tune_name = argument("setting", Type::text, true);
    for (const auto &knob : drone::knob_info) tune_name.choices.emplace_back(knob.name);
    tune_name.choices.emplace_back("reset");
    auto tune_value = argument("value", Type::number, true);
    auto tune = action("dronetune",
        "Change a drone setting: dronetune <setting> <value>. On its own it lists every setting; 'dronetune reset' puts them all back",
        Group::movement, {tune_name, tune_value});
    tune.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    tune.run = [](const Model &, const Values &args, const Output &out) {
        const auto show = [&out](std::size_t i) {
            out(std::format("{:<9} {:<8g} {} (default {:g}, range {:g}-{:g})", drone::knob_info[i].name,
                            drone::knobs().value[i].load(), drone::knob_info[i].help, drone::knob_info[i].def,
                            drone::knob_info[i].lo, drone::knob_info[i].hi));
        };
        if (args.empty()) {
            for (std::size_t i = 0; i < drone::knob_count; ++i) show(i);
            return;
        }
        const auto name = std::get<std::string>(args[0]);
        if (name == "reset") {
            std::vector<std::pair<std::string, Json>> saved;
            for (std::size_t i = 0; i < drone::knob_count; ++i) {
                drone::knobs().value[i] = drone::knob_info[i].def;
                saved.emplace_back(std::format("Drone.Tune.{}", drone::knob_info[i].name),
                                   static_cast<double>(drone::knob_info[i].def));
            }
            profile_runtime::set_local_values(saved);
            out("Every drone setting is back to its default.");
            return;
        }
        const int index = drone::find_knob(name);
        if (index < 0) {
            out(std::format("error: There is no setting called '{}'. Type dronetune on its own to see them all.", name));
            return;
        }
        const auto i = static_cast<std::size_t>(index);
        if (args.size() < 2) {
            show(i);
            return;
        }
        const auto wanted = static_cast<float>(std::get<double>(args[1]));
        const auto value = std::clamp(wanted, drone::knob_info[i].lo, drone::knob_info[i].hi);
        drone::knobs().value[i] = value;
        profile_runtime::set_local_values({{std::format("Drone.Tune.{}", name), static_cast<double>(value)}});
        out(value == wanted ? std::format("{} = {:g}", name, value)
                            : std::format("{} = {:g} (the closest allowed value)", name, value));
    };
    registry.add(std::move(tune));
    auto menu_command = action("dronemenu", "Open the ReSkate menu on its DRONE page", Group::movement);
    menu_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    menu_command.run = [](const Model &, const Values &, const Output &out) {
        drone::menu_flag().store(true);
        out("Opening the DRONE page.");
    };
    registry.add(std::move(menu_command));
    auto arm_command = action("dronearm", "Arm the drone: motors on, ready to fly", Group::movement);
    arm_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    arm_command.run = [](const Model &, const Values &, const Output &out) {
        drone::set_armed(true);
        out("Armed.");
    };
    registry.add(std::move(arm_command));
    auto disarm_command = action("dronedisarm",
        "Disarm the drone: motors off and the sticks do nothing. It falls (or hangs in the air with dronetune disarmfreeze 1)", Group::movement);
    disarm_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    disarm_command.run = [](const Model &, const Values &, const Output &out) {
        drone::set_armed(false);
        out("Disarmed.");
    };
    registry.add(std::move(disarm_command));
    auto reset_command = action("dronereset",
        "Put the drone back where it took off, with a fresh battery and timer", Group::movement);
    reset_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    reset_command.run = [](const Model &, const Values &, const Output &out) {
        drone::request_reset();
        out("Back to the takeoff point.");
    };
    registry.add(std::move(reset_command));
    auto radio_mode = variable("droneradio",
        "Fly the drone with a controller: a gamepad, or an RC radio in USB Joystick mode",
        Group::movement, argument("0|1", Type::boolean));
    radio_mode.inspect = [](const Model &m) {
        return boolean_state(m.debug.available, drone::radio::config().enabled.load(), "Waiting for local player controls.",
                             "Set it up on the DRONE page (dronemenu), CONTROLLER tab");
    };
    radio_mode.run = [](const Model &, const Values &args, const Output &out) {
        const bool on = std::get<bool>(args[0]);
        drone::radio::config().enabled = on;
        profile_runtime::set_local_values({{"Drone.Radio", on ? 1.0 : 0.0}});
        out(on ? "Controller on. If a stick does the wrong thing, run Set up controller (dronemenu, CONTROLLER tab)."
               : "Controller off. The drone uses the keyboard and mouse.");
    };
    radio_mode.reset = [](const Model &, const Output &) {
        drone::radio::config().enabled = false;
        profile_runtime::set_local_values({{"Drone.Radio", 0.0}});
    };
    registry.add(std::move(radio_mode));
    auto radio_check = action("droneradiocheck", "Show what your controller is sending right now, and how the drone reads it", Group::movement);
    radio_check.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    radio_check.run = [](const Model &, const Values &, const Output &out) {
        const auto sample = drone::radio::poll();
        if (!sample.valid) {
            out("No controller found. Plug in a gamepad, or an RC radio with its USB mode set to Joystick, then try again.");
            return;
        }
        out(std::format("Controller: {}", sample.name));
        out(std::format("Raw axes 1-6: {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f}", sample.axes[0], sample.axes[1],
                        sample.axes[2], sample.axes[3], sample.axes[4], sample.axes[5]));
        std::string pressed;
        for (int n = 1; n <= 32; ++n)
            if (sample.buttons & (1u << (n - 1))) pressed += (pressed.empty() ? "" : " ") + std::to_string(n);
        out(std::format("Buttons pressed: {}", pressed.empty() ? "none" : pressed));
        const auto map = drone::radio::mapping_snapshot();
        const auto cmd = drone::read_sticks(sample.axes);
        out(std::format("Mapping (roll pitch throttle yaw): {} {} {} {}", map[0], map[1], map[2], map[3]));
        out(std::format("Drone reads: roll {:+.2f} (right +)  pitch {:+.2f} (forward +)  throttle {:.2f}  yaw {:+.2f} (right +)",
                        cmd.roll, cmd.pitch, cmd.throttle, cmd.yaw));
        out("A stick doing the wrong thing? Easiest fix: dronemenu, CONTROLLER tab, Set up controller.");
    };
    registry.add(std::move(radio_check));
    const auto axis_argument = [](const char *name) {
        auto a = argument(name, Type::number);
        a.minimum = -6;
        a.maximum = 6;
        return a;
    };
    // Finding the cosmetics the drone could put on the pilot (goggles while flying): see drone_outfit.h.
    auto items_command = action("droneitems",
        "List installed cosmetics whose name contains a word (droneitems goggle), or your skater's outfit slots (droneitems slots)",
        Group::movement, {argument("word", Type::text)});
    items_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    items_command.run = [](const Model &, const Values &args, const Output &out) {
        const auto word = std::get<std::string>(args[0]);
        try {
            if (word == "slots") {
                out("Your skater's outfit slots (number, slot, what it holds now):");
                for (const auto &line : drone::outfit::slots()) out(line);
                return;
            }
            std::size_t total{};
            const auto found = drone::outfit::find_items(word, 80, total);
            out(std::format("{} installed item(s) contain '{}'{}", total, word, total > found.size() ? " (the first 80):" : ":"));
            for (const auto &name : found) out("  " + name);
        } catch (const std::exception &error) {
            out(std::format("error: {}", error.what()));
        }
    };
    registry.add(std::move(items_command));
    auto radio_axes = action("droneaxes", "Choose which controller axis is roll, pitch, throttle and yaw (1-6 each; a minus reverses one). Set up controller does this for you",
        Group::movement, {axis_argument("roll"), axis_argument("pitch"), axis_argument("throttle"), axis_argument("yaw")});
    radio_axes.inspect = [](const Model &) {
        const auto map = drone::radio::mapping_snapshot();
        return State{true, std::format("{} {} {} {}", map[0], map[1], map[2], map[3]), {}, {}, false};
    };
    radio_axes.run = [](const Model &, const Values &args, const Output &out) {
        std::array<int, 4> next{};
        for (std::size_t i = 0; i < 4; ++i) {
            next[i] = static_cast<int>(std::lround(std::get<double>(args[i])));
            if (next[i] == 0 || std::abs(next[i]) > 6) {
                out("error: Each axis must be 1 to 6, or -1 to -6 to reverse it.");
                return;
            }
        }
        std::vector<std::pair<std::string, Json>> saved;
        for (std::size_t i = 0; i < 4; ++i) {
            drone::radio::config().map[i] = next[i];
            saved.emplace_back(std::format("Drone.Axis{}", i), static_cast<double>(next[i]));
        }
        profile_runtime::set_local_values(saved);
        out(std::format("Axes saved: roll {} pitch {} throttle {} yaw {}", next[0], next[1], next[2], next[3]));
    };
    registry.add(std::move(radio_axes));
    auto speed = argument("speed", Type::number);
    // Any speed in range, so the drone's presets and top speed slider can set theirs; the usual
    // ones are still offered as completions.
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
    auto boost_speed_argument = argument("speed", Type::number);
    boost_speed_argument.minimum = 1;
    boost_speed_argument.maximum = 300;
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
        request_debug(Debug::set_forward_velocity_speed, false, 20.0f);
    };
    registry.add(std::move(boost_speed));
    auto up_speed_argument = boost_speed_argument;
    up_speed_argument.maximum = 25;
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
        request_debug(Debug::set_up_velocity_speed, false, 20.0f);
    };
    registry.add(std::move(up_speed));
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
