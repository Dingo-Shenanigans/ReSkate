#include "Extension/Console/commands.h"
#include "style_capture.h"
#include "style_commands.h"
#include "style_editor.h"
#include "style_layer.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace dingosdk::console {
namespace {
bool in_solo() { return !multiplayer_session_active(); }
std::string error(std::string_view reason) { return "error: " + style::sentence(reason); }
bool server_too_full() { return !style_editor::stand_in_slot_free(); }
// The layer must be on for a recording: the command that asks for it switches it on.
bool layer_ready(const Model &m) {
    if (!style_layer::available()) return false;
    if (!m.style.enabled) style_layer::request_enabled(true);
    return true;
}

void add_clip_commands(Commands &registry) {
    auto show = action("style editor show", "Play a flip trick's clip on the stand-in. With no trick, remove the shown clip",
                       Group::gameplay, {trick_argument(true)});
    show.run = [](const Model &, const Values &args, const Output &out) {
        const auto trick = trick_at(args, 0);
        if (trick && server_too_full()) return out(error(style::full_server_text));
        style_editor::request_show(trick);
    };
    registry.add(std::move(show));
    auto capture = action("style editor capture", "Record one flip trick's clip on the practice stage", Group::gameplay,
                          {trick_argument(true)});
    capture.run = [](const Model &m, const Values &args, const Output &out) {
        const auto trick = trick_at(args, 0);
        if (!trick) return out("error: Give a flip trick.");
        if (!style::capturable(trick)) return out(error(style::not_capturable_text));
        if (!in_solo()) return out(error(style::solo_recording_text));
        if (server_too_full()) return out(error(style::full_server_text));
        if (!layer_ready(m)) return out(error(style_layer::unavailable_text));
        style_editor::request_capture(trick);
    };
    registry.add(std::move(capture));
    auto record = action("style editor record",
                         "Record every flip trick that has no clip or an old one, in one trip to the practice stage", Group::gameplay);
    record.run = [](const Model &m, const Values &, const Output &out) {
        if (!in_solo()) return out(error(style::solo_recording_text));
        if (!layer_ready(m)) return out(error(style_layer::unavailable_text));
        style_editor::request_record_tricks();
        out("Recording your tricks.");
    };
    registry.add(std::move(record));
    auto forget = action("style editor forget",
                         "Delete the saved clip of a flip trick, or all clips. 'Record all my tricks' in the editor records it again",
                         Group::gameplay, {argument("trick", Type::text)});
    forget.run = [](const Model &, const Values &args, const Output &out) {
        const auto &name = std::get<std::string>(args[0]);
        const bool all = console::equal(name, "all");
        const auto trick = style::parse_flip_trick(name);
        if (!all && !trick) return out(error(style::not_flip_trick_text));
        style_editor::request_forget(all ? std::uint8_t{} : *trick);
        out("Forgetting it.");
    };
    registry.add(std::move(forget));
}

void add_playback_commands(Commands &registry) {
    auto play = action("style editor play", "Play the shown clip in a loop from the shown moment", Group::gameplay);
    play.run = [](const Model &, const Values &, const Output &) { style_editor::request_play(true); };
    registry.add(std::move(play));
    auto pause = action("style editor pause", "Hold the shown clip at the shown moment", Group::gameplay);
    pause.run = [](const Model &, const Values &, const Output &) { style_editor::request_play(false); };
    registry.add(std::move(pause));
    auto hold = action("style editor hold", "Hold the shown clip at a time on the trick's timeline (0 to 3)", Group::gameplay,
                       {number_argument("time", 0, style::trick_end)});
    hold.run = [](const Model &, const Values &args, const Output &) { style_editor::request_hold(number_at(args, 0)); };
    registry.add(std::move(hold));
    auto frames = argument("frames", Type::integer);
    frames.minimum = -style::max_step_frames;
    frames.maximum = style::max_step_frames;
    auto step = action("style editor step", "Move the held clip forward or backward by frames of 1/60 s", Group::gameplay, {frames});
    step.run = [](const Model &, const Values &args, const Output &) {
        style_editor::request_step(static_cast<int>(std::get<std::int64_t>(args[0])));
    };
    registry.add(std::move(step));
    auto speed = action("style editor speed", "Play the shown clip at a fraction of its recorded speed (0.1 to 1)", Group::gameplay,
                        {number_argument("speed", style::min_speed, 1)});
    speed.run = [](const Model &, const Values &args, const Output &) { style_editor::request_speed(number_at(args, 0)); };
    registry.add(std::move(speed));
    const auto turn = [] { return number_argument("amount", -style::max_orbit_step, style::max_orbit_step); };
    auto orbit = action("style editor orbit",
                        "Turn the editor camera around the stand-in (yaw, pitch), move it (distance) and raise its target (height)",
                        Group::gameplay,
                        {turn(), turn(), turn(), number_argument("height", -style::max_orbit_step, style::max_orbit_step, true)});
    orbit.run = [](const Model &, const Values &args, const Output &) {
        style_editor::request_orbit(number_at(args, 0), number_at(args, 1), number_at(args, 2),
                                    args.size() > 3 ? number_at(args, 3) : 0.0f);
    };
    registry.add(std::move(orbit));
}

void add_screen_commands(Commands &registry) {
    auto open = action("style editor open", "Open the style editor screen. Select the trick to edit there", Group::gameplay);
    open.run = [](const Model &m, const Values &, const Output &out) {
        if (server_too_full()) return out(error(style::full_server_text));
        if (style_capture::active()) return out(error(style::busy_text));
        if (!layer_ready(m)) return out(error(style_layer::unavailable_text));
        style_editor::open_editor();
        out("Opening the style editor.");
    };
    registry.add(std::move(open));
    auto close = action("style editor close", "Close the style editor screen", Group::gameplay);
    close.run = [](const Model &, const Values &, const Output &) {
        style_editor::close_and_hide();
    };
    registry.add(std::move(close));
}
} // namespace

std::string style_editor_status() { return std::format(" Editor {}.", style_editor::screen_open() ? "open" : "closed"); }

void register_style_editor_commands(Commands &registry) {
    add_clip_commands(registry);
    add_playback_commands(registry);
    add_screen_commands(registry);
}
} // namespace dingosdk::console
