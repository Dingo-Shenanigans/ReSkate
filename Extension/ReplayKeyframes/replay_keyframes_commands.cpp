#include "replay_keyframes.h"
#include "replay_settings.h"
#include "Extension/Console/commands.h"
#include <format>
#include <string>

// The replay editor camera's keyframe smoothing (replay_keyframes.h); the menus' switches (Mod Options >
// Visuals, and the overlay's Skater > Freecam) send `keysmooth on|off` too.
namespace dingosdk::console {
void register_replay_keyframes_commands(Commands &registry) {
    const auto unavailable = "Keyframe smoothing is unavailable for this game build.";
    auto on = variable("keysmooth", "Smooth the replay editor's camera through its keyframes: it still passes through "
                       "every key at its time, without the game's changes of speed at them. Off is the game's own curve. "
                       "Kept between runs",
                       Group::graphics, argument("on|off", Type::boolean));
    on.inspect = [unavailable](const Model &) {
        return boolean_state(replay_keyframes::available(), replay_keyframes::on(), unavailable);
    };
    on.run = [](const Model &, const Values &args, const Output &out) {
        out(replay_keyframes::set_on(std::get<bool>(args[0])));
    };
    on.reset = [](const Model &, const Output &out) { out(replay_keyframes::set_on(false)); };
    registry.add(std::move(on));

    const auto fraction = [](const char *name) {
        auto value = argument(name, Type::number);
        value.minimum = 0.0;
        value.maximum = 1.0;
        return value;
    };
    auto smooth = variable("keysmooth amount", "How smooth the camera is when keysmooth is on: 1 (the default) is the "
                           "smoothest that still passes through every key, 0 the game's own curve",
                           Group::graphics, fraction("0..1"));
    smooth.inspect = [unavailable](const Model &) {
        return State{replay_keyframes::available(), std::format("{:g}", replay_keyframes::smoothness()), unavailable, {},
                     replay_keyframes::smoothness() != replay_keyframes::Settings{}.amount};
    };
    smooth.run = [](const Model &, const Values &args, const Output &out) {
        out(replay_keyframes::set_smoothness(static_cast<float>(std::get<double>(args[0]))));
    };
    smooth.reset = [](const Model &, const Output &out) {
        out(replay_keyframes::set_smoothness(replay_keyframes::Settings{}.amount));
    };
    registry.add(std::move(smooth));

    auto status = action("keysmooth status", "Keyframe smoothing's settings, and how many framing segments were built, by "
                         "Behaviour, since the last status", Group::graphics);
    status.execution = Execution::local;
    status.run = [](const Model &, const Values &, const Output &out) { out(replay_keyframes::status()); };
    registry.add(std::move(status));
}
} // namespace dingosdk::console
