#include "Extension/Customization/board_material.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace {
using namespace dingosdk;
using namespace dingosdk::board_material;
namespace n = addr::native_cosmetics;

void check(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

// Memory as the game lays a board out, by pages; a read of a page never written fails.
struct Memory {
    std::map<std::uintptr_t, std::array<unsigned char, 4096>> pages;
    void put_bytes(std::uintptr_t at, const void* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i, ++at) pages[at & ~std::uintptr_t{4095}][at & 4095] = static_cast<const unsigned char*>(data)[i];
    }
    template <class T> void put(std::uintptr_t at, const T& value) { put_bytes(at, &value, sizeof(value)); }
    bool operator()(std::uintptr_t at, void* out, std::size_t size) const {
        for (std::size_t i = 0; i < size; ++i, ++at) {
            const auto page = pages.find(at & ~std::uintptr_t{4095});
            if (page == pages.end()) return false;
            static_cast<unsigned char*>(out)[i] = page->second[at & 4095];
        }
        return true;
    }
};

constexpr std::uintptr_t base = 0x140000000;
constexpr std::uint64_t number_key = 0x841c7d74837f1d39ULL, integer_key = 0xb00abec403499469ULL, switch_key = 0xa5f9a6b8098279adULL;

struct Board {
    Memory memory;
    std::uintptr_t colour_type{};
    std::uintptr_t entity = 0x10000000;
    std::uintptr_t next = 0x20000000; // where the next allocation goes
    std::uintptr_t grip{}, deck{}, trucks{};
    std::uintptr_t deck_item{};
    std::map<std::uint64_t, std::uintptr_t> grip_nodes;
    std::uintptr_t take(std::size_t size) {
        const auto at = next;
        next += (size + 0xfff) & ~std::size_t{0xfff};
        std::vector<unsigned char> zero(size);
        memory.put_bytes(at, zero.data(), zero.size());
        return at;
    }
    // A type object and its information, in the game's image as the real ones are.
    std::uintptr_t type(std::uintptr_t object, std::uint32_t hash, std::uint16_t size) {
        const auto info = object + 0x100;
        memory.put(base + object, base + info);
        memory.put(base + info, hash);
        memory.put<std::uint16_t>(base + info + 6, size);
        return base + object;
    }
    struct Value {
        std::uint64_t key;
        std::uintptr_t type;
        std::uint16_t size;
        std::array<std::uint32_t, 4> raw{};
        std::uint32_t flags = 3;
    };
    // What the engine's typed setter does with a key a material does not hold: a node of its own,
    // linked into the material's map (developer_board.cpp, the native set_shader_parameter).
    std::uintptr_t add(std::uintptr_t material, std::uint64_t key, std::uint16_t identity, const Colour& value) {
        std::uintptr_t buckets{}, node{};
        std::uint32_t count{};
        memory(material + n::material_parameters, &buckets, sizeof(buckets));
        memory(material + n::material_parameter_count, &count, sizeof(count));
        const auto added = take(0x48);
        memory.put(added, developer_hoodie_detail::ParameterKey{identity, 12, 1, 1, key});
        std::array<std::uint32_t, 4> raw{};
        std::memcpy(raw.data(), value.data(), sizeof(value));
        memory.put(added + n::parameter_value, raw);
        memory.put(added + n::parameter_type, colour_type);
        memory.put<std::uint32_t>(added + n::parameter_flags, 3);
        memory(buckets, &node, sizeof(node));
        memory.put(added + n::parameter_next, node); // first in its bucket
        memory.put(buckets, added);
        memory.put<std::uint32_t>(material + n::material_parameter_count, count + 1);
        return added;
    }
    std::uintptr_t material(const std::vector<Value>& values, std::map<std::uint64_t, std::uintptr_t>* nodes = nullptr) {
        const auto at = take(0x300), buckets = take(16);
        memory.put(at + n::material_parameters, buckets);
        memory.put<std::uint32_t>(at + n::material_parameter_buckets, 1);
        memory.put<std::uint32_t>(at + n::material_parameter_count, static_cast<std::uint32_t>(values.size()));
        memory.put<std::uintptr_t>(buckets + 8, 0xdead0000); // the map's end
        std::uintptr_t previous{};
        for (const auto& v : values) {
            const auto node = take(0x48);
            memory.put(node, developer_hoodie_detail::ParameterKey{0x1234, v.size, 1, 1, v.key});
            memory.put(node + n::parameter_value, v.raw);
            memory.put(node + n::parameter_type, v.type);
            memory.put(node + n::parameter_flags, v.flags);
            memory.put(previous ? previous + n::parameter_next : buckets, node);
            previous = node;
            if (nodes) (*nodes)[v.key] = node;
        }
        return at;
    }
    std::uintptr_t item(std::uintptr_t appearance, std::string_view slot, const std::vector<std::pair<std::string_view, std::uintptr_t>>& materials) {
        const auto at = take(0x200), name = take(256), buckets = take(16);
        memory.put_bytes(name, slot.data(), slot.size());
        memory.put(at + 0x30, name);
        memory.put(at + 0x48, appearance + 0x40);
        memory.put<std::uint32_t>(at + 0xb0, 2);
        memory.put(at + 0x1f0, buckets);
        memory.put<std::uint32_t>(at + 0x1f8, 1);
        memory.put<std::uint32_t>(at + 0x1fc, static_cast<std::uint32_t>(materials.size()));
        memory.put<std::uintptr_t>(buckets + 8, 0xdead0000);
        std::uintptr_t previous{};
        for (const auto& [material_name, material] : materials) {
            const auto node = take(0x30);
            memory.put(node, developer_hoodie_detail::material_hash(material_name));
            memory.put(node + 8, material);
            memory.put(previous ? previous + 0x28 : buckets, node);
            previous = node;
        }
        return at;
    }
    // `declared`: the deck as the Example declared-input deck's material holds it live: integers and switches only.
    explicit Board(bool declared = false) {
        const auto colour = type(0x7780c50, n::color_rgb_type_hash, 12), number = type(0x765db28, n::float_type_hash, 4),
                   integer = type(0x765da68, n::integer_type_hash, 4), toggle = type(0x7780f50, n::boolean_type_hash, 4);
        colour_type = colour;
        const auto bits = [](float a, float b = 0, float c = 0) {
            std::array<std::uint32_t, 4> raw{};
            const std::array<float, 3> f{a, b, c};
            std::memcpy(raw.data(), f.data(), sizeof(f));
            return raw;
        };
        grip = material({{graphic_colour, colour, 12, bits(.2f, .3f, .4f)},
                         {number_key, number, 4, bits(1)},
                         {integer_key, integer, 4, {13}},
                         {switch_key, toggle, 4, {1}},
                         {0x1111, colour, 12, bits(.5f, .5f, .5f), 0}}, // not live: bit 0 of its flags clear
                        &grip_nodes);
        if (declared) {
            std::vector<Value> selectors;
            for (std::uint64_t k = 1; k <= 7; ++k) selectors.push_back({0x5000 + k, k % 2 ? integer : toggle, 4, {static_cast<std::uint32_t>(k)}});
            deck = material(selectors);
        } else {
            deck = material({{base_colour, colour, 12, bits(.5f, .5f, .5f)}});
        }
        trucks = material({{base_colour, colour, 12, bits(.1f, .1f, .1f)}});
        const auto collection = take(0x80), cosmetic = take(0x200), appearance = take(0x100), list = take(24);
        memory.put(entity, base + addr::engine::board_entity_vtable);
        memory.put(entity + 0x70, collection);
        memory.put(collection, entity);
        memory.put<std::uint8_t>(collection + 8, 2);
        memory.put(collection + 0x20, cosmetic);
        memory.put(collection + 0x40, appearance);
        memory.put(cosmetic, base + n::cosmetic_component_vtable);
        memory.put(cosmetic + 0x18, collection);
        memory.put(appearance, base + addr::engine::skater_appearance_vtable);
        memory.put(appearance + 0x18, collection);
        deck_item = item(appearance, developer_board_detail::slots[0], {{"Deck_Mat", deck}, {"DeckTop_Mat", grip}});
        memory.put(list, deck_item);
        memory.put(list + 8, item(appearance, developer_board_detail::slots[1], {{"Truck_Mat", trucks}}));
        memory.put(appearance + 0x90, list);
        memory.put(appearance + 0x98, list + 16);
        memory.put(appearance + 0xa0, list + 24);
    }
    detail::Found walk() { return detail::walk(memory, base, entity); }
    Colour colour_at(std::uintptr_t node) {
        Colour c{};
        memory(node + n::parameter_value, c.data(), sizeof(c));
        return c;
    }
};

const detail::Material* part_of(const detail::Found& found, Part part) {
    for (const auto& m : found.materials)
        if (m.part == part) return &m;
    return nullptr;
}

void walks_the_board() {
    Board board;
    const auto found = board.walk();
    check(!found.pending && found.materials.size() == 3, "The board's three materials are found");
    const auto* grip = part_of(found, Part::grip);
    const auto* deck = part_of(found, Part::deck);
    check(grip && deck && part_of(found, Part::trucks) && !part_of(found, Part::wheels), "each as its part, and no wheels without their slot");
    check(grip->address == board.grip && grip->item == board.deck_item && grip->nodes.size() == 5, "The grip is the deck slot's DeckTop_Mat");
    const auto& p = grip->nodes;
    check(p[0].parameter.kind == Kind::colour && p[0].parameter.key == graphic_colour && p[0].parameter.value == Colour{.2f, .3f, .4f},
          "A colour reads as one");
    check(p[1].parameter.kind == Kind::number && p[1].parameter.value[0] == 1, "a number as one");
    check(p[2].parameter.kind == Kind::integer && p[2].parameter.integer == 13, "an integer as one");
    check(p[3].parameter.kind == Kind::toggle && p[3].parameter.integer == 1, "a switch as one");
    check(p[4].parameter.kind == Kind::other, "and a value not live is of no kind plugins set");
}

void refuses_what_picks_the_look() {
    Board board;
    const auto found = board.walk();
    Kind kind{};
    check(detail::refusal(found, Part::grip, graphic_colour, {1, 0, 0}, true, kind).empty() && kind == Kind::colour, "A colour may be held");
    check(detail::refusal(found, Part::grip, number_key, {-3, 0, 0}, false, kind).empty() && kind == Kind::number, "and a number");
    check(detail::refusal(found, Part::grip, integer_key, {13, 0, 0}, false, kind).find("only read") != std::string::npos,
          "An integer, which picks how the board is drawn, is not");
    check(detail::refusal(found, Part::grip, switch_key, {0, 0, 0}, false, kind).find("only read") != std::string::npos, "nor a switch");
    check(!detail::refusal(found, Part::grip, 0x1111, {.5f, .5f, .5f}, true, kind).empty(), "nor a value that is not live");
    check(!detail::refusal(found, Part::grip, graphic_colour, {1.5f, 0, 0}, true, kind).empty(), "A colour is 0 to 1");
    check(!detail::refusal(found, Part::grip, graphic_colour, {1, 0, 0}, false, kind).empty(), "and a number is not a colour");
    check(!detail::refusal(found, Part::grip, number_key, {1000, 0, 0}, false, kind).empty(), "A number has bounds");
    check(detail::refusal(found, Part::wheels, base_colour, {1, 0, 0}, true, kind).find("no parameter") != std::string::npos,
          "A part without the key says so");
}

void holds_and_puts_back() {
    Board board;
    std::vector<detail::Saved> saved;
    std::vector<detail::Hold> holds{{"glow", Part::grip, graphic_colour, Kind::colour, {1, 0, 0}}};
    int writes{}, publishes{};
    auto write = [&](std::uintptr_t material, std::uintptr_t node, Kind kind, const Colour& value) {
        ++writes;
        board.memory.put_bytes(node + n::parameter_value, value.data(), kind == Kind::colour ? sizeof(Colour) : sizeof(float));
        board.memory.put<std::uint8_t>(material + n::material_changed, 1);
        return true;
    };
    auto publish = [&](std::uintptr_t item) {
        check(item == board.deck_item, "The grip's item publishes its materials");
        ++publishes;
    };
    const auto tick = [&] {
        const auto found = board.walk();
        detail::apply(saved, holds, found, write, publish);
    };
    const auto node = board.grip_nodes[graphic_colour];
    tick();
    check(board.colour_at(node) == Colour{1, 0, 0} && writes == 1 && publishes == 1, "A held colour is written and published");
    std::uint8_t changed{};
    board.memory(board.grip + n::material_changed, &changed, 1);
    check(changed == 1, "and its material marked changed");
    for (int i = 0; i < 6; ++i) tick();
    check(writes == 1 && publishes == 1 + developer_hoodie_detail::settle_publishes, "A colour standing still is published until it shows, then left");
    holds[0].value = {0, 1, 0};
    tick();
    check(board.colour_at(node) == Colour{0, 1, 0} && writes == 2, "A new value is written");
    // The game sets its own while it is held: held again.
    board.memory.put(node + n::parameter_value, Colour{.7f, .7f, .7f});
    tick();
    check(board.colour_at(node) == Colour{0, 1, 0} && writes == 3, "What is held wins over a change");
    holds.clear();
    publishes = 0;
    for (int i = 0; i < 8; ++i) tick();
    check(board.colour_at(node) == Colour{.7f, .7f, .7f} && writes == 4, "Let go, it goes back to the board's own (the game's latest)");
    check(saved.empty() && publishes == 1 + developer_hoodie_detail::settle_publishes, "and is published until that shows");

    // A number, let go after the game changed it: left as the game has it.
    holds = {{"glow", Part::grip, number_key, Kind::number, {4, 0, 0}}};
    tick();
    float number{};
    board.memory(board.grip_nodes[number_key] + n::parameter_value, &number, 4);
    check(number == 4, "A held number is written");
    holds.clear();
    board.memory.put(board.grip_nodes[number_key] + n::parameter_value, 9.0f);
    tick();
    board.memory(board.grip_nodes[number_key] + n::parameter_value, &number, 4);
    check(number == 9 && saved.empty(), "and one the game changed is not put back");

    // A recipe on its way: nothing is written.
    holds = {{"glow", Part::grip, graphic_colour, Kind::colour, {0, 0, 1}}};
    auto found = board.walk();
    found.pending = true;
    const auto before = writes;
    detail::apply(saved, holds, found, write, publish);
    check(writes == before, "Nothing is written while the board is being dressed");
}

// ---- shader inputs a mod declares for its own deck (reskate-materials.json) ---------------------------------
// The Example declared-input deck's eight inputs (parameter-contract.json): colours its compiled shader reads,
// in no live map until something sets them.
struct Liquid { std::uint64_t key; std::uint16_t identity; };
constexpr Liquid liquid[] = {
    {6569911127076434714ULL, 46697}, {5869417437401782227ULL, 45276}, {6633899794922297581ULL, 59994}, {6507148046963062431ULL, 65135},
    {6339966355936362695ULL, 20838}, {6498503899632300675ULL, 22501}, {6196286651448922263ULL, 47378}, {6344408684715011858ULL, 17465}};
constexpr std::uint64_t declared_number = 0x7777777777777777ULL;

std::vector<mods::MaterialDeclaration> declared_declarations() {
    mods::MaterialDeclaration d{"Example", "deck", {"Example_Deck"}, {}};
    for (const auto& l : liquid) d.parameters.push_back({l.key, l.identity, true, {.5f, .5f, .5f}});
    d.parameters.push_back({declared_number, 0x4242, false, {.25f, 0, 0}});
    return {d};
}
auto never_sold = [](std::string_view) { return false; };
detail::Found declared_found(Board& board) {
    auto found = board.walk();
    found.declared = detail::declared_for(declared_declarations(), Part::deck, "example_deck", never_sold, "Example");
    return found;
}

void declarations_follow_the_equipped_item() {
    const auto all = declared_declarations();
    const auto deck = detail::declared_for(all, Part::deck, "EXAMPLE_DECK", never_sold, "Example");
    check(deck.size() == 9 && deck[0].key == liquid[0].key && deck[0].identity == liquid[0].identity && deck[0].kind == Kind::colour &&
              deck[0].fallback == Colour{.5f, .5f, .5f} && deck[8].kind == Kind::number,
          "The equipped item's declarations apply (its key in any case)");
    check(detail::declared_for(all, Part::deck, "Some_Other_Deck", never_sold, "Example").empty(), "another deck's do not");
    check(detail::declared_for(all, Part::grip, "Example_Deck", never_sold, "Example").empty(), "nor another part's");
    check(detail::declared_for(all, Part::deck, "", never_sold, "Example").empty(), "nor an empty slot's");
    auto sold = [](std::string_view) { return true; };
    check(detail::declared_for(all, Part::deck, "Example_Deck", sold, "Example").empty(), "An item the game sells (or one it cannot tell) takes none");
    // Two mods giving one key differently: which shader is right cannot be told, so neither.
    auto both = all;
    both.push_back({"Other", "deck", {"Example_Deck"}, {{liquid[0].key, 1, true, {0, 0, 0}}, {liquid[1].key, liquid[1].identity, true, {.5f, .5f, .5f}}}});
    check(detail::declared_for(all,Part::deck,"Example_Deck",never_sold,"Other").empty(),"Other mod cannot declare this item");
    check(detail::declared_for(all,Part::deck,"Example_Deck",never_sold,"").empty(),"Unknown ownership is refused");
    const auto merged = detail::declared_for(both, Part::deck, "Example_Deck", never_sold, "Example");
    check(std::any_of(merged.begin(), merged.end(), [](const detail::Declared& d) { return d.key == liquid[0].key; }) &&
              std::count_if(merged.begin(), merged.end(), [](const detail::Declared& d) { return d.key == liquid[1].key; }) == 1,
          "Another mod cannot suppress the owner's declarations");
}

void lists_and_refuses_declared() {
    Board board(true);
    auto found = declared_found(board);
    const auto listed = detail::listed(found, Part::deck);
    check(listed.size() == 7 + 9, "The deck lists its seven live selectors and its nine declared inputs");
    check(std::all_of(listed.begin(), listed.begin() + 7, [](const Parameter& p) { return !p.inherited && (p.kind == Kind::integer || p.kind == Kind::toggle); }),
          "the selectors as the material holds them");
    check(std::all_of(listed.begin() + 7, listed.end(), [](const Parameter& p) { return p.inherited; }) && listed[7].value == Colour{.5f, .5f, .5f},
          "the declared ones inherited, at their default");
    check(detail::listed(board.walk(), Part::deck).size() == 7, "Without the declaration only the live ones show");

    Kind kind{};
    check(detail::refusal(found, Part::deck, liquid[0].key, {.1f, .2f, .3f}, true, kind).empty() && kind == Kind::colour,
          "A declared colour the material does not hold may be held");
    check(detail::refusal(board.walk(), Part::deck, liquid[0].key, {.1f, .2f, .3f}, true, kind).find("no parameter") != std::string::npos,
          "but not on a deck that does not declare it");
    check(!detail::refusal(found, Part::deck, liquid[0].key, {.1f, .2f, .3f}, false, kind).empty(), "A declared colour is not a number");
    check(!detail::refusal(found, Part::deck, liquid[0].key, {1.5f, 0, 0}, true, kind).empty(), "and its channels are 0 to 1");
    check(detail::refusal(found, Part::deck, declared_number, {.5f, 0, 0}, false, kind).find("only a declared colour") != std::string::npos,
          "A declared number with no live value cannot be added (its type is not known to this build)");
    check(detail::refusal(found, Part::deck, 0x5001, {1, 0, 0}, false, kind).find("only read") != std::string::npos,
          "A live integer stays read-only");
    // A declaration naming a key the material holds as an integer or a switch changes nothing.
    auto clash = found;
    clash.declared.push_back({Part::deck, 0x5002, 0x1234, Kind::colour, {0, 0, 0}});
    check(detail::refusal(clash, Part::deck, 0x5002, {1, 0, 0}, true, kind).find("not as its mod declares") != std::string::npos,
          "A switch a mod declares as a colour is refused");
    // A live colour whose identity is not the declared one is not the declared parameter.
    Board ordinary;
    auto other = ordinary.walk();
    other.declared.push_back({Part::grip, graphic_colour, 0x9999, Kind::colour, {.5f, .5f, .5f}});
    check(detail::refusal(other, Part::grip, graphic_colour, {1, 0, 0}, true, kind).find("not as its mod declares") != std::string::npos,
          "and so is a live colour of another identity");
}

void owners_and_limits() {
    Board board(true);
    const auto found = declared_found(board);
    std::vector<detail::Hold> holds;
    check(detail::take(holds, found, "liquid", Part::deck, liquid[0].key, {.1f, .2f, .3f}, true).empty() && holds.size() == 1 &&
              holds[0].declared && holds[0].kind == Kind::colour,
          "A plugin takes a declared colour");
    check(detail::take(holds, found, "other", Part::deck, liquid[0].key, {1, 1, 1}, true).find("liquid holds") != std::string::npos,
          "another plugin cannot take it from it");
    check(detail::take(holds, found, "liquid", Part::deck, liquid[0].key, {.4f, .5f, .6f}, true).empty() && holds.size() == 1 &&
              holds[0].value == Colour{.4f, .5f, .6f},
          "its owner sets it again");
    check(!detail::take(holds, found, "liquid", Part::deck, 0x5001, {1, 0, 0}, false).empty() && holds.size() == 1, "A refusal holds nothing");
    for (std::size_t i = 1; i < most_held; ++i) holds.push_back({"liquid", Part::grip, 0x9000 + i, Kind::colour, {}, false});
    check(detail::take(holds, found, "liquid", Part::deck, liquid[1].key, {0, 0, 0}, true).find("at most") != std::string::npos,
          "A plugin holds at most 32, declared ones included");
    check(detail::take(holds, found, "other", Part::deck, liquid[1].key, {0, 0, 0}, true).empty(), "another has its own 32");
    detail::release(holds, "liquid", Part::grip, std::nullopt);
    check(holds.size() == 2, "Letting go of a part lets go of the plugin's holds there only");
    detail::release(holds, "liquid", std::nullopt, std::nullopt);
    check(holds.size() == 1 && holds[0].owner == "other", "and letting go of all (an unload) leaves the others' alone");
}

void adds_and_puts_back_declared() {
    Board board(true);
    std::vector<detail::Saved> saved;
    std::vector<detail::Hold> holds;
    int writes{}, publishes{}, inserts{};
    std::map<std::uint64_t, std::uintptr_t> added;
    auto write = [&](std::uintptr_t material, std::uintptr_t node, Kind kind, const Colour& value) {
        check(material == board.deck, "Only the deck is written");
        ++writes;
        board.memory.put_bytes(node + n::parameter_value, value.data(), kind == Kind::colour ? sizeof(Colour) : sizeof(float));
        return true;
    };
    auto publish = [&](std::uintptr_t item) {
        check(item == board.deck_item, "The deck's item publishes");
        ++publishes;
    };
    auto insert = [&](std::uintptr_t material, const detail::Declared& d, const Colour& value) {
        check(material == board.deck && d.part == Part::deck && d.kind == Kind::colour, "A declared colour is added to the deck's material");
        ++inserts;
        added[d.key] = board.add(material, d.key, d.identity, value);
        return true;
    };
    const auto found = [&] { return declared_found(board); };
    const auto tick = [&] { detail::apply(saved, holds, found(), write, publish, insert); };
    // The liquid controller's eight colours, all held at once.
    for (std::size_t i = 0; i < 8; ++i)
        check(detail::take(holds, found(), "liquid", Part::deck, liquid[i].key, {.1f * i, .2f, .3f}, true).empty(), "each liquid colour is taken");
    tick();
    check(inserts == 8 && writes == 0 && publishes == 1, "The eight are added by the engine's setter, and the deck published once");
    for (std::size_t i = 0; i < 8; ++i) check(board.colour_at(added[liquid[i].key]) == Colour{.1f * i, .2f, .3f}, "at the held value");
    const auto walked = board.walk();
    const auto* deck = part_of(walked, Part::deck);
    check(deck && deck->nodes.size() == 15 && std::count_if(deck->nodes.begin(), deck->nodes.end(), [&](const detail::Node& node) {
              return node.parameter.kind == Kind::colour && node.identity == liquid[0].identity;
          }) == 1,
          "and then the material holds them live, with their identities");
    for (int i = 0; i < 6; ++i) tick();
    check(inserts == 8 && writes == 0, "Once added they are not added again");
    // About 30 times a second, the controller's new values: written in place.
    holds[0].value = {.9f, .8f, .7f};
    tick();
    check(inserts == 8 && writes == 1 && board.colour_at(added[liquid[0].key]) == Colour{.9f, .8f, .7f}, "A new value is written in place");
    // Let go (clear_parameter, or the plugin unloading): each goes back to its declared default.
    detail::release(holds, "liquid", std::nullopt, std::nullopt);
    for (int i = 0; i < 8; ++i) tick();
    for (std::size_t i = 0; i < 8; ++i)
        check(board.colour_at(added[liquid[i].key]) == Colour{.5f, .5f, .5f}, "Let go, a declared colour goes back to its shader's default");
    check(saved.empty(), "and nothing is kept once that shows");
    const auto after = board.walk();
    check(part_of(after, Part::deck)->nodes.size() == 15, "What was added stays (the renderer copies it), at its default");
    check(std::all_of(part_of(after, Part::deck)->nodes.begin(), part_of(after, Part::deck)->nodes.begin() + 15, [](const detail::Node& node) {
              return node.parameter.kind != Kind::integer || node.parameter.integer != 0;
          }),
          "and the selectors were never touched");
}

void a_change_of_deck_lets_go() {
    Board board(true);
    std::vector<detail::Saved> saved;
    std::vector<detail::Hold> holds;
    std::uintptr_t node{};
    int inserts{};
    auto write = [&](std::uintptr_t, std::uintptr_t at, Kind, const Colour& value) {
        board.memory.put_bytes(at + n::parameter_value, value.data(), sizeof(Colour));
        return true;
    };
    auto publish = [](std::uintptr_t) {};
    auto insert = [&](std::uintptr_t material, const detail::Declared& d, const Colour& value) {
        ++inserts;
        node = board.add(material, d.key, d.identity, value);
        return true;
    };
    check(detail::take(holds, declared_found(board), "liquid", Part::deck, liquid[0].key, {1, 0, 0}, true).empty(), "taken");
    detail::apply(saved, holds, declared_found(board), write, publish, insert);
    check(inserts == 1 && board.colour_at(node) == Colour{1, 0, 0}, "added on the declared-input deck");
    // The deck is changed for one that declares nothing: the walk finds no declaration.
    auto changed = board.walk();
    check(detail::drop_undeclared(holds, changed) == 1 && holds.empty(), "The hold is let go when its deck no longer declares it");
    for (int i = 0; i < 8; ++i) detail::apply(saved, holds, changed, write, publish, insert);
    check(inserts == 1 && board.colour_at(node) == Colour{.5f, .5f, .5f} && saved.empty(), "what it added goes back, and nothing more is added");
    // A hold kept (taken while declared) is never written to a deck that does not declare it.
    holds = {{"liquid", Part::deck, liquid[1].key, Kind::colour, {0, 1, 0}, true}};
    detail::apply(saved, holds, changed, write, publish, insert);
    check(inserts == 1 && saved.empty(), "A declared hold is not carried onto an incompatible deck");
    // A board being dressed: nothing is let go or written while the recipe is on its way.
    changed.declarations_pending[0]=true;
    check(detail::drop_undeclared(holds,changed)==0 && holds.size()==1,"Rebuilding materials retain declared holds while settling");
    changed.pending = true;
    check(detail::drop_undeclared(holds, changed) == 0 && holds.size() == 1, "nor let go while the board is being dressed");
    // Back on the declared-input deck it applies again.
    detail::apply(saved, holds, declared_found(board), write, publish, insert);
    check(inserts == 2, "and is added again once the declaring deck is back");
}

void declarations_wait_for_the_board_to_settle() {
    detail::Gate gate;
    check(!detail::settled(gate, "Example_Deck", 0x1000, 0), "A newly equipped item does not apply at once");
    check(!detail::settled(gate, "Example_Deck", 0x1000, detail::settle_ms - 1), "nor before it has settled");
    check(detail::settled(gate, "Example_Deck", 0x1000, detail::settle_ms), "then it does");
    check(!detail::settled(gate, "Example_Deck", 0x2000, detail::settle_ms + 5), "A new material under it starts again");
    check(!detail::settled(gate, "Other_Deck", 0x2000, 2 * detail::settle_ms + 5), "as does another item");
    check(!detail::settled(gate, "Other_Deck", 0, 9 * detail::settle_ms), "and no material never settles");
}
} // namespace

int main() {
    const std::map<std::string, int> stock{{"free_stock", 1}, {"paid_stock", 1}};
    const auto not_reserved = [](std::string_view) { return false; };
    check(detail::declared_item_restricted("FREE_STOCK", true, stock, not_reserved), "free stock cannot grant declarations");
    check(detail::declared_item_restricted("paid_stock", true, stock, not_reserved), "paid stock cannot grant declarations");
    check(detail::declared_item_restricted("OWN_DEV_TEST", true, stock, not_reserved), "developer items cannot grant declarations");
    check(detail::declared_item_restricted("own_private", false, stock, not_reserved), "unknown stock catalogue state fails closed");
    check(!detail::declared_item_restricted("own_private", true, stock, not_reserved), "a non-stock private item can proceed to ownership checks");
    check(detail::declared_item_restricted("own_reserved", true, stock, [](std::string_view) { return true; }), "reservation policy is still applied");
    walks_the_board();
    refuses_what_picks_the_look();
    holds_and_puts_back();
    declarations_follow_the_equipped_item();
    lists_and_refuses_declared();
    owners_and_limits();
    adds_and_puts_back_declared();
    a_change_of_deck_lets_go();
    declarations_wait_for_the_board_to_settle();
    std::cout << "board material tests passed\n";
}
