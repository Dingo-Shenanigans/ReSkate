#include "skate_menu_internal.h"
#include "overlay_internal.h"
#include "Extension/Skater/drone_flight.h"
#include "Extension/Skater/drone_beacon.h"
#include "Extension/Skater/drone_outfit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

// The DRONE page of the ReSkate menu: flying, handling, rates, the display, the controller and
// multiplayer. Every change takes effect at once and is saved by running the same `dronetune`
// command a player could type, so the console and this page always agree.

namespace dingosdk::overlay::menu {
namespace {
namespace drone = dingosdk::drone;
using drone::Knob;

// Runs a console command on the game's thread, quietly: a slider's save is not news.
void run(const CallbacksV3& callbacks, const std::string& command) {
    if (!callbacks.queue_console_command) return;
    std::array<char, 256> result{};
    callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
}

float get(Knob k) { return drone::knobs().value[k].load(std::memory_order_relaxed); }
void set(Knob k, float v) { drone::knobs().value[k].store(v, std::memory_order_relaxed); }
// Saves a setting by running `dronetune`, which also clamps it and keeps it for next time.
void save(const CallbacksV3& callbacks, Knob k) { run(callbacks, std::format("dronetune {} {}", drone::knob_info[k].name, get(k))); }
std::string tip(Knob k) {
    return std::format("{}.\n\nConsole: dronetune {} (default {:g})", drone::knob_info[k].help, drone::knob_info[k].name,
                       drone::knob_info[k].def);
}

void slider(SkateMenu& menu, const CallbacksV3& callbacks, Knob k, const char* label, const char* format = "%.2f") {
    const auto help = tip(k);
    field(menu, label, help.c_str());
    float v = get(k);
    ImGui::PushID(static_cast<int>(k));
    if (ImGui::SliderFloat("##value", &v, drone::knob_info[k].lo, drone::knob_info[k].hi, format)) set(k, v);
    if (ImGui::IsItemDeactivatedAfterEdit()) save(callbacks, k);
    ImGui::PopID();
}
void integer(SkateMenu& menu, const CallbacksV3& callbacks, Knob k, const char* label) {
    const auto help = tip(k);
    field(menu, label, help.c_str());
    int v = static_cast<int>(std::lround(get(k)));
    ImGui::PushID(static_cast<int>(k));
    if (ImGui::SliderInt("##value", &v, static_cast<int>(drone::knob_info[k].lo), static_cast<int>(drone::knob_info[k].hi)))
        set(k, static_cast<float>(v));
    if (ImGui::IsItemDeactivatedAfterEdit()) save(callbacks, k);
    ImGui::PopID();
}
void toggle(SkateMenu& menu, const CallbacksV3& callbacks, Knob k, const char* label) {
    const auto help = tip(k);
    bool on = get(k) >= 0.5f;
    if (toggle_row(menu, label, help.c_str(), on)) { set(k, on ? 1.0f : 0.0f); save(callbacks, k); }
}
void options(SkateMenu& menu, const CallbacksV3& callbacks, Knob k, const char* label, std::initializer_list<const char*> names) {
    const auto help = tip(k);
    field(menu, label, help.c_str());
    int v = std::clamp(static_cast<int>(std::lround(get(k))), 0, static_cast<int>(names.size()) - 1);
    if (choice(menu, drone::knob_info[k].name, v, names)) { set(k, static_cast<float>(v)); save(callbacks, k); }
}

// ---- FLY
void fly_tab(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const bool enabled = drone::is_enabled();
    const bool radio_on = drone::radio::config().enabled.load();
    const bool freecam = model.debug.free_camera;
    const bool flying = enabled && freecam;

    begin_card(menu, "drone-fly", "FLY");
    // The game's interface is hidden for the flight (the `hideui` drone setting) and only
    // brought back if the drone was what hid it.
    static bool hid_ui = false;
    if (primary_button(menu, flying ? "STOP FLYING" : "START FLYING", model.debug.camera_available)) {
        if (flying) {
            run(callbacks, "dronemode 0");
            run(callbacks, "freecam 0"); // back to the skater
            if (hid_ui) run(callbacks, "hideui 0");
            hid_ui = false;
        } else {
            if (!freecam) run(callbacks, "freecam 1");
            if (!enabled) run(callbacks, "dronemode 1");
            if (get(Knob::kHideUi) >= 0.5f && !model.debug.game_ui_hidden) { run(callbacks, "hideui 1"); hid_ui = true; }
        }
    }
    if (!model.debug.camera_available) warn("Load into a level to fly.");
    else if (flying) note(radio_on ? "Flying with the controller. Close the menu to fly." : "Flying with the keyboard and mouse. Close the menu to fly.");
    else if (enabled) note("Drone mode is on, but freecam is off: press Start flying.");
    else note("Keyboard: W/S tilt forward/back, A/D roll, E/Q throttle up/down, mouse turns, Shift boosts.");
    bool radio = radio_on;
    if (toggle_row(menu, "Fly with a controller", "An RC radio in USB Joystick mode, or a gamepad. Set it up on the CONTROLLER tab.", radio))
        run(callbacks, radio ? "droneradio 1" : "droneradio 0");
    if (!radio_on && drone::radio::poll().valid)
        warn("A controller is connected. Turn on Fly with a controller to use it, or the game keeps using it for your skater.");
    toggle(menu, callbacks, Knob::kHideUi, "Hide the game's interface while flying");
    end_card();

    begin_card(menu, "drone-quad", "DRONE");
    {   // Presets (drone_flight.h): a whole drone in one choice. "Custom" once anything is changed.
        const int current = drone::matching_preset(model.debug.camera_speed);
        field(menu, "Preset", "A whole drone in one choice: handling, rates, size, camera tilt, motor sound and top speed.");
        if (ImGui::BeginCombo("##preset", current >= 0 ? drone::presets[current].name : "Custom")) {
            for (std::size_t p = 0; p < drone::preset_count; ++p) {
                const auto& preset = drone::presets[p];
                if (ImGui::Selectable(preset.name, static_cast<int>(p) == current)) {
                    for (const auto& v : preset.values) { set(v.knob, v.value); save(callbacks, v.knob); }
                    run(callbacks, std::format("flyspeed {}", preset.top_speed));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", preset.description);
            }
            ImGui::EndCombo();
        }
        note(current >= 0 ? drone::presets[current].description : "Your own settings. Pick a preset to start from one of them.");
    }
    {   // What the pilot's skater wears over their eyes while flying.
        const int worn = std::clamp(static_cast<int>(std::lround(get(Knob::kGoggles))), 0, static_cast<int>(drone::outfit::eyewear_count));
        field(menu, "Goggles", "Your skater wears these while you fly. Your own eyewear comes back when you land.");
        if (ImGui::BeginCombo("##goggles", worn ? drone::outfit::eyewear[worn - 1].label : "None")) {
            if (ImGui::Selectable("None", worn == 0)) { set(Knob::kGoggles, 0.0f); save(callbacks, Knob::kGoggles); }
            for (std::size_t i = 0; i < drone::outfit::eyewear_count; ++i)
                if (ImGui::Selectable(drone::outfit::eyewear[i].label, worn == static_cast<int>(i) + 1)) {
                    set(Knob::kGoggles, static_cast<float>(i + 1));
                    save(callbacks, Knob::kGoggles);
                }
            ImGui::EndCombo();
        }
    }
    {   // The game's value comes back every frame; hold the one being set until the game has it,
        // or the slider would snap back while it is dragged.
        static float pending = -1.0f;
        static double sent_at = 0.0;
        if (pending >= 0.0f && (std::abs(model.debug.camera_speed - pending) < 0.01f ||
                                (sent_at > 0.0 && ImGui::GetTime() - sent_at > 1.0))) { pending = -1.0f; sent_at = 0.0; }
        float top = pending >= 0.0f ? pending : model.debug.camera_speed;
        field(menu, "Top speed", "The same setting as freecam's flight speed.\n\nConsole: flyspeed");
        if (ImGui::SliderFloat("##top-speed", &top, 0.6f, 300.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) pending = top;
        if (ImGui::IsItemDeactivatedAfterEdit()) { run(callbacks, std::format("flyspeed {}", top)); sent_at = ImGui::GetTime(); }
        // The flight model caps speed at four times the fly speed (m/s), twice that with boost.
        const auto speed = std::format("Up to about {:.0f} km/h (twice that with boost).", top * 4.0f * 3.6f);
        note(speed.c_str());
    }
    end_card();

    begin_card(menu, "drone-motors", "MOTORS");
    const bool armed = drone::armed();
    info(menu, "Motors", armed ? "ARMED" : "DISARMED");
    if (drone::flying()) {
        const auto& t = drone::telemetry();
        info(menu, "Flight", std::format("{:.0f}% throttle, {:.0f} km/h, {:.1f} m up, {:.0f} s", t.throttle.load() * 100.0f,
                                         t.speed.load() * 3.6f, t.altitude.load(), t.seconds.load()));
    }
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button(armed ? "Disarm" : "Arm", ImVec2(third, 0))) drone::set_armed(!armed);
    ImGui::SameLine();
    if (ImGui::Button("Back to takeoff", ImVec2(third, 0))) drone::request_reset();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the drone back where it took off, with a fresh battery and timer.");
    ImGui::SameLine();
    if (ImGui::Button("Reset settings", ImVec2(third, 0))) run(callbacks, "dronetune reset");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts every drone setting back to its default.");
    end_card();

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 170, 170, 255));
    ImGui::TextUnformatted("Created by neekgate for ReSkate");
    ImGui::PopStyleColor();
}

// ---- HANDLING
void handling_tab(SkateMenu& menu, const CallbacksV3& callbacks) {
    begin_card(menu, "drone-flight", "FLIGHT");
    slider(menu, callbacks, Knob::kGravity, "Gravity", "%.1f");
    slider(menu, callbacks, Knob::kHover, "Hover throttle");
    slider(menu, callbacks, Knob::kDrag, "Air drag");
    slider(menu, callbacks, Knob::kUptilt, "Camera tilt", "%.0f degrees");
    end_card();

    begin_card(menu, "drone-collisions", "COLLISIONS");
    toggle(menu, callbacks, Knob::kCollide, "Collide with the world");
    slider(menu, callbacks, Knob::kRadius, "Collision size", "%.2f m");
    slider(menu, callbacks, Knob::kFloor, "Invisible floor", "%.1f m");
    toggle(menu, callbacks, Knob::kDisarmFreeze, "Hang in the air when disarmed");
    end_card();

    begin_card(menu, "drone-range", "RANGE");
    bool limited = get(Knob::kRange) > 0.0f;
    if (toggle_row(menu, "Limited range",
            "Like a real radio and video link: the picture breaks up as you near the limit, and further out the motors cut "
            "(failsafe) until you reset. Off: fly anywhere on the map.", limited)) {
        set(Knob::kRange, limited ? 300.0f : 0.0f);
        save(callbacks, Knob::kRange);
    }
    if (limited) {
        field(menu, "Range");
        float metres = get(Knob::kRange);
        if (ImGui::SliderFloat("##range", &metres, 50.0f, 5000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic)) set(Knob::kRange, metres);
        if (ImGui::IsItemDeactivatedAfterEdit()) save(callbacks, Knob::kRange);
        const auto explained = std::format(
            "Measured from where you took off. The picture starts to break up at {:.0f} m and is lost at {:.0f} m. Stay without a "
            "picture for {:.0f} seconds, or go past {:.0f} m, and the motors cut.",
            get(Knob::kRange) * drone::range_clean, get(Knob::kRange), drone::failsafe_after_seconds,
            get(Knob::kRange) * drone::range_failsafe);
        note(explained.c_str());
    } else {
        note("Unlimited: fly anywhere on the map.");
    }
    end_card();
}

// ---- RATES
void rates_card(SkateMenu& menu, const CallbacksV3& callbacks, const char* id, const char* title, Knob centre, Knob max, Knob expo) {
    begin_card(menu, id, title);
    slider(menu, callbacks, centre, "Centre sensitivity", "%.0f deg/s");
    slider(menu, callbacks, max, "Max rate", "%.0f deg/s");
    slider(menu, callbacks, expo, "Expo");
    end_card();
}
void rates_tab(SkateMenu& menu, const CallbacksV3& callbacks) {
    note("How fast the drone turns, set like Betaflight's \"Actual\" rates. Centre: the speed near the middle of the stick. "
         "Max: the speed at full stick. Expo: how much softer the middle is.");
    rates_card(menu, callbacks, "drone-roll", "ROLL", Knob::kRollCenter, Knob::kRollMax, Knob::kRollExpo);
    rates_card(menu, callbacks, "drone-pitch", "PITCH", Knob::kPitchCenter, Knob::kPitchMax, Knob::kPitchExpo);
    rates_card(menu, callbacks, "drone-yaw", "YAW", Knob::kYawCenter, Knob::kYawMax, Knob::kYawExpo);
}

// ---- DISPLAY
void display_tab(SkateMenu& menu, const CallbacksV3& callbacks) {
    begin_card(menu, "drone-osd", "GOGGLE DISPLAY", "the OSD");
    toggle(menu, callbacks, Knob::kOsd, "Show the goggle display");
    slider(menu, callbacks, Knob::kOsdScale, "Text size");
    options(menu, callbacks, Knob::kOsdColor, "Colour", {"White", "Green", "Yellow", "Cyan", "Orange", "Pink"});
    options(menu, callbacks, Knob::kOsdFont, "Font", {"Pixel", "Smooth"});
    slider(menu, callbacks, Knob::kOsdMargin, "Distance from the edge");
    toggle(menu, callbacks, Knob::kCrosshair, "Crosshair");
    end_card();

    begin_card(menu, "drone-osd-show", "SHOW");
    toggle(menu, callbacks, Knob::kShowArmed, "Armed / disarmed");
    toggle(menu, callbacks, Knob::kShowBattery, "Battery");
    toggle(menu, callbacks, Knob::kShowTimer, "Timer");
    toggle(menu, callbacks, Knob::kShowThrottle, "Throttle");
    toggle(menu, callbacks, Knob::kShowAlt, "Altitude");
    toggle(menu, callbacks, Knob::kShowSpeed, "Speed");
    toggle(menu, callbacks, Knob::kSticks, "My sticks on screen");
    slider(menu, callbacks, Knob::kStickSize, "Sticks size");
    end_card();

    begin_card(menu, "drone-analog", "ANALOGUE VIDEO");
    toggle(menu, callbacks, Knob::kAnalog, "Analogue video look");
    slider(menu, callbacks, Knob::kAnalogNoise, "Static");
    slider(menu, callbacks, Knob::kAnalogScan, "Scanlines");
    slider(menu, callbacks, Knob::kAnalogVignette, "Dark corners");
    slider(menu, callbacks, Knob::kAnalogGlitch, "Rolling glitches");
    end_card();

    begin_card(menu, "drone-sound", "SOUND");
    slider(menu, callbacks, Knob::kVolume, "Motor volume");
    slider(menu, callbacks, Knob::kMotorPitch, "Motor pitch");
    end_card();
}

// ---- CONTROLLER
const char* axis_text(int signed_axis, char* buffer, std::size_t size) {
    if (signed_axis == 0) std::snprintf(buffer, size, "none");
    else std::snprintf(buffer, size, "axis %d%s", std::abs(signed_axis), signed_axis < 0 ? " (reversed)" : "");
    return buffer;
}
// Live bars for the six axes; `highlight` (0-5) is drawn as the one being picked.
void axis_bars(const drone::radio::Sample& sample, int highlight) {
    for (int i = 0; i < 6; ++i) {
        const auto v = sample.axes[static_cast<std::size_t>(i)];
        char value[16];
        std::snprintf(value, sizeof value, "%+.2f", static_cast<double>(v));
        const bool lit = i == highlight;
        if (lit) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(120, 220, 120, 255));
        ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(ImGui::GetContentRegionAvail().x * 0.75f, 0), value);
        if (lit) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("Axis %d%s", i + 1, lit ? "  <" : "");
    }
}
// The guided setup (drone_radio.h: Calibration). Saves through the same commands as the manual controls.
void setup_wizard(SkateMenu& menu, const CallbacksV3& callbacks, const drone::radio::Sample& sample) {
    using Step = drone::radio::Calibration::Step;
    auto& c = drone::radio::calibration();
    if (sample.valid) c.update(sample.axes, sample.buttons, ImGui::GetIO().DeltaTime);
    const auto step = c.step();
    if (c.active()) drone::radio::mark_calibrating();
    const char* title = "";
    const char* body = "";
    switch (step) {
    case Step::rest:
        title = "STEP 1 OF 7: STICKS AT REST";
        body = "Let go of both sticks so they centre, and put the throttle all the way down (a gamepad's sticks just centre). "
               "Then press Ready.";
        break;
    case Step::throttle:
        title = "STEP 2 OF 7: THROTTLE";
        body = c.waiting_for_return() ? "Got it. Now let the throttle back down."
                                      : "Push the throttle all the way up and hold it there. On a gamepad, that's the left stick, up.";
        break;
    case Step::yaw:
        title = "STEP 3 OF 7: YAW";
        body = c.waiting_for_return() ? "Got it. Let the stick go." : "Push the yaw stick (left stick, sideways) fully right and hold it.";
        break;
    case Step::pitch:
        title = "STEP 4 OF 7: PITCH";
        body = c.waiting_for_return() ? "Got it. Let the stick go." : "Push the right stick fully forward (away from you) and hold it.";
        break;
    case Step::roll:
        title = "STEP 5 OF 7: ROLL";
        body = c.waiting_for_return() ? "Got it. Let the stick go." : "Push the right stick fully right and hold it.";
        break;
    case Step::arm:
        title = "STEP 6 OF 7: ARM SWITCH (OPTIONAL)";
        body = "Flip the switch you want to arm the motors with to its ARMED position. No arm switch? Press Skip.";
        break;
    case Step::reset:
        title = "STEP 7 OF 7: RESET SWITCH (OPTIONAL)";
        body = c.waiting_for_return() ? "Got it. Let the switch go back."
                                      : "Flick the switch or press the button you want to put the drone back at its start. No reset switch? Press Skip.";
        break;
    default: break;
    }
    if (step == Step::done) {
        // Save once, then show what was found.
        static bool saved = false;
        if (!saved) {
            const auto m = c.mapping();
            run(callbacks, std::format("droneaxes {} {} {} {}", m[0], m[1], m[2], m[3]));
            set(Knob::kArmAxis, static_cast<float>(c.arm_axis())); save(callbacks, Knob::kArmAxis);
            set(Knob::kArmButton, static_cast<float>(c.arm_button())); save(callbacks, Knob::kArmButton);
            set(Knob::kResetAxis, static_cast<float>(c.reset_axis())); save(callbacks, Knob::kResetAxis);
            set(Knob::kResetButton, static_cast<float>(c.reset_button())); save(callbacks, Knob::kResetButton);
            set(Knob::kThrottleCentre, c.throttle_centred() ? 1.0f : 0.0f); save(callbacks, Knob::kThrottleCentre);
            // Gamepad sticks drift more than a radio's gimbals.
            if (c.throttle_centred() && get(Knob::kDeadzone) < 0.12f) { set(Knob::kDeadzone, 0.12f); save(callbacks, Knob::kDeadzone); }
            // Setting a controller up means flying with it.
            if (!drone::radio::config().enabled.load()) run(callbacks, "droneradio 1");
            saved = true;
        }
        begin_card(menu, "drone-setup-done", "SETUP SAVED", "the drone now flies with this controller");
        const auto m = c.mapping();
        static const char* const names[4] = {"Roll", "Pitch", "Throttle", "Yaw"};
        char buffer[32];
        for (int i = 0; i < 4; ++i) info(menu, names[i], axis_text(m[static_cast<std::size_t>(i)], buffer, sizeof buffer));
        const auto switch_text = [&buffer](int axis, int button) -> std::string {
            if (button > 0) return std::format("button {}", button);
            return axis_text(axis, buffer, sizeof buffer);
        };
        info(menu, "Arm switch", switch_text(c.arm_axis(), c.arm_button()));
        info(menu, "Reset switch", switch_text(c.reset_axis(), c.reset_button()));
        if (c.throttle_centred())
            note("Your throttle springs back to the middle, so the middle now holds the drone up: push up to climb, down to descend.");
        if (primary_button(menu, "DONE")) { c.cancel(); saved = false; }
        end_card();
        return;
    }
    begin_card(menu, "drone-setup", title);
    note(body);
    if (!sample.valid) warn("The controller is not connected.");
    else axis_bars(sample, c.waiting_for_return() ? -1 : c.candidate());
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (step == Step::rest) {
        ImGui::BeginDisabled(!sample.valid);
        if (ImGui::Button("Ready", ImVec2(third, 0))) c.capture_rest(sample.axes, sample.buttons);
        ImGui::EndDisabled();
        ImGui::SameLine();
    } else if ((step == Step::arm || step == Step::reset) && !c.waiting_for_return()) {
        if (ImGui::Button("Skip", ImVec2(third, 0))) c.skip();
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel", ImVec2(third, 0))) c.cancel();
    end_card();
}
void controller_tab(SkateMenu& menu, const CallbacksV3& callbacks) {
    const auto sample = drone::radio::poll();
    auto& c = drone::radio::calibration();
    if (c.step() != drone::radio::Calibration::Step::idle) {
        setup_wizard(menu, callbacks, sample);
        return;
    }
    begin_card(menu, "drone-controller", "CONTROLLER");
    if (!sample.valid) {
        warn("No controller found. Plug in a gamepad, or an RC radio with its USB mode set to Joystick (on EdgeTX radios, pick "
             "\"USB Joystick (HID)\" when you plug it in).");
    } else {
        info(menu, "Connected", sample.name);
        if (primary_button(menu, "SET UP CONTROLLER")) c.start();
        note("Goes through each stick and switch and saves which is which. The drone holds still meanwhile.");
    }
    end_card();

    begin_card(menu, "drone-manual", "MANUAL SETUP");
    if (sample.valid) {
        axis_bars(sample, -1);
        std::string pressed;
        for (int n = 1; n <= 32; ++n)
            if (sample.buttons & (1u << (n - 1))) pressed += (pressed.empty() ? "" : " ") + std::to_string(n);
        info(menu, "Buttons pressed", pressed.empty() ? "none" : pressed);
        note("Move a stick or flip a switch to see which axis or button it is.");
    }
    static const char* const names[4] = {"Roll", "Pitch", "Throttle", "Yaw"};
    static const char* const axes[] = {"Axis 1", "Axis 2", "Axis 3", "Axis 4", "Axis 5", "Axis 6"};
    auto map = drone::radio::mapping_snapshot();
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        int axis = std::clamp(std::abs(map[static_cast<std::size_t>(i)]), 1, 6) - 1;
        bool flip = map[static_cast<std::size_t>(i)] < 0;
        field(menu, names[i]);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::Combo("##axis", &axis, axes, 6)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Reverse", &flip)) changed = true;
        map[static_cast<std::size_t>(i)] = (axis + 1) * (flip ? -1 : 1);
        ImGui::PopID();
    }
    if (changed) run(callbacks, std::format("droneaxes {} {} {} {}", map[0], map[1], map[2], map[3]));
    toggle(menu, callbacks, Knob::kThrottleCentre, "Throttle springs back to the middle");
    slider(menu, callbacks, Knob::kDeadzone, "Stick dead zone");
    end_card();

    begin_card(menu, "drone-switches", "SWITCHES", "an axis or button no stick uses; 0 = none");
    integer(menu, callbacks, Knob::kArmAxis, "Arm switch axis");
    integer(menu, callbacks, Knob::kArmButton, "Arm button");
    integer(menu, callbacks, Knob::kResetAxis, "Reset switch axis");
    integer(menu, callbacks, Knob::kResetButton, "Reset button");
    end_card();
}

// ---- LOBBY
void lobby_tab(SkateMenu& menu, const CallbacksV3& callbacks) {
    begin_card(menu, "drone-lobby", "OTHER PLAYERS");
    if (!drone::beacon::server_allows().load()) {
        warn("This server has drones turned off. You can still fly, but nobody here sees your drone and you don't see theirs.");
    }
    toggle(menu, callbacks, Knob::kBeacon, "Let other players see my drone");
    toggle(menu, callbacks, Knob::kShowBeacons, "Show other players' drones");
    note("Other players only see drones if they are on a version of ReSkate with the drone.");
    end_card();
}
} // namespace

void drone_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.drone_tab, {"FLY", "HANDLING", "RATES", "DISPLAY", "CONTROLLER", "LOBBY"}, "drone-tabs");
    ImGui::PushID(menu.drone_tab);
    ImGui::BeginChild("drone-tab", ImVec2(0, page_body_height(menu)));
    switch (menu.drone_tab) {
    case 0: fly_tab(menu, model, callbacks); break;
    case 1: handling_tab(menu, callbacks); break;
    case 2: rates_tab(menu, callbacks); break;
    case 3: display_tab(menu, callbacks); break;
    case 4: controller_tab(menu, callbacks); break;
    default: lobby_tab(menu, callbacks); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}
} // namespace dingosdk::overlay::menu
