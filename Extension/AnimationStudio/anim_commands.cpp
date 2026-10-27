#include "anim_studio.h"
#include "Extension/Console/commands.h"
#include <string>

namespace dingosdk::console {
void register_animation_studio_commands(Commands& registry) {
    // Opening it from a line, which is what the ReSkate menu's tile queues as well.
    auto open_verb = argument("action", Type::text, false);
    open_verb.choices = {"open", "close", "probe", "stance"};
    auto open = action("animstudio", "Open or close the Animator Studio (F8); probe looks for where the game keeps the clip it plays", Group::gameplay, {open_verb});
    open.run = [](const Model&, const Values& values, const Output& out) {
        const auto verb = values.empty() ? std::string("open") : std::get<std::string>(values[0]);
        if (verb == "stance") {
            out(anim_studio::toggle_stance_readout());
            return;
        }
        if (verb == "probe") {
            out(anim_studio::arm_live_probe());
            return;
        }
        const bool wanted = verb != "close";
        anim_studio::request_open(wanted);
        out(wanted ? "Opening the Animator Studio." : "Closing the Animator Studio.");
    };
    registry.add(std::move(open));
    auto verb = argument("action", Type::text, true);
    verb.choices = {"start", "stop", "status"};
    auto entry = action("animstudio trace", "Read-only animation transition trace (60 s); exports a local CSV", Group::gameplay, {verb});
    entry.run = [](const Model&, const Values& values, const Output& out) {
        out(anim_studio::research_trace(values.empty() ? "status" : std::get<std::string>(values[0])));
    };
    registry.add(std::move(entry));
    auto dump = action("animstudio dump", "Export the last 15 gameplay seconds as local diagnostic text files", Group::gameplay, {});
    dump.run = [](const Model&, const Values&, const Output& out) {
        std::string message; anim_studio::export_diagnostics(message); out(message);
    };
    registry.add(std::move(dump));
}
} // namespace dingosdk::console
