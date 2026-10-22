#include "filmer.h"
#include "Extension/Console/commands.h"

// The `filmer` console commands: the only way anything reaches the Filmer, the menu and the binds
// (Filmer on / off, slap the lens) included.
namespace dingosdk::console {
void register_filmer_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
        bool quiet; // sent on every slider movement: not echoed into the console
    };
    const Verb verbs[]{
        {"status", "What the Filmer is doing", nullptr, false},
        {"toggle", "Turn the Filmer on (as last used, or the mode given) or off", "[follow|film]", false},
        {"mode", "off; follow: the Filmer (another skater) rides with a skater and films them; film: you film (right stick aims)", "off|follow|film", false},
        {"subject", "Whom the Filmer follows", "me|<player>", false},
        {"target", "Whom the assist frames while you film", "auto|<player>", false},
        {"set", "Change a Filmer option",
         "style auto|behind|side|ahead, side auto|left|right|swap, camitem <cosmetic>|default, camslot hand|eyewear|default, lens fisheye|long, distance|height|tilt|human|reaction|sensitivity|hold|bend|age|gap|mousesens|lift|crouchdepth|hunch|distvariety|heightvariety <number>, "
         "cycle|slamzoom|stopzoom|debug|assist|invert|recenter|hidehud|stick|clicks|drops|lunge|cruise|look|anywhere|fisheye|bars|hood|grade|soft|scanlines|grain|date|viewfinder|"
         "dropouts|headswitch|tracking|smear|iris 0|1",
         true},
        {"switchside", "The Filmer goes over to your other side (left / right) and keeps to it", nullptr, false},
        {"slap", "Slap the lens: ends the clip", nullptr, false},
        {"clips", "The clips marked this session", nullptr, false},
        {"trace", "Record what the Filmer sees for 30 seconds (trace.csv), to send for tuning", nullptr, false},
        {"angle", "The Filmer's angles: one held, or several Auto moves between", "next|auto|behind|side|ahead|toggle behind|side|ahead", false},
        {"cruise", "Film mode: ride at the speed that holds you behind the skater you frame", "[on|off]", false},
        {"zoom", "Long lens, while the camera is held still: zoom in, out or back to its widest", "in|out|reset", false},
        {"preset", "The EFFECTS preset: clean (no look), vx (the VX1000 look) or worn (an old tape)", "clean|vx|worn", false},
        {"reset", "Put every Filmer option back to its default", nullptr, false},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("filmer ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.echo_input = !verb.quiet;
        entry.run = [name = std::string(verb.name)](const Model &model, const Values &values, const Output &out) {
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
            out(filmer::command(name, words, model.debug.game_ui_hidden));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
