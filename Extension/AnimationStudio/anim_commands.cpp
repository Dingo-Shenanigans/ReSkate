#include "anim_studio.h"
#include "Extension/Console/commands.h"

namespace dingosdk::console {
void register_animation_studio_commands(Commands& registry) {
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
