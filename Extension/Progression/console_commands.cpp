#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "script_natives.h"
namespace dingosdk::console {
namespace {
void save(const std::string &operation, const Values &args, const Output &out) {
    std::vector<std::string> words{operation};
    for (const auto &value : args)
        words.push_back(value_text(value));
    const bool saved = set_local_progression(words);
    const auto state = local_profile_progression();
    out((saved ? "" : "error: ") + (state.feedback.empty() ? "Local profile unavailable." : state.feedback));
}
Argument count(std::string name, double maximum) {
    auto arg = argument(std::move(name), Type::unsigned_integer);
    arg.minimum = 0;
    arg.maximum = maximum;
    return arg;
}
} // namespace
void register_progression_commands(Commands &registry) {
    const auto available = [](const Model &m) {
        return State{m.progression.available, {}, "Local profile is unavailable.", {}, false};
    };
    auto stop = count("stop", 255);
    stop.complete = [](const Model &m, auto) {
        std::vector<std::string> result;
        for (const auto &s : m.progression.bus_stops)
            result.push_back(std::to_string(s.number));
        return result;
    };
    auto state = count("state", 2);
    state.choices = {"0", "1", "2"};
    auto bus = action("progression busstop", "Save bus stop state: 0 hidden, 1 visible, 2 unlocked", Group::progression,
                      {stop, state});
    bus.inspect = available;
    bus.run = [](const Model &, const Values &args, const Output &out) { save("busstop", args, out); };
    registry.add(std::move(bus));
    for (const auto &key : {"challenges", "maxranks", "unlockall"}) {
        auto entry = variable(key,
                              equal(key, "challenges")  ? "Show or hide challenges while keeping saved progress"
                              : equal(key, "maxranks") ? "Automatically maximize district ranks"
                                                       : "Own every catalogue item, store and premium pass included "
                                                         "(applies at the next game start)",
                              Group::progression, argument("0|1", Type::boolean));
        entry.aliases = {"progression " + std::string(key)};
        entry.inspect = [key = std::string(key)](const Model &m) {
            const bool value = key == "challenges" ? !m.progression.challenges_hidden
                               : key == "maxranks" ? m.progression.ranks_maxed
                                                   : m.progression.everything_unlocked;
            return boolean_state(m.progression.available, value, "Local profile is unavailable.");
        };
        entry.run = [key = std::string(key)](const Model &, const Values &args, const Output &out) {
            save(key, args, out);
        };
        registry.add(std::move(entry));
    }
    auto name = argument("name");
    name.rest = true;
    auto card = variable("cardname", "Set the name on your player card; multiplayer always uses your Steam name",
                         Group::progression, name);
    card.inspect = [](const Model &m) {
        const auto &custom = m.player_card.custom_name;
        return State{m.player_card.available, custom.empty() ? std::nullopt : std::optional<std::string>(custom),
                     "Local profile is unavailable.", custom.empty() ? "Using your Steam name" : "Local card only",
                     !custom.empty()};
    };
    const auto rename = [](std::string_view value, const Output &out) {
        const bool saved = set_local_player_card_name(value);
        const auto state = local_profile_player_card();
        out((saved ? "" : "error: ") + (state.feedback.empty() ? "Local profile unavailable." : state.feedback));
    };
    card.run = [rename](const Model &, const Values &args, const Output &out) {
        rename(std::get<std::string>(args[0]), out);
    };
    card.reset = [rename](const Model &, const Output &out) { rename({}, out); };
    registry.add(std::move(card));
    auto score = action("ripscore", "Save RIP score, score cap, and level", Group::progression,
                        {count("score", 1000000000), count("cap", 1000000000), count("level", 10000)});
    score.aliases = {"progression ripscore"};
    score.inspect = available;
    score.run = [](const Model &, const Values &args, const Output &out) { save("ripscore", args, out); };
    registry.add(std::move(score));
    auto wear = action("boardwear reset", "Clear accumulated board wear", Group::gameplay);
    wear.inspect = [](const Model &m) {
        return State{m.offline.board_wear.available, {},
                     "The board wear settings field has not been resolved.", {}, false};
    };
    wear.run = [](const Model &m, const Values &, const Output &out) {
        profile_runtime::reset_board_wear();
        out(m.offline.board_wear.effective
                ? "Board wear cleared on the next tick."
                : "Board wear reset queued; it applies once board wear is enabled.");
    };
    registry.add(std::move(wear));
    auto district = argument("district");
    district.complete = [](const Model &m, auto) {
        std::vector<std::string> result;
        for (const auto &d : m.progression.districts)
            if (!d.id.empty())
                result.push_back(d.id);
        return result;
    };
    auto rank = action("progression district", "Save a district rank and disable automatic max ranks",
                       Group::progression, {district, count("rank", 10000)});
    rank.inspect = available;
    rank.run = [](const Model &, const Values &args, const Output &out) { save("district", args, out); };
    registry.add(std::move(rank));
    auto id = argument("id");
    id.complete = [](const Model &m, auto) {
        std::vector<std::string> result;
        for (const auto &row : m.missions)
            result.push_back(row.id);
        return result;
    };
    for (const auto &operation : {"complete", "reset"}) {
        auto entry = action("mission " + std::string(operation),
                            equal(operation, "complete") ? "Save mission completion" : "Reset saved mission completion",
                            Group::progression, {id});
        entry.inspect = [](const Model &m) {
            return State{m.missions_available, {}, "Local missions are unavailable.", {}, false};
        };
        entry.run = [complete = equal(operation, "complete")](const Model &, const Values &args, const Output &out) {
            const bool saved = set_local_mission_completed(std::get<std::string>(args[0]), complete);
            const auto model = local_profile_missions();
            out((saved ? "" : "error: ") + (model.feedback.empty() ? "Local profile unavailable." : model.feedback));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
