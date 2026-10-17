#include "domination.h"
#include "Extension/Console/commands.h"

// The `domination` console commands (domination.h).
namespace dingosdk::console {
void register_domination_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
        std::string (*run)(const std::vector<std::string> &);
    };
    const Verb verbs[]{
        {"start", "Start a Domination round in the chosen park (on another map: loads it first), with everyone in the session (5 minutes; solo: just you)",
         "[seconds] [solo]",
         [](const std::vector<std::string> &words) { return domination::round_start(words); }},
        {"join", "Join the round someone else in the session started", nullptr,
         [](const std::vector<std::string> &) { return domination::join(); }},
        {"set", "The next round's settings: its map and park (the red-bowl park unless chosen), length, players and teams (off by default)",
         "duration <seconds>|players everyone|solo|teams on|off|map next|previous|park <id>|next|previous",
         [](const std::vector<std::string> &words) { return domination::settings(words); }},
        {"stop", "End the round now; a round someone else started, you leave", nullptr, [](const std::vector<std::string> &) { return domination::round_stop(); }},
        {"status", "The park, the round, and the spot you are on", nullptr,
         [](const std::vector<std::string> &) { return domination::status(); }},
        {"spots", "The park's spots and this round's owners and scores", nullptr,
         [](const std::vector<std::string> &) { return domination::spots(); }},
        {"parks", "Every park, its map and whether that map is installed here", nullptr,
         [](const std::vector<std::string> &) { return domination::parks(); }},
        {"stats", "The rounds you have played to the end: in all, with others and won, per map and per park", nullptr,
         [](const std::vector<std::string> &) { return domination::stats(); }},
        {"scan", "Park-making: write a height map of an area (one ground ray per grid point)", "<x0> <z0> <x1> <z1> [step] [top] [bottom]",
         [](const std::vector<std::string> &words) { return domination::scan(words); }},
        {"tp", "Park-making: teleport facing a heading (degrees from +z towards +x)", "<x> <y|ground> <z> [heading]",
         [](const std::vector<std::string> &words) { return domination::teleport(words); }},
        {"survey", "Park-making: visit a list of places (a file of x y z lines): a height map around each, and a look north, east, south and west",
         "<places file> [half size] [step] | stop",
         [](const std::vector<std::string> &words) { return domination::survey(words); }},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("domination ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [run = verb.run](const Model &, const Values &values, const Output &out) {
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
            out(run(words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
