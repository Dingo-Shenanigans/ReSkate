#include "board_break.h"
#include "board_break_model.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Console/commands.h"
#include <algorithm>

// Board Break's console switches; the menu's (Mod Options > Player) send them too.
namespace dingosdk::console {
void register_board_break_commands(Commands &registry) {
    const auto unavailable = "Board Break is unavailable for this game build.";
    auto on = variable("boardbreak", "A hard slam can snap your board at the nose, in the middle or at the tail "
                       "(needs one of the Board Break deck packs)",
                       Group::gameplay, argument("0|1", Type::boolean));
    on.inspect = [unavailable](const Model &) {
        const auto deck = board_break::deck();
        return boolean_state(board_break::available(), board_break::enabled(), unavailable,
                             deck.empty() ? std::string("no Board Break deck is in the game") : "the " + board_break::deck_label(deck) + " deck");
    };
    on.run = [](const Model &, const Values &args, const Output &) { board_break::set_enabled(std::get<bool>(args[0])); };
    registry.add(std::move(on));

    auto which = argument("deck", Type::text, true);
    which.complete = [](const Model &, auto) {
        auto names = mods::installed_board_break_decks();
        names.insert(names.begin(), "next");
        return names;
    };
    auto deck = action("boardbreak deck", "Which deck every board has, by its name or its number, or next; the level loads again with it",
                       Group::gameplay, {std::move(which)});
    deck.run = [](const Model &, const Values &args, const Output &out) {
        if (!args.empty() && !std::get<std::string>(args[0]).empty()) {
            out(board_break::choose_deck(std::get<std::string>(args[0])));
            return;
        }
        // No deck named: which one is in the game, and which there are. The decks the feature was
        // made with keep their numbers; the others follow.
        const auto installed = mods::installed_board_break_decks();
        const auto &known = mods::board_break_decks;
        std::string list;
        std::size_t number{};
        const auto add = [&](std::string_view name, bool there) {
            const auto place = ++number;
            list += (place > 1 ? ", " : "") + std::to_string(place) + " " + board_break::deck_label(name) + (there ? "" : " (not installed)");
        };
        for (const auto name : known) add(name, std::find(installed.begin(), installed.end(), name) != installed.end());
        for (const auto &name : installed)
            if (std::find(known.begin(), known.end(), name) == known.end()) add(name, true);
        out("In the game: " + board_break::deck_label(board_break::deck()) + ". Decks: " + list + ".");
    };
    registry.add(std::move(deck));

    auto where = argument("nose|mid|tail", Type::text, true);
    where.complete = [](const Model &, auto) {
        return std::vector<std::string>(board_break::place_names.begin(), board_break::place_names.end());
    };
    auto snap = action("boardbreak snap", "Break your board now, at the nose, in the middle, at the tail or wherever, to look at a break: "
                       "it stays broken until `boardbreak mend` or a respawn. A piece only comes off while you ride",
                       Group::gameplay, {std::move(where)});
    snap.run = [](const Model &, const Values &args, const Output &out) {
        const auto wanted = args.empty() ? std::string{} : std::get<std::string>(args[0]);
        const auto &names = board_break::place_names;
        const auto named = std::find(names.begin(), names.end(), wanted);
        board_break::snap(named == names.end() ? -1 : static_cast<int>(named - names.begin()));
        out(board_break::deck().empty() ? "No Board Break deck is in the game: the board holds." : "Snapped.");
    };
    registry.add(std::move(snap));

    auto mend = action("boardbreak mend", "Make a broken board whole again", Group::gameplay);
    mend.run = [](const Model &, const Values &, const Output &out) {
        board_break::mend();
        out("Mended.");
    };
    registry.add(std::move(mend));
    // The player's settings (board_break_feel.h), kept: with no value each says what it is.
    struct Setting {
        const char *name, *description;
    };
    const Setting settings[]{
        {"always", "Board Break: every slam breaks the board, and every slide that comes down onto a rail (for replay clips)"},
        {"sensitivity", "Board Break: how easily a board breaks, percent (100; 200 takes half the force, 50 twice as much)"},
        {"joined", "Board Break: percent of breaks the grip tape holds together (50)"},
        {"rails", "Board Break: a boardslide or darkslide that comes down too hard onto a rail can snap the board"},
        {"wear", "Board Break: boards wear out with every hit, handle worse and break more easily as they do"},
        {"wearrate", "Board Break: how fast boards wear, percent (100)"},
        {"cracks", "Board Break: the crack (and metal) sound of a break"},
        {"bodysounds", "Board Break: the body's hits in a bail, where and as hard as it hits"},
    };
    for (const auto &setting : settings) {
        auto entry = action(std::string("boardbreak ") + setting.name, setting.description, Group::gameplay,
                            {argument("value", Type::text, true)});
        entry.run = [name = std::string(setting.name)](const Model &, const Values &values, const Output &out) {
            const auto *text = values.empty() ? nullptr : std::get_if<std::string>(&values[0]);
            if (!text || text->empty()) {
                out(name + " is " + std::to_string(board_break::setting_value(board_break::settings(), name)) + ".");
                return;
            }
            int value{};
            if (*text == "on" || *text == "yes") value = 1;
            else if (*text == "off" || *text == "no") value = 0;
            else {
                try {
                    value = std::stoi(*text);
                } catch (...) {
                    out("Give a number (or on/off).");
                    return;
                }
            }
            out(board_break::set_setting(name, value) ? name + " is now " + std::to_string(board_break::setting_value(board_break::settings(), name)) + "."
                                                      : "No such setting.");
        };
        registry.add(std::move(entry));
    }

    auto worn = action("boardbreak worn", "How worn the board of the deck in the game is, percent; 0 is a new board", Group::gameplay,
                       {argument("percent", Type::text, true)});
    worn.run = [](const Model &, const Values &values, const Output &out) {
        const auto *text = values.empty() ? nullptr : std::get_if<std::string>(&values[0]);
        if (text && !text->empty()) {
            try {
                board_break::set_wear(std::stof(*text) / 100.0f);
            } catch (...) {
                out("Give a percent, 0 to 100.");
                return;
            }
        }
        const auto wear = board_break::wear();
        out("The board is " + std::string(board_break::condition_name(wear)) + ", " + std::to_string(static_cast<int>(wear * 100.0f + 0.5f)) + "% worn.");
    };
    registry.add(std::move(worn));

    auto status = action("boardbreak status", "Everything Board Break is set to, and how worn the board is", Group::gameplay);
    status.run = [](const Model &, const Values &, const Output &out) { out(board_break::status()); };
    registry.add(std::move(status));

    auto metal = action("boardbreak metal", "Which materials a board breaking on them sounds like metal on: mark (what you grind now), unmark, clear, list",
                        Group::gameplay, {argument("mark|unmark|clear|list", Type::text, true)});
    metal.run = [](const Model &, const Values &values, const Output &out) {
        const auto *text = values.empty() ? nullptr : std::get_if<std::string>(&values[0]);
        out(board_break::metal_command(text ? *text : std::string("list")));
    };
    registry.add(std::move(metal));

    auto materials = action("boardbreak materials", "The level's materials your skater touched in the last 3 seconds, by number (to find a rail's)",
                            Group::gameplay);
    materials.run = [](const Model &, const Values &, const Output &out) { out(board_break::materials_report()); };
    registry.add(std::move(materials));
}
} // namespace dingosdk::console
