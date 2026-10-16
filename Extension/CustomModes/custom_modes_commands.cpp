#include "custom_modes.h"
#include "Extension/Console/commands.h"
#include <charconv>
#include <format>

// The `modes` console commands: the custom multiplayer modes, hosting a game for one.
namespace dingosdk::console {
namespace {
std::vector<std::string> words_of(const Values &values) {
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
    return words;
}
} // namespace

void register_custom_modes_commands(Commands &registry) {
    auto rest = [](const char *usage) {
        auto argument_ = argument(usage, Type::text, true);
        argument_.rest = true;
        return std::vector<Argument>{std::move(argument_)};
    };
    auto list = action("modes list", "The custom multiplayer modes", Group::gameplay);
    list.run = [](const Model &, const Values &, const Output &out) {
        const auto modes = custom_modes::list();
        if (modes.empty()) {
            out("No custom multiplayer modes are installed.");
            return;
        }
        for (const auto &mode : modes)
            out(std::format("{} ({}): {}{}", mode.name, mode.id, mode.tagline, mode.panel.available ? "" : " - " + mode.panel.unavailable));
    };
    registry.add(std::move(list));

    auto host = action("modes host", "Host a game for a custom mode (its map is loaded first if needed)", Group::gameplay,
                       rest("<mode id> [public|code] [players] [name]"));
    host.run = [](const Model &, const Values &values, const Output &out) {
        const auto words = words_of(values);
        if (words.empty()) {
            out("usage: modes host <mode id> [public|code] [players] [name]");
            return;
        }
        bool public_lobby = true;
        unsigned players = 8;
        std::string name;
        for (std::size_t i = 1; i < words.size(); ++i) {
            unsigned number{};
            const auto &word = words[i];
            if (word == "public" || word == "code") public_lobby = word == "public";
            else if (std::from_chars(word.data(), word.data() + word.size(), number).ec == std::errc{} && name.empty()) players = number;
            else name += (name.empty() ? "" : " ") + word;
        }
        out(custom_modes::host(words[0], public_lobby, players, name));
    };
    registry.add(std::move(host));

    auto cancel = action("modes cancel", "Stop waiting to host a custom mode's game", Group::gameplay);
    cancel.run = [](const Model &, const Values &, const Output &out) { out(custom_modes::cancel_host()); };
    registry.add(std::move(cancel));

    auto status = action("modes status", "What hosting a custom mode's game is doing", Group::gameplay);
    status.run = [](const Model &, const Values &, const Output &out) {
        const auto state = custom_modes::host_state();
        out(state.status.empty() ? std::string("No custom mode game is hosted.") : state.status);
    };
    registry.add(std::move(status));
}
} // namespace dingosdk::console
