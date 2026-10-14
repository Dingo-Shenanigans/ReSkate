#include "grind_exit_control.h"
#include "Extension/Console/commands.h"

// The grind exit triggers' console verbs (grind_exit_control.h).
namespace dingosdk::console {
void register_grind_exit_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
    };
    const Verb verbs[]{
        {"exits", "Grind exit triggers: LT/RT pressed while grinding pick the side you pop out to (kept)", "on|off|status"},
        {"stance", "Grind exits: your skater's stance, for toe/heel sides (kept)", "goofy|regular"},
        {"sides", "Grind exits: LT toe / RT heel, or LT left / RT right of travel (kept)", "toeheel|travel"},
        {"strength", "Grind exits: how far to the side a trigger exit pops, percent of the full exit (kept; 100 is the game's own)", "<10-100>"},
        {"guard", "Grind exits: hide a trigger used to pick the side from the game until released (no accidental grab)", "on|off"},
        {"leanguard", "Grind exits: the left stick's left/right reaches the game at the pop while a trigger picks the side", "on|off"},
        {"prewindow", "Grind exits: seconds a trigger may be pressed before the grind connects and still count (1)", "<seconds>"},
        {"sidespeed", "Grind exits: sideways speed of a chosen exit when the game had none (m/s, game's own 1.8)", "<m/s>"},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("grindexit ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words{name};
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
            out(grind_exit_control::command(words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
