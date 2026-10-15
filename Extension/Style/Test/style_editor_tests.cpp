#include "Extension/Console/commands.h"
#include "Extension/Style/style_commands.h"
#include "Extension/Style/style_camera.h"
#include "Extension/Style/style_editor.h"
#include "Extension/Style/style_internal.h"
#include "Extension/Style/style_layer.h"
#include "Extension/Style/style_layer_editor.h"
#include "Extension/Style/style_pose.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures{};
void check(bool ok, std::string_view what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
std::vector<std::string> calls;
bool screen{}, layer_available{true}, capturing{};
std::string call_log() {
    std::string text;
    for (const auto &call : calls) text += (text.empty() ? "" : "; ") + call;
    return text;
}
struct Case {
    std::string_view command;
    std::string_view call;
    std::string_view reply;
};
void camera_checks() {
    using namespace dingosdk::style;
    using namespace dingosdk::style_camera;
    const auto close = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    const std::array<float, 16> level{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto camera = level;
    check(orbit_camera(camera, 0, 0, 3, {1, 2, 3}, 0.5f) && camera[0] == 1 && camera[5] == 1 && camera[10] == 1 &&
              close(camera[12], 1) && close(camera[13], 2.5f) && close(camera[14], 6),
          "a level camera at yaw 0 is behind its target, raised by the height");
    camera = level;
    check(orbit_camera(camera, 0.7f, 0.3f, 2, {0, 0, 0}, 0), "a turned and tilted camera is placed");
    const auto row_dot = [&](std::size_t a, std::size_t b) {
        return camera[a * 4] * camera[b * 4] + camera[a * 4 + 1] * camera[b * 4 + 1] + camera[a * 4 + 2] * camera[b * 4 + 2];
    };
    check(close(row_dot(0, 0), 1) && close(row_dot(1, 1), 1) && close(row_dot(2, 2), 1) && close(row_dot(0, 1), 0) &&
              close(row_dot(0, 2), 0) && close(row_dot(1, 2), 0),
          "its rows are orthonormal");
    auto mirrored = level;
    mirrored[0] = -1;
    check(orbit_camera(mirrored, 0, 0, 3, {0, 0, 0}, 0) && mirrored[0] == -1 && mirrored[5] == 1, "a mirrored camera stays mirrored");
    check(!orbit_camera(camera, 0, pi / 2, 3, {0, 0, 0}, 0) && !orbit_camera(camera, 0, -pi / 2, 3, {0, 0, 0}, 0),
          "a camera straight up or down has no right axis");
    check(close(editor_yaw({0, 0, 0, 1}), -0.2092036f), "a skater with no turn gets the editor's angle from a quarter turn");
    OrbitState orbit;
    orbit.pitch.store(1.0f);
    orbit.distance.store(9.0f);
    orbit.height.store(2.0f);
    orbit.frame(0.25f);
    orbit.aim({1, 2, 3});
    auto framed = level;
    check(orbit.yaw.load() == 0.25f && orbit.pitch.load() == editor_pitch && orbit.distance.load() == editor_distance &&
              orbit.height.load() == editor_height && orbit.pose(framed) &&
              close(framed[13], 2 + editor_height + std::sin(editor_pitch) * editor_distance),
          "a new framing resets the tilt, the distance and the height, and the camera follows it");
}
void helper_checks() {
    using namespace dingosdk::style;
    const Pace measured{440, 290, 450};
    const std::vector<float> rotated{2.0f, 0.5f, 1.0f}, tied{1.0f, 1.0f};
    TrickSet clips;
    clips.set(3).set(32);
    for (const float time : {0.0f, 0.4f, 1.0f, 1.7f, 2.2f, 3.0f})
        check(std::abs(timeline_at(paced(time, measured), measured) - time) < 1e-4f, "timeline_at undoes paced");
    check(capturable(1) && capturable(30) && !capturable(31) && !capturable(32) && !capturable(0),
          "the quick ollie and nollie cannot be captured");
    check(first_trick(clips) == 3 && clips.count() == 2 && first_trick(TrickSet{}) == 0, "a trick set lists its tricks");
    check(with_stage_status("the stage did not open", "the stage closed: the map changed") ==
              "the stage did not open: the stage closed: the map changed",
          "a stage status follows what failed");
    check(with_stage_status("the stage closed", "the stage closed: the map changed") == "the stage closed: the map changed" &&
              with_stage_status("the stage closed", stage_closed_text) == "the stage closed",
          "a status that starts with what failed stands alone, and the closed status adds nothing");
    check(std::abs(dingosdk::style::distance({0, 0, 0}, {3, 4, 12}) - 13) < 1e-5f &&
              std::abs(ground_distance({0, 9, 0}, {3, -9, 4}) - 5) < 1e-5f,
          "distances in space and on the ground");
    const std::vector<float> marks{0.5f, 3.0f, 2.0f};
    check(!last_key(marks, 0) && last_key(marks, 1) && !last_key(marks, 2), "the last keyframe is the latest in time");
    check(next_key_time(marks, 0.5f) == 2.0f && next_key_time(marks, 2.0f) == trick_end, "the next keyframe is the nearest later one");
    const std::vector<float> close_keys{1.01f, 1.0f};
    check(previous_key_time(close_keys, 1.01f) == 1.0f && next_key_time(close_keys, 1.0f) == 1.01f &&
              previous_key_time(close_keys, 1.0f) == 0,
          "two keyframes 0.01 s apart step to each other both ways");
    check(selection_after_delete(rotated, 0) == 1u && selection_after_delete(rotated, 2) == 1u &&
              selection_after_delete(rotated, 1) == 1u && selection_after_delete(tied, 1) == 0u && selection_after_delete(tied, 0) == 0u &&
              !selection_after_delete(std::vector<float>{1.0f}, 0),
          "a delete selects the keyframe before it in time, else the earliest one left");
    check(key_at_time(rotated, 1.0f) == 2u && key_at_time(rotated, 0.5004f) == 1u && !key_at_time(rotated, 1.5f) &&
              !key_at_time(std::vector<float>{}, 1.0f),
          "the keyframe at a time, else none");
    camera_checks();
}
void guarded_tick_checks() {
    using namespace dingosdk::style;
    int stops{};
    constexpr auto channel = dingosdk::logging::Channel::skater;
    guarded_tick(channel, "Test machine", [] { throw std::runtime_error("broken"); }, [&] { return ++stops > 0; });
    check(stops == 1, "a tick that throws ends the machine");
    guarded_tick(channel, "Test machine", [] {}, [&] { return ++stops > 0; });
    check(stops == 1, "a tick that does not throw leaves it running");
    // A stop that throws is contained: this call returns.
    guarded_tick(channel, "Test machine", [] { throw 1; }, []() -> bool { throw std::runtime_error("the stop failed"); });
}
} // namespace

namespace dingosdk::style_layer {
void request_enabled(bool enabled) { calls.push_back(enabled ? "enabled 1" : "enabled 0"); }
bool available() noexcept { return layer_available; }
} // namespace dingosdk::style_layer
namespace dingosdk::style_capture {
bool active() noexcept { return capturing; }
} // namespace dingosdk::style_capture
namespace dingosdk::style_editor {
void request_show(std::uint8_t trick) { calls.push_back("show " + std::to_string(trick)); }
void request_record_tricks() { calls.push_back("record"); }
void request_capture(std::uint8_t trick) { calls.push_back("capture " + std::to_string(trick)); }
void open_editor() { calls.push_back("open"); }
bool screen_open() noexcept { return screen; }
bool close_editor() {
    calls.push_back("close");
    return true;
}
void request_forget(std::uint8_t trick) { calls.push_back("forget " + std::to_string(trick)); }
void request_hide() { calls.push_back("hide"); }
void close_and_hide() {
    calls.push_back("close");
    calls.push_back("hide");
}
void request_hold(float time) noexcept { calls.push_back(std::format("hold {}", time)); }
void request_play(bool play) noexcept { calls.push_back(play ? "play" : "pause"); }
void request_step(int frames) noexcept { calls.push_back(std::format("step {}", frames)); }
void request_speed(float speed) noexcept { calls.push_back(std::format("speed {}", speed)); }
void request_orbit(float yaw, float pitch, float distance, float height) noexcept {
    calls.push_back(std::format("orbit {} {} {} {}", yaw, pitch, distance, height));
}
} // namespace dingosdk::style_editor

namespace layer = dingosdk::style_layer;
static_assert(layer::riding_still(100) && !layer::riding_still(101));

int main() {
    using namespace dingosdk::console;
    try {
        helper_checks();
        guarded_tick_checks();
        Commands registry;
        register_style_editor_commands(registry);
        Model model;
        std::string printed;
        const Output out{[&](const std::string &line) { printed += line; }, {}, {}};
        const auto run = [&](std::string_view text) {
            printed.clear();
            calls.clear();
            return registry.execute(text, model, out);
        };
        const Case cases[]{
            {"style editor show", "show 0", ""},
            {"style editor show heelflip", "show 3", ""},
            {"style editor forget Kickflip", "forget 2", "Forgetting it."},
            {"style editor forget all", "forget 0", "Forgetting it."},
            {"style editor forget moonflip", "", "error: That is not a flip trick."},
            {"style editor capture heelflip", "enabled 1; capture 3", ""},
            {"style editor record", "enabled 1; record", "Recording your tricks."},
            {"style editor capture olliepop", "", "error: The quick ollie and the quick nollie cannot be captured."},
            {"style editor capture", "", "error: Give a flip trick."},
            {"style editor hold 1.5", "hold 1.5", ""},
            {"style editor play", "play", ""},
            {"style editor pause", "pause", ""},
            {"style editor step -3", "step -3", ""},
            {"style editor speed 0.25", "speed 0.25", ""},
            {"style editor orbit 0.5 -0.25 1", "orbit 0.5 -0.25 1 0", ""},
            {"style editor close", "close; hide", ""},
        };
        for (const auto &c : cases) check(run(c.command) && call_log() == c.call && printed == c.reply, c.command);
        // The registry refuses these before a stub runs: out of range or not finite.
        for (const std::string_view command : {"style editor speed 2", "style editor speed 0", "style editor hold 3.5",
                                               "style editor hold nan", "style editor orbit 25 0 0", "style editor orbit inf 0 0"})
            check(!run(command) && calls.empty(), command);
        {
            // On a multiplayer server the editor works while the stand-in's slot is free.
            namespace editor = dingosdk::style_editor;
            check(editor::stand_in_slot_free(0) && editor::stand_in_slot_free(editor::stand_in_slot - editor::stand_in_margin - 1) &&
                      !editor::stand_in_slot_free(editor::stand_in_slot - editor::stand_in_margin) &&
                      !editor::stand_in_slot_free(editor::stand_in_slot + 1),
                  "the stand-in's slot is free only below the margin");
            model.multiplayer.active = model.style.enabled = true;
            dingosdk::set_multiplayer_session_active(true);
            dingosdk::set_multiplayer_slots_in_use(editor::stand_in_slot - editor::stand_in_margin - 1);
            check(run("style editor show heelflip") && call_log() == "show 3" && printed.empty(), "a server with room shows a clip");
            check(run("style editor open") && call_log() == "open" && printed == "Opening the style editor.", "and opens the editor");
            dingosdk::set_multiplayer_slots_in_use(editor::stand_in_slot - editor::stand_in_margin);
            const std::string full = "error: The style editor is not available on a server this full.";
            for (const std::string_view command : {"style editor open", "style editor show heelflip"})
                check(run(command) && printed == full && calls.empty(), std::string(command) + " is refused on a server this full");
            check(run("style editor show") && call_log() == "show 0" && printed.empty(), "the stand-in can be cleared on a full server");
            model.multiplayer.active = model.style.enabled = false;
            dingosdk::set_multiplayer_session_active(false);
            dingosdk::set_multiplayer_slots_in_use(0);
        }
        {
            dingosdk::set_multiplayer_session_active(true);
            const std::string solo = "error: Record your tricks in solo play.";
            for (const std::string_view command : {"style editor record", "style editor capture heelflip"})
                check(run(command) && printed == solo && calls.empty(), std::string(command) + " is refused on a multiplayer server");
            dingosdk::set_multiplayer_session_active(false);
            capturing = true;
            check(run("style editor open") && printed == "error: A capture or a recording is running." && calls.empty(),
                  "open is refused while a recording runs");
            capturing = false;
        }
        model.style.enabled = true;
        check(run("style editor record") && call_log() == "record" && printed == "Recording your tricks.",
              "record with the layer on asks for no switch");
        check(run("style editor capture heelflip") && call_log() == "capture 3", "and so does capture");
        model.style.enabled = false;
        layer_available = false;
        for (const std::string_view command : {"style editor record", "style editor capture heelflip"})
            check(run(command) && printed == "error: Style is unavailable for this game build. 'style status' says why." &&
                      calls.empty(),
                  std::string(command) + " is refused while style is unavailable");
        check(run("style editor open") && printed == "error: Style is unavailable for this game build. 'style status' says why." &&
                  calls.empty(),
              "the editor does not open while style is unavailable");
        layer_available = true;
        model.style.enabled = false;
        check(run("style editor open") && call_log() == "enabled 1; open" && printed == "Opening the style editor.",
              "style editor open switches the layer on and opens the editor");
        check(style_editor_status() == " Editor closed.", "style status says that the editor is closed");
        screen = true;
        check(style_editor_status() == " Editor open.", "and that it is open");
        screen = false;
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        ++failures;
    }
    if (!failures) std::cout << "style editor tests passed\n";
    return failures ? 1 : 0;
}
