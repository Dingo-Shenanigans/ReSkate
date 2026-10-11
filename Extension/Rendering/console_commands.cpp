#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Rendering/texture_refresh.h"
namespace dingosdk::console {
void register_graphics_commands(Commands &registry) {
    constexpr const char *names[]{"filmgrain", "vignette", "chromaticaberration"};
    constexpr const char *descriptions[]{"Film grain effect", "Darkening around the screen edges",
                                         "Color fringing at screen edges"};
    for (unsigned i = 0; i < graphics_keys.size(); ++i) {
        auto value = argument("0|1|-1", Type::integer);
        value.choices = {"-1", "0", "1"};
        auto entry = variable(names[i], descriptions[i], Group::graphics, value);
        entry.aliases = {"graphics " + std::string(graphics_keys[i])};
        entry.inspect = [i](const Model &m) {
            return State{
                m.graphics.available,
                m.graphics.ready[i] ? std::optional<std::string>(m.graphics.enabled[i] ? "1" : "0") : std::nullopt,
                "Graphics controls are unavailable.",
                "Saved override: " +
                    (m.graphics.choices.effects[i] == -1 ? "default" : std::to_string(m.graphics.choices.effects[i])),
                m.graphics.choices.effects[i] != -1};
        };
        entry.run = [i](const Model &, const Values &args, const Output &out) {
            const bool saved =
                set_local_graphics_control(graphics_keys[i], static_cast<int>(std::get<std::int64_t>(args[0])));
            out(saved ? local_profile_graphics_controls().status : "error: Graphics control could not be saved.");
        };
        entry.reset = [i](const Model &, const Output &out) {
            out(set_local_graphics_control(graphics_keys[i], -1) ? local_profile_graphics_controls().status
                                                                 : "error: Graphics control could not be restored.");
        };
        registry.add(std::move(entry));
    }
    auto value = argument("-1", Type::integer, true);
    value.choices = {"-1"};
    auto reset = action("graphics reset", "Restore all authored graphics effects", Group::graphics, {value});
    reset.run = [](const Model &, const Values &, const Output &out) {
        out(set_local_graphics_control("reset", -1) ? local_profile_graphics_controls().status
                                                    : "error: Could not restore graphics.");
    };
    registry.add(std::move(reset));
    auto refresh = action("textures refresh", "Composite your skater's tattoos and board's deck, grip and stickers "
                          "again at the current texture quality, once no menu is up", Group::graphics);
    refresh.run = [](const Model &, const Values &, const Output &out) {
        texture_refresh::request();
        out("Queued; the result is in the log.");
    };
    registry.add(std::move(refresh));
}
} // namespace dingosdk::console
