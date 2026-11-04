#pragma once
#include "developer_hoodie_material.h"
#include "Engine/Game/Build/supported_build.h"
#include <cstddef>

namespace dingosdk {
namespace developer_board_detail {
using developer_hoodie_detail::Binding;
using developer_hoodie_detail::Color;
using developer_hoodie_detail::ItemAnimation;
using developer_hoodie_detail::ParameterKey;
using developer_hoodie_detail::animate;
using developer_hoodie_detail::item_animation;
using developer_hoodie_detail::item_color;
using developer_hoodie_detail::material_hash;
using developer_hoodie_detail::settle_publishes;
inline constexpr std::uint64_t graphic_color = 0x873f48af36e50d59ULL;
inline constexpr std::uint64_t base_color = 0x8c46cd354b18a9e2ULL;
inline constexpr Color shader_default{.5f, .5f, .5f};
inline constexpr std::array<std::string_view, 3> slots{
    "Characters/Customization/Board/Slots/Skateboard_Deck_Slot",
    "Characters/Customization/Board/Slots/Skateboard_Trucks_Slot",
    "Characters/Customization/Board/Slots/Skateboard_Wheels_Slot"};
// One colour of the board: a named material of the controller assigned one of `slots`, the
// shader key, and which of the board's marked cosmetics it colours (deck, grip tape, trucks,
// wheels: the order of their styles after the skater's, multiplayer::mark_item_names).
struct Target {
    std::string_view material;
    std::size_t controller;
    std::uint64_t key;
    std::uint8_t part;
};
inline constexpr std::size_t part_count = multiplayer::mark_items - multiplayer::skater_mark_items;
// The board loader registers Deck_Mat / DeckTop_Mat for the compiled sections
// Skateboard_base_mat / mat_skateboard_top, whatever deck is on it: use the live named map
// identities. The deck's wood follows its graphic.
inline constexpr std::array<Target, 5> targets{{
    {"Deck_Mat", 0, graphic_color, 0},
    {"Deck_Mat", 0, base_color, 0},
    {"DeckTop_Mat", 0, graphic_color, 1},
    {"Truck_Mat", 1, base_color, 2},
    {"Wheel_mat", 2, base_color, 3},
}};
// The inline NativeValue layout used by addEsVector's setter. Its type is the live ColorRgb
// descriptor, not a serialized Vec3 or boxed value.
struct NativeColor {
    Color color{};
    std::uint32_t padding{};
    std::uintptr_t type{};
    std::uint32_t flags{3}, reserved{};
};
static_assert(sizeof(NativeColor) == 32 && offsetof(NativeColor, type) == 16 && offsetof(NativeColor, flags) == 24);
inline ParameterKey parameter_key(std::uint64_t hash, std::uint16_t type) noexcept {
    return {static_cast<std::uint16_t>(hash == graphic_color ? 0xd442 : 0x8c54), sizeof(Color), type, 1, hash};
}
inline constexpr std::size_t max_bindings = targets.size() * 4;
using developer_hoodie_detail::Materials;
template<class Memory>
std::uintptr_t parameter(Memory &m, std::uintptr_t material, std::uint64_t key) {
    const auto buckets = m.ptr(material, 0x268);
    const auto size = m.template get<std::uint32_t>(material, 0x270);
    const auto total = m.template get<std::uint32_t>(material, 0x274);
    // The native setter also uses these buckets when inserting a missing color.
    m.check(buckets && size && size <= 1024 && total <= 512, "Board parameter map exceeds bounds.");
    const auto sentinel = m.ptr(buckets, size * 8ULL);
    auto node = m.ptr(buckets, (key % size) * 8ULL);
    std::uint32_t visited{};
    for (; node && node != sentinel && visited < total; ++visited, node = m.ptr(node, 0x40))
        if (m.template get<std::uint64_t>(node, 8) == key) return node;
    m.check(!node || node == sentinel, "Board parameter chain exceeds bounds.");
    return 0;
}
template<class Read> Materials materials(Read &read, std::uintptr_t base, std::uintptr_t entity) {
    using namespace multiplayer;
    CosmeticMemory<Read &> m{read, base};
    Materials out;
    m.check(m.ptr(entity) == base + addr::engine::board_entity_vtable, "Board actor type differs.");
    out.component = m.component(entity);
    out.pending = m.template get<std::uint8_t>(out.component, 0x10a) || m.template get<std::uint8_t>(out.component, 0x10d);
    const auto type = base + addr::native_cosmetics::color_rgb_type;
    const auto info = m.ptr(type);
    m.check(info >= base && info < base + supported_build::game_image_size && m.template get<std::uint32_t>(info) == 0x885eff59 &&
                m.template get<std::uint16_t>(info, 6) == sizeof(Color), "Board ColorRgb descriptor differs.");
    const auto type_index = m.template get<std::uint16_t>(type, 8);
    out.appearance = read_native_component(read, entity, base + addr::engine::skater_appearance_vtable);
    const auto begin = m.ptr(out.appearance, 0x90), end = m.ptr(out.appearance, 0x98), capacity = m.ptr(out.appearance, 0xa0);
    m.check(begin <= end && end <= capacity && (end - begin) % 8 == 0 && (capacity - begin) % 8 == 0 &&
                (capacity - begin) / 8 <= 64 && (begin || !capacity), "Board controller array exceeds bounds.");
    std::array<bool, slots.size()> seen{};
    for (auto entry = begin; entry < end; entry += 8) {
        const auto item = m.ptr(entry);
        if (!item) continue;
        m.check(m.ptr(item, 0x48) == out.appearance + 0x40, "Board controller ownership differs.");
        const auto name = m.text(m.ptr(item, 0x30));
        const auto assigned = std::find(slots.begin(), slots.end(), name);
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
        for (const auto &target : targets) {
            if (target.controller != slot) continue;
            const auto hash = material_hash(target.material);
            auto node = m.ptr(buckets, (hash % size) * 8ULL);
            std::uint32_t visited{};
            for (; node && node != sentinel && visited < total; ++visited, node = m.ptr(node, 0x28))
                if (m.template get<std::uint32_t>(node) == hash) break;
            m.check(!node || node == sentinel || visited < total, "Board material chain exceeds bounds.");
            if (!node || node == sentinel) continue;
            std::array<std::uintptr_t, 4> variants{};
            for (std::size_t v = 0; v < variants.size(); ++v) {
                const auto material = m.ptr(node, 8 + v * 8);
                if (!material || std::find(variants.begin(), variants.begin() + v, material) != variants.begin() + v) continue;
                variants[v] = material;
                const auto p = parameter(m, material, target.key);
                const auto key = parameter_key(target.key, type_index);
                Color color = shader_default;
                if (p) {
                    const auto native_key = m.template get<ParameterKey>(p);
                    m.check(native_key.identity == key.identity && native_key.size == key.size && native_key.type == key.type &&
                                native_key.count == key.count && m.ptr(p, 0x20) == type &&
                                (m.template get<std::uint32_t>(p, 0x28) & 1), "Board color parameter type differs.");
                    color = m.template get<Color>(p, 0x10);
                    m.check(std::all_of(color.begin(), color.end(), [](float c) { return std::isfinite(c); }), "Board color is invalid.");
                }
                m.check(out.bindings.size() < max_bindings, "Board colors exceed bounds.");
                out.bindings.push_back({item, material, p, key, color, target.part});
            }
        }
    }
    return out;
}
} // namespace developer_board_detail
// What was saved of a board's parts while they are coloured. A key a part's material does not
// have inherits .5 from the compiled board shaders: one is added at native preset priority, so
// a later native preset replaces it, and goes back to that default when the colouring stops.
// Never clear a NativeValue: the renderer still copies the key's full payload.
struct DeveloperBoardState : developer_hoodie_detail::ItemState {};
} // namespace dingosdk
