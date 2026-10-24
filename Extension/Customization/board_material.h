#pragma once
#include "developer_board_material.h"
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The shader parameters of the local player's own board as plugins read and set them
// (reskate.board.parameters): each part's material instance holds a few values of its own (read
// from the running game, 2026-10-22: the deck and its grip eight each, the trucks and wheels three),
// which the renderer takes for that board alone. A plugin may hold a colour or a number there; an
// integer or a switch picks how the board is drawn (the foil looks the store sells among them), so
// those are only read. Nothing of the board's items changes and nobody else sees it: other players
// draw this board from its cosmetics, as they are.
namespace dingosdk::board_material {
enum class Part : std::uint8_t { deck, grip, trucks, wheels };
inline constexpr std::array<std::string_view, 4> part_names{"deck", "grip", "trucks", "wheels"};
enum class Kind : std::uint8_t { colour, number, integer, toggle, other };
using Colour = std::array<float, 3>;
struct Parameter {
    std::uint64_t key{};
    Kind kind{Kind::other};
    Colour value{};         // a colour, or a number in [0]
    std::int32_t integer{}; // an integer, or a switch's 0 or 1
};
// The two colours known by what they colour (the developer board animates them): the graphic's and
// the base's.
inline constexpr std::uint64_t graphic_colour = developer_board_detail::graphic_color;
inline constexpr std::uint64_t base_colour = developer_board_detail::base_color;
// What a plugin may set: a colour of 0 to 1 a channel, as the board's own are, or a number.
inline constexpr float most_number = 100.0f;
inline constexpr std::size_t most_held = 32;

// Client thread. The part's parameters as its material holds them now; none without a board.
std::optional<std::vector<Parameter>> read(Part part);
// Held by `owner` and written every tick until cleared: a colour (all three), or a number
// (`value[0]`, `colour` false). Empty when held, else why not.
std::string set(std::string_view owner, Part part, std::uint64_t key, const Colour& value, bool colour);
// What `owner` holds of that part and key (all its parts, all its keys) put back.
void clear(std::string_view owner, std::optional<Part> part = std::nullopt, std::optional<std::uint64_t> key = std::nullopt);
// Each client tick while the skater is in play: what is held written, what is let go put back.
void tick(std::uintptr_t base, std::uintptr_t client) noexcept;

namespace detail {
struct Node {
    std::uintptr_t address{};
    Parameter parameter;
};
struct Material {
    std::uintptr_t item{}, address{};
    Part part{};
    std::vector<Node> nodes;
};
struct Found {
    std::vector<Material> materials;
    bool pending{}; // a recipe on its way: nothing is written
};
// Each part's named material on the board's slot controllers; the deck slot holds two.
struct Target {
    std::string_view material;
    std::size_t slot;
    Part part;
};
inline constexpr std::array<Target, 4> targets{{
    {"Deck_Mat", 0, Part::deck},
    {"DeckTop_Mat", 0, Part::grip},
    {"Truck_Mat", 1, Part::trucks},
    {"Wheel_mat", 2, Part::wheels},
}};
inline constexpr std::size_t most_nodes = 512;

inline Kind kind_of(std::uint32_t type_hash, std::uint16_t type_size, const developer_hoodie_detail::ParameterKey& key, bool live) noexcept {
    namespace n = addr::native_cosmetics;
    if (!live || key.count != 1 || key.size != type_size) return Kind::other;
    if (type_hash == n::color_rgb_type_hash && type_size == sizeof(Colour)) return Kind::colour;
    if (type_hash == n::float_type_hash && type_size == 4) return Kind::number;
    if (type_hash == n::integer_type_hash && type_size == 4) return Kind::integer;
    if (type_hash == n::boolean_type_hash && type_size == 4) return Kind::toggle;
    return Kind::other;
}
template <class Memory> std::vector<Node> parameters(Memory& m, std::uintptr_t material) {
    namespace n = addr::native_cosmetics;
    const auto buckets = m.ptr(material, n::material_parameters);
    const auto size = m.template get<std::uint32_t>(material, n::material_parameter_buckets);
    const auto total = m.template get<std::uint32_t>(material, n::material_parameter_count);
    std::vector<Node> out;
    if (!buckets || !size) return out;
    m.check(size <= 1024 && total <= most_nodes, "Board parameter map exceeds bounds.");
    const auto sentinel = m.ptr(buckets, size * 8ULL);
    for (std::uint32_t b = 0; b < size; ++b) {
        std::uint32_t visited{};
        for (auto node = m.ptr(buckets, b * 8ULL); node && node != sentinel; node = m.ptr(node, n::parameter_next)) {
            m.check(++visited <= total && out.size() < total, "Board parameter chain exceeds bounds.");
            const auto key = m.template get<developer_hoodie_detail::ParameterKey>(node);
            const auto raw = m.template get<std::array<std::uint32_t, 4>>(node, n::parameter_value);
            const auto type = m.ptr(node, n::parameter_type);
            Parameter p;
            p.key = key.hash;
            if (type) {
                const auto info = m.ptr(type);
                // A type's information is in the game's image, as the board's colour descriptor is.
                if (info >= m.base && info < m.base + 0x09144000)
                    p.kind = kind_of(m.template get<std::uint32_t>(info), m.template get<std::uint16_t>(info, 6), key,
                                     (m.template get<std::uint32_t>(node, n::parameter_flags) & 1) != 0);
            }
            if (p.kind == Kind::colour || p.kind == Kind::number) {
                std::memcpy(p.value.data(), raw.data(), p.kind == Kind::colour ? sizeof(Colour) : sizeof(float));
                m.check(std::all_of(p.value.begin(), p.value.end(), [](float v) { return std::isfinite(v); }), "Board parameter is invalid.");
            } else if (p.kind == Kind::integer) {
                p.integer = static_cast<std::int32_t>(raw[0]);
            } else if (p.kind == Kind::toggle) {
                p.integer = raw[0] != 0;
            }
            out.push_back({node, p});
        }
    }
    return out;
}
// The board actor's parts (developer_board_detail::materials walks the same way to its colours).
template <class Read> Found walk(Read& read, std::uintptr_t base, std::uintptr_t entity) {
    using namespace multiplayer;
    CosmeticMemory<Read&> m{read, base};
    Found out;
    m.check(m.ptr(entity) == base + addr::engine::board_entity_vtable, "Board actor type differs.");
    const auto component = m.component(entity);
    out.pending = m.template get<std::uint8_t>(component, 0x10a) || m.template get<std::uint8_t>(component, 0x10d);
    const auto appearance = read_native_component(read, entity, base + addr::engine::skater_appearance_vtable);
    const auto begin = m.ptr(appearance, 0x90), end = m.ptr(appearance, 0x98), capacity = m.ptr(appearance, 0xa0);
    m.check(begin <= end && end <= capacity && (end - begin) % 8 == 0 && (capacity - begin) / 8 <= 64 && (begin || !capacity),
            "Board controller array exceeds bounds.");
    const auto& slots = developer_board_detail::slots;
    std::array<bool, developer_board_detail::slots.size()> seen{};
    for (auto entry = begin; entry < end; entry += 8) {
        const auto item = m.ptr(entry);
        if (!item) continue;
        m.check(m.ptr(item, 0x48) == appearance + 0x40, "Board controller ownership differs.");
        const auto assigned = std::find(slots.begin(), slots.end(), m.text(m.ptr(item, 0x30)));
        if (assigned == slots.end()) continue;
        const auto slot = static_cast<std::size_t>(assigned - slots.begin());
        m.check(!seen[slot], "Board has duplicate slot controllers.");
        seen[slot] = true;
        if (!(m.template get<std::uint32_t>(item, 0xb0) & 2)) { out.pending = true; continue; }
        const auto buckets = m.ptr(item, 0x1f0);
        const auto size = m.template get<std::uint32_t>(item, 0x1f8), total = m.template get<std::uint32_t>(item, 0x1fc);
        if (!total) { out.pending = true; continue; }
        m.check(buckets && size && size <= 256 && total <= 128, "Board material map exceeds bounds.");
        const auto sentinel = m.ptr(buckets, size * 8ULL);
        for (const auto& target : targets) {
            if (target.slot != slot) continue;
            const auto hash = developer_hoodie_detail::material_hash(target.material);
            auto node = m.ptr(buckets, (hash % size) * 8ULL);
            std::uint32_t visited{};
            for (; node && node != sentinel && visited < total; ++visited, node = m.ptr(node, 0x28))
                if (m.template get<std::uint32_t>(node) == hash) break;
            if (!node || node == sentinel || visited == total) continue;
            std::array<std::uintptr_t, 4> variants{};
            for (std::size_t v = 0; v < variants.size(); ++v) {
                const auto material = m.ptr(node, 8 + v * 8);
                if (!material || std::find(variants.begin(), variants.begin() + v, material) != variants.begin() + v) continue;
                variants[v] = material;
                out.materials.push_back({item, material, target.part, parameters(m, material)});
            }
        }
    }
    return out;
}

inline const char* kind_name(Kind kind) noexcept {
    switch (kind) {
    case Kind::colour: return "a colour";
    case Kind::number: return "a number";
    case Kind::integer: return "an integer";
    case Kind::toggle: return "a switch";
    default: return "of a kind plugins do not set";
    }
}
// Whether `value` may be held on that key of the part: empty, and its kind in `kind`, else why not.
inline std::string refusal(const Found& found, Part part, std::uint64_t key, const Colour& value, bool colour, Kind& kind) {
    const auto name = part_names[static_cast<std::size_t>(part)];
    std::optional<Kind> has;
    for (const auto& material : found.materials)
        if (material.part == part)
            for (const auto& node : material.nodes)
                if (node.parameter.key == key) has = node.parameter.kind;
    if (!has) return std::format("the {} has no parameter {:016x}", name, key);
    if (*has != Kind::colour && *has != Kind::number)
        return std::format("{:016x} is {}, which picks how the board is drawn; plugins only read it", key, kind_name(*has));
    if ((*has == Kind::colour) != colour) return std::format("{:016x} is {}", key, kind_name(*has));
    if (colour && !std::all_of(value.begin(), value.end(), [](float c) { return c >= 0 && c <= 1; }))
        return "a colour's channels are 0 to 1";
    if (!colour && !(std::abs(value[0]) <= most_number)) return std::format("a number is -{0} to {0}", most_number);
    kind = *has;
    return {};
}
// A value a plugin holds, and what was found where it was written.
struct Hold {
    std::string owner;
    Part part{};
    std::uint64_t key{};
    Kind kind{};
    Colour value{};
};
struct Saved {
    std::uintptr_t material{}, node{};
    Kind kind{};
    Colour original{}, last{};
    std::uint8_t settling{}; // publishes still owed for `last`
};
inline bool same(Kind kind, const Colour& a, const Colour& b) noexcept {
    return kind == Kind::colour ? a == b : a[0] == b[0];
}
// One tick, as the developer board's colours go (developer_hoodie_detail::animate): what is held
// written where it is not yet, and published until it shows (the renderer shows a value one publish
// late); what is no longer held put back while it is still ours, a value changed by the game since
// left as the game has it. write(material, node, kind, value) -> written; publish(item).
template <class Write, class Publish>
void apply(std::vector<Saved>& saved, const std::vector<Hold>& holds, const Found& live, Write& write, Publish& publish) {
    if (live.pending) return;
    using developer_hoodie_detail::settle_publishes;
    std::vector<Saved> next;
    std::vector<std::uintptr_t> shown;
    for (const auto& material : live.materials) {
        bool show{};
        for (const auto& node : material.nodes) {
            const auto& p = node.parameter;
            if (p.kind != Kind::colour && p.kind != Kind::number) continue;
            const auto hold = std::find_if(holds.begin(), holds.end(), [&](const Hold& h) {
                return h.part == material.part && h.key == p.key && h.kind == p.kind;
            });
            const auto at = std::find_if(saved.begin(), saved.end(), [&](const Saved& s) {
                return s.material == material.address && s.node == node.address && s.kind == p.kind;
            });
            const Saved* was = at == saved.end() ? nullptr : &*at;
            const bool ours = was && same(p.kind, p.value, was->last);
            const auto original = ours ? was->original : p.value;
            if (hold != holds.end()) {
                if (same(p.kind, p.value, hold->value)) {
                    const std::uint8_t left = ours ? was->settling : 0;
                    next.push_back({material.address, node.address, p.kind, original, hold->value, static_cast<std::uint8_t>(left ? left - 1 : 0)});
                    show = show || left != 0;
                } else if (write(material.address, node.address, p.kind, hold->value)) {
                    next.push_back({material.address, node.address, p.kind, original, hold->value, settle_publishes});
                    show = true;
                } else if (was) {
                    next.push_back(*was);
                }
            } else if (was && (ours || same(p.kind, p.value, was->original))) {
                // Back to the board's own value, and published until it shows.
                const bool first = !same(p.kind, was->last, was->original);
                const bool back = same(p.kind, p.value, was->original) || write(material.address, node.address, p.kind, was->original);
                const std::uint8_t left = first ? settle_publishes : was->settling ? was->settling - 1 : 0;
                if (!back) next.push_back(*was);
                else if (left) next.push_back({material.address, node.address, p.kind, was->original, was->original, left});
                show = show || back;
            }
        }
        if (show && std::find(shown.begin(), shown.end(), material.item) == shown.end()) shown.push_back(material.item);
    }
    saved = std::move(next);
    for (const auto item : shown) publish(item);
}
} // namespace detail
} // namespace dingosdk::board_material
