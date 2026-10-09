#include "s3hom_hud.h"
#include "Extension/Console/commands.h"

// The `hom` console commands: Hall Of Meat status and diagnostics.
namespace dingosdk::console {
void register_skate3_hom_commands(Commands &registry) {
    struct Verb {
        const char *name, *description, *usage;
    };
    const Verb verbs[]{
        {"status", "Hall Of Meat state and best bail", nullptr},
        {"rig", "Log the skater skeleton (diagnostic)", nullptr},
        {"phys", "Log the skater physics bodies (diagnostic)", nullptr},
        {"roll", "X-ray bone roll from the joints (1) or from the body forward (0)", "0|1"},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("hom ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(overlay::skate3_hom_command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
