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
    Board() {
        const auto colour = type(0x7780c50, n::color_rgb_type_hash, 12), number = type(0x765db28, n::float_type_hash, 4),
                   integer = type(0x765da68, n::integer_type_hash, 4), toggle = type(0x7780f50, n::boolean_type_hash, 4);
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
        deck = material({{base_colour, colour, 12, bits(.5f, .5f, .5f)}});
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
} // namespace

int main() {
    walks_the_board();
    refuses_what_picks_the_look();
    holds_and_puts_back();
    std::cout << "board material tests passed\n";
}
