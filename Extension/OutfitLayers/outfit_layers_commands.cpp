#include "outfit_layers.h"
#include "Extension/Console/commands.h"
#include "Extension/Customization/item_browser.h"

// The console side of collections (the player's own lists of items) and outfit layers (items worn on
// top of the outfit). The item grids do the same with V and B.
namespace dingosdk::console {
namespace {
std::pair<std::string, std::string> verb_of(const Values &values) {
    const auto text = values.empty() ? std::string{} : std::get<std::string>(values[0]);
    const auto space = text.find(' ');
    return {text.substr(0, space), space == std::string::npos ? std::string{} : text.substr(space + 1)};
}
}
void register_outfit_layer_commands(Commands &registry) {
    auto collection_arguments = argument("verb_and_arguments", Type::text, true);
    collection_arguments.rest = true;
    collection_arguments.complete = [](const Model &, auto) {
        return std::vector<std::string>{"list", "new", "delete", "add", "show"};
    };
    auto collections = action("collections", "Your own item collections: list, new <name>, delete <name|number>, "
                              "add <name|number> (the highlighted item in or out), show <name|number> (the item grids show only it)",
                              Group::console, {std::move(collection_arguments)});
    collections.run = [](const Model &, const Values &values, const Output &out) {
        const auto [verb, rest] = verb_of(values);
        if (verb == "new") out(item_browser::make_collection(rest));
        else if (verb == "delete") out(item_browser::delete_collection(rest));
        else if (verb == "add") {
            item_browser::pick_collection(rest);
            out("The highlighted item goes into " + rest + " (or out of it).");
        } else if (verb == "show") out(item_browser::show_collection(rest));
        else out(item_browser::list_collections());
    };
    registry.add(std::move(collections));

    auto layer_arguments = argument("verb_and_arguments", Type::text, true);
    layer_arguments.rest = true;
    layer_arguments.complete = [](const Model &, auto) {
        return std::vector<std::string>{"wear", "off", "clear", "collection", "status", "slots", "find"};
    };
    auto layers = action("layers", "Items worn on top of the outfit: wear <item>, off <item>, clear, collection <name|number> "
                         "(every item of it on top), status; slots and find <part> are research aids",
                         Group::console, {std::move(layer_arguments)});
    layers.run = [](const Model &, const Values &values, const Output &out) {
        const auto [verb, rest] = verb_of(values);
        if (verb == "wear") out(item_browser::wear_layer(rest, true));
        else if (verb == "off") out(item_browser::wear_layer(rest, false));
        else if (verb == "clear") out(item_browser::clear_layers());
        else if (verb == "collection") out(item_browser::wear_collection(rest));
        else if (verb == "slots") out(outfit_layers::slots());
        else if (verb == "find") out(outfit_layers::find(rest));
        else out(item_browser::list_collections());
    };
    registry.add(std::move(layers));
}
} // namespace dingosdk::console
