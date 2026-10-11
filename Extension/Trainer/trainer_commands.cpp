#include "trainer.h"
#include "Extension/Console/commands.h"

// The `trainer` console commands: the only way anything reaches the trainer, the menu included.
namespace dingosdk::console {
void register_trainer_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
        bool quiet; // sent on every slider movement: not echoed into the console
    };
    const Verb verbs[]{
        {"status", "What the trainer is doing", nullptr, false},
        {"open", "Open the menu on the trainer page", "[tune|presets|practice|map|realistic|fun|everything]", false},
        {"set", "Set a physics tuning value live", "<value id> <number>", true},
        {"freeze", "Keep a value through presets and resets", "<value id> 0|1", true},
        {"reset", "Put a value (or all) back to stock", "<value id>|all", false},
        {"find", "List tuning values whose id contains the words", "<words>", false},
        {"preset", "Apply, remove, save or delete a preset", "apply|remove|save|delete <name>", false},
        {"dial", "Turn a built-in preset up or down: 1 is the game's own", "<multiplier> <preset name>", false},
        {"slot", "Select a marker slot", "<1-5>", false},
        {"marker", "Save, go to or clear a marker", "save|go|clear [slot]", false},
        {"tp", "Teleport the skater", "<x> <y> <z>", false},
        {"waypoint", "Teleport to the waypoint placed on the pause map, onto the surface there", "[info]", false},
        {"ground", "Teleport to the topmost surface at a map position", "<x> <z>", false},
        {"spot", "Go to one of the map author's spots", "<number>", false},
        {"option", "Trainer options", "hud|hud_jump|auto_return|pad|log 0|1, return_delay <seconds>, hippy_height|nocomply_height|boneless_height|offboard_height|flip_speed <x>", true},
        {"profile", "The preset this map applies on load", "set <preset>|clear", false},
        {"jumps", "The last measured jump", nullptr, false},
        {"where", "The skater's position, heading and speed", nullptr, false},
        {"states", "Physics states the skater has been in (diagnostic)", nullptr, false},
        {"classes", "The game's tuning classes the trainer found in memory (diagnostic)", "[find]", false},
        {"refresh", "Refresh the skater's cached copy of the tuning (diagnostic)", nullptr, false},
        {"dump", "Write every tuning value to a text file", nullptr, false},
        {"selftest", "Check the trainer against the running game", nullptr, false},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("trainer ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.echo_input = !verb.quiet;
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    // Split on spaces; a preset name keeps its spaces (the trainer joins the rest).
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            // Value ids contain spaces ("Push.Speed x"): the last word is the number, the rest the id.
            if ((name == "set" || name == "freeze") && words.size() > 2) {
                std::string id;
                for (std::size_t i = 0; i + 1 < words.size(); ++i) id += (i ? " " : "") + words[i];
                words = {id, words.back()};
            } else if (name == "reset" && words.size() > 1) {
                std::string id;
                for (std::size_t i = 0; i < words.size(); ++i) id += (i ? " " : "") + words[i];
                words = {id};
            } else if (name == "find" && words.size() > 1) {
                std::string all;
                for (std::size_t i = 0; i < words.size(); ++i) all += (i ? " " : "") + words[i];
                words = {all};
            }
            out(trainer::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
