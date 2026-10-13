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
}
} // namespace dingosdk::console
