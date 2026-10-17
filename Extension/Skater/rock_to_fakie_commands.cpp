#include "rock_to_fakie.h"
#include "Extension/Console/commands.h"

// The Rock to Fakie console verb (rock_to_fakie.h).
namespace dingosdk::console {
void register_rock_to_fakie_commands(Commands &registry) {
    std::vector<Argument> arguments;
    auto rest = argument("on|off|status", Type::text, true);
    rest.rest = true;
    arguments.push_back(std::move(rest));
    auto entry = action("rocktofakie", "Rock to Fakie: a Board or Lip Stall shows as Rock to Fakie, and left with B is recorded as one (kept)",
        Group::gameplay, std::move(arguments));
    entry.run = [](const Model &, const Values &values, const Output &out) {
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
        out(rock_to_fakie::command(words));
    };
    registry.add(std::move(entry));
}
} // namespace dingosdk::console
