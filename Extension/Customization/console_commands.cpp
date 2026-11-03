#include "Extension/Console/commands.h"
#include "Extension/Assets/peer_cosmetics_fetch.h"
#include "Extension/Customization/item_browser.h"
#include "Extension/Customization/worn_items.h"
#include "Extension/Multiplayer/Remote/outfit_filter.h"
#include <charconv>

namespace dingosdk::console {
void register_item_commands(Commands &registry) {
    auto arguments = argument("verb_and_arguments", Type::text, true);
    arguments.rest = true;
    arguments.complete = [](const Model &, auto) {
        return std::vector<std::string>{"fav", "search", "filter", "status", "peek", "poke"};
    };
    auto items = action("items", "Skater item grids: fav (favorite the highlighted item), search <text>, "
                        "filter all|favorites|mods|game, status; peek/poke are research aids",
                        Group::console, {std::move(arguments)});
    items.run = [](const Model &, const Values &values, const Output &out) {
        const auto text = values.empty() ? std::string{} : std::get<std::string>(values[0]);
        const auto space = text.find(' ');
        const auto verb = text.substr(0, space);
        const auto rest = space == std::string::npos ? std::string{} : text.substr(space + 1);
        if (verb == "fav") {
            item_browser::toggle_focused_favorite();
            out("Favorite toggled for the highlighted item.");
        } else if (verb == "search") {
            item_browser::set_search(rest);
            out(rest.empty() ? "Search cleared." : "Searching for \"" + rest + "\".");
        } else if (verb == "filter") {
            for (std::size_t i = 0; i < std::size(item_browser::filter_names); ++i)
                if (item_browser::folded(item_browser::filter_names[i]).starts_with(item_browser::folded(rest)) && !rest.empty()) {
                    item_browser::set_filter(static_cast<item_browser::Filter>(i));
                    out("Filter: " + std::string(item_browser::filter_names[i]) + ".");
                    return;
                }
            item_browser::cycle_filter();
            out("Next filter.");
        } else if (verb == "peek" || verb == "poke") {
            item_browser::request_poke(text);
            out("Queued; the answer is in the log.");
        } else {
            item_browser::request_status();
            out("Status written to the log.");
        }
    };
    registry.add(std::move(items));
}
// Other players' outfits: what of them shows here (outfit_filter.h) and the cosmetics fetched
// for them (peer_cosmetics_fetch.h). The menu's MULTIPLAYER > OUTFITS tab does the same.
void register_outfit_commands(Commands &registry) {
    auto arguments = argument("verb_and_arguments", Type::text, true);
    arguments.rest = true;
    arguments.complete = [](const Model &, auto) {
        return std::vector<std::string>{"status", "mod-items", "hide-item", "show-item", "hide-mod", "show-mod",
                                        "fetch", "automatic", "sources", "wanted", "keep", "remove", "cleanup"};
    };
    auto outfits = action("outfits", "Other players' outfits: status, mod-items on|off, hide-item|show-item <key>, "
                          "hide-mod|show-mod <Owner-Name>, fetch <Owner-Name[-1.2.3]> [player], automatic on|off, sources, wanted, "
                          "keep|remove <Owner-Name>, "
                          "cleanup unused <days>|size <MB>|every-launch|never",
                          Group::console, {std::move(arguments)});
    outfits.run = [](const Model &, const Values &values, const Output &out) {
        namespace filter = multiplayer::outfit_filter;
        namespace fetched = peer_cosmetics_fetch;
        namespace store = mods::peer_cosmetics;
        const auto text = values.empty() ? std::string{} : std::get<std::string>(values[0]);
        const auto space = text.find(' ');
        const auto verb = text.substr(0, space);
        auto rest = space == std::string::npos ? std::string{} : text.substr(space + 1);
        const auto lower = [](std::string value) {
            for (auto &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        };
        auto rules = *filter::rules();
        const auto save = [&](const std::string &said) { out(filter::set_rules(rules) ? said : said + " (It could not be saved.)"); };
        if (verb == "mod-items" && (rest == "on" || rest == "off")) {
            rules.mod_items = rest == "on";
            save(rules.mod_items ? "Other players' mod items show." : "Other players wear the game's own items only.");
        } else if ((verb == "hide-item" || verb == "show-item") && !rest.empty()) {
            if (verb == "hide-item") rules.items.insert(lower(rest));
            else rules.items.erase(lower(rest));
            save((verb == "hide-item" ? "Hidden on everyone: " : "Shown again: ") + rest + ".");
        } else if ((verb == "hide-mod" || verb == "show-mod") && !rest.empty()) {
            if (verb == "hide-mod") rules.mods.insert(lower(rest));
            else rules.mods.erase(lower(rest));
            save("Items from " + rest + (verb == "hide-mod" ? " are hidden on everyone." : " show again."));
        } else if (verb == "fetch" && !rest.empty()) {
            const auto gap = rest.find(' ');
            out(fetched::fetch(rest.substr(0, gap), gap == std::string::npos ? std::string{} : rest.substr(gap + 1)));
        } else if (verb == "automatic" && (rest == "on" || rest == "off")) {
            auto policy = fetched::view().policy;
            policy.automatic = rest == "on";
            out(!fetched::set_policy(policy) ? std::string("The setting could not be saved.")
                : policy.automatic ? "Items other players wear that you do not have are fetched on their own."
                                   : "Items other players wear are fetched only when you ask.");
        } else if (verb == "sources") {
            // What this player's outfit tells everyone else, as sent with it.
            std::vector<std::string> keys;
            try {
                for (const auto &item : worn_items::worn_outfit()) keys.push_back(item.item);
                for (const auto &part : worn_items::worn_board(false)) keys.push_back(part.item);
            } catch (...) {
                return out("Your outfit cannot be read now.");
            }
            const auto sources = filter::item_sources(keys);
            if (sources.empty()) out("You wear nothing from a mod named as a Thunderstore package, so your outfit names no package.");
            for (const auto &source : sources) out("  " + source.asset + " from " + source.package);
        } else if (verb == "wanted") {
            // What others wear that this PC lacks, from a package they named.
            const auto wanted = filter::wanted();
            if (wanted.empty()) out("Nobody here wears an item you lack from a package they named (or you hide them).");
            for (const auto &want : wanted) out("  " + want.key + " from " + want.package + ", worn by " + std::to_string(want.player));
        } else if (verb == "keep" && !rest.empty()) {
            out(fetched::keep(rest) ? rest + " is one of your own mods now; it is never deleted for being unused."
                                    : rest + " is not a fetched mod.");
        } else if (verb == "remove" && !rest.empty()) {
            out(fetched::remove_at_next_launch(rest, true) ? rest + " is deleted at the next launch." : rest + " is not a fetched mod.");
        } else if (verb == "cleanup" && !rest.empty()) {
            auto policy = fetched::view().policy;
            const auto gap = rest.find(' ');
            const auto how = rest.substr(0, gap);
            unsigned amount{};
            if (gap != std::string::npos) std::from_chars(rest.data() + gap + 1, rest.data() + rest.size(), amount);
            if (how == "unused") {
                policy.cleanup = store::Cleanup::unused;
                if (amount) policy.days = amount;
            } else if (how == "size") {
                policy.cleanup = store::Cleanup::size;
                if (amount) policy.max_mb = amount;
            } else if (how == "every-launch") policy.cleanup = store::Cleanup::every_launch;
            else if (how == "never") policy.cleanup = store::Cleanup::never;
            else return out("cleanup unused <days>, size <MB>, every-launch or never.");
            out(fetched::set_policy(policy) ? "Saved; it applies at the next launch." : "The policy could not be saved.");
        } else {
            const auto view = fetched::view();
            const auto state = fetched::status();
            out(std::string("Mod items on others: ") + (rules.mod_items ? "shown" : "hidden") + "; hidden items " +
                std::to_string(rules.items.size()) + ", mods " + std::to_string(rules.mods.size()) + ", players " +
                std::to_string(rules.players.size()) + ".");
            out(std::string("Fetching missing items: ") +
                (view.policy.automatic ? "on its own, up to " + std::to_string(view.policy.automatic_mb) + " MB a pack" : "when asked") + ".");
            out("Fetched cosmetics: " + std::to_string(view.mods.size()) + " mods, " + std::to_string(view.bytes >> 20) + " MB; cleanup " +
                std::string(store::cleanup_names[static_cast<std::size_t>(view.policy.cleanup)]) + " (" +
                std::to_string(view.policy.days) + " days, " + std::to_string(view.policy.max_mb) + " MB).");
            for (const auto &mod : view.mods)
                out("  " + mod.folder + " " + mod.version + (mod.from.empty() ? "" : ", for " + mod.from) + ", " +
                    std::to_string(mod.bytes >> 20) + " MB" + (mod.remove ? ", deleted at next launch" : ""));
            if (!state.line.empty())
                out("Last fetch: " + state.line +
                    (state.busy && state.total ? " (" + std::to_string(state.received >> 20) + " of " + std::to_string(state.total >> 20) + " MB)" : ""));
        }
    };
    registry.add(std::move(outfits));
}
} // namespace dingosdk::console
