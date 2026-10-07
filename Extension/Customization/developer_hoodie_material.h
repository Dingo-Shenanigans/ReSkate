#pragma once
#include "Extension/Multiplayer/developer_identity.h"
#include "Extension/Multiplayer/Remote/cosmetics.h"
#include "Extension/Multiplayer/Remote/native_cosmetics_layout.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The cosmetics of a player on one of the backend's lists (developer_identity.h) are coloured
// for them: whatever they wear and ride, on their own material instances, as they choose on the
// menu's Special page. It began as the developers' rainbow hoodie and pink board, which the
// names here still say.
namespace dingosdk {
namespace developer_hoodie_detail {
// The compiled appearance preset names a skater's item controllers by their assigned CAS slot
// ("TopItem" and the like are Lua construction names, not the live controllers' names).
inline constexpr std::string_view slot_folder = "Characters/MainCharacters/Generic/CAS/Common/Slots/";
// The skater's marked cosmetics, in the order of their styles (multiplayer::mark_item_names):
// top, bottoms, shoes, socks, hat, glasses, outfit, costume.
inline constexpr std::array<std::string_view, multiplayer::skater_mark_items> slots{
    "CAS_Top_Slot",      "CAS_Bottom_Slot",  "CAS_Footwear_Slot", "CAS_Sock_Slot",
    "CAS_Headgear_Slot", "CAS_Eyewear_Slot", "CAS_Outfit_Slot",   "CAS_CostumeFullBody_Slot"};
// Serialized ParamDbKey identities of the ColorRgb parameters the game's clothing presets set
// (AppearanceShaderExpressionPreset.ParamInfo; read from 114 presets of every kind of item,
// 2026-10-04): the colours of an item's regions. Every kind of clothing draws on the same
// ones, the first thirteen by far the most, and each item colours some and leaves the rest at
// the shaders' neutral grey (`neutral`), where its textures show as they are.
// On the relaxed hoodie, as seen in game: the first is the whole body, sleeves and hood; the
// fourth the cuffs and hem; the fifth the drawstrings and hood lining.
inline constexpr std::array<std::uint64_t, 28> color_keys{
    0x8c46cd354b18a9e2ULL, 0x1567b5742f4ec0b2ULL, 0x1564a942cb33c53bULL, 0xf526305270d94cb3ULL,
    0x99879bf202d1960aULL, 0x998450425dbd9044ULL, 0x18984e85c679da8dULL, 0x2bf96fc9d8da57bbULL,
    0x13cfb51bd8474129ULL, 0x7c06880748ded394ULL, 0xfca496001ea60718ULL, 0xfca79ddc325249ceULL,
    0x5e1943a7c20ab77cULL, 0x0adafe3e13df4057ULL, 0xe3ab5f6a33dd1684ULL, 0xc31ad6ca613dcf43ULL,
    0xc1c70aa67f8bde7cULL, 0xc1c4554f6b58eee8ULL, 0x24e6723cc9f17eccULL, 0xd98b435969485311ULL,
    0x27e785e804edea5cULL, 0x27e45bf8d67d959fULL, 0xae823ac5acb2abbaULL, 0x89cbc6b088ae9b1bULL,
    0x593aacb9e12dcd6bULL, 0x4ec6aeb742a2c383ULL, 0xa19416e977675f88ULL, 0x0a94b0e28d102968ULL};
using Color = std::array<float, 3>;
inline constexpr Color neutral{.5f, .5f, .5f};
// Native ParamDbKey, as a material's parameter node holds it. The first word is the final two
// serialized GUID bytes; the type index is the live ColorRgb descriptor's.
struct ParameterKey {
    std::uint16_t identity{}, size{}, type{}, count{};
    std::uint64_t hash{};
};
static_assert(sizeof(ParameterKey) == 16 && offsetof(ParameterKey, hash) == 8);
// One colour of one material instance. `part` is which marked cosmetic it belongs to.
struct Binding {
    std::uintptr_t item{}, material{}, node{}; // node 0: no such parameter yet (the board adds one)
    ParameterKey key{};
    Color color{};
    std::uint8_t part{};
};
struct SavedColor {
    Binding binding;
    Color original{}, last{};
    std::uint8_t settling{}; // publishes still owed for `last` (settle_publishes)
};
// The colours found on an actor in one walk.
struct Materials {
    std::uintptr_t component{}, appearance{};
    std::vector<Binding> bindings;
    bool pending{};
    // Clothing: a colour at `neutral` is a region the item leaves to its textures, and is left
    // alone, unless the item colours none of its regions: then all of them are coloured.
    bool regions_only{};
};
// What was saved of an actor's colours while they are coloured here.
struct ItemState {
    std::uintptr_t entity{}, component{}, appearance{};
    std::uint64_t generation{};
    std::vector<SavedColor> colors;
};
// What is equipped can be anything: this many colours of a skater, and no more.
inline constexpr std::size_t max_bindings = 512;
inline std::uint32_t material_hash(std::string_view name) noexcept {
    std::uint32_t hash = 5381;
    for (auto c : name) hash = hash * 33 ^ static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return hash;
}
inline Color rainbow(std::uint64_t milliseconds, std::uint64_t period = 8000) noexcept {
    // One continuous hue cycle every eight seconds (or `period`), in the material's existing gamut.
    const auto hue = static_cast<float>(milliseconds % period) * (6.f / static_cast<float>(period));
    const auto sector = static_cast<unsigned>(hue);
    constexpr float high = .8f, low = .025f;
    const auto rising = low + (high - low) * (hue - static_cast<float>(sector));
    const auto falling = high + low - rising;
    switch (sector) {
    case 0: return {high, rising, low};
    case 1: return {falling, high, low};
    case 2: return {low, high, rising};
    case 3: return {low, falling, high};
    case 4: return {rising, low, high};
    default: return {high, low, falling};
    }
}
// What one of a listed player's marked cosmetics does right now: nothing, the rainbow, or a
// walk through three colours and back (one colour three times stands still).
struct ItemAnimation {
    bool on{}, rainbow{};
    std::array<Color, 3> stops{};
    std::uint64_t period = 3000; // milliseconds for a round
};
// A colour as picked on screen, as the material takes it: linear, and inside the gamut the
// rainbow keeps to.
inline Color material_color(const std::array<std::uint8_t, 3> &picked) noexcept {
    Color out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = std::max(.004f, std::pow(static_cast<float>(picked[i]) / 255.f, 2.2f) * .8f);
    return out;
}
// The colours the pickers start from: about where each list's own animation begins and ends.
inline std::pair<std::array<std::uint8_t, 3>, std::array<std::uint8_t, 3>> standard_picks(multiplayer::IdentityList mark) noexcept {
    using multiplayer::IdentityList;
    if (mark == IdentityList::content_creator) return {{0x88, 0x23, 0x23}, {0xff, 0x48, 0x70}};
    if (mark == IdentityList::homie) return {{0xad, 0x6c, 0x1c}, {0xff, 0xe6, 0x67}};
    if (mark == IdentityList::centrix) return {{0x1f, 0x7b, 0xff}, {0xff, 0xff, 0xff}};
    return {{0x6e, 0x20, 0xff}, {0xb4, 0x78, 0xff}};
}
// What a cosmetic does for a player on `mark`'s list (nobody else's does anything) with the
// style they chose on the Special page. Left standard, a developer's cycles the rainbow, a
// content creator's goes from deep red through bright red to a reddish pink and back, and a
// homie's from deep gold through bright gold to yellow and back, and Centrix's from blue
// through light blue to white and back. No style asks for the rainbow:
// it is a developer's standard, and nobody else's.
inline ItemAnimation item_animation(std::optional<multiplayer::IdentityList> mark, const multiplayer::MarkStyle &style) noexcept {
    using multiplayer::IdentityList;
    using multiplayer::MarkMode;
    ItemAnimation out;
    if (!mark || style.mode == MarkMode::off) return out;
    out.on = true;
    out.rainbow = style.mode == MarkMode::standard && *mark == IdentityList::developer;
    const std::uint64_t normal = out.rainbow ? 8000 : 3000;
    out.period = style.speed == 1 ? normal * 2 : style.speed == 2 ? normal / 2 : normal;
    if (style.mode == MarkMode::solid) {
        out.stops.fill(material_color(style.from));
    } else if (style.mode == MarkMode::gradient) {
        const auto from = material_color(style.from), to = material_color(style.to);
        out.stops = {from, Color{(from[0] + to[0]) * .5f, (from[1] + to[1]) * .5f, (from[2] + to[2]) * .5f}, to};
    } else if (!out.rainbow) {
        // One colour getting lighter and darker is hard to see, so each goes on into the colour
        // next to it: a deep shade, a bright one, then a reddish pink or a yellow.
        constexpr std::array<Color, 3> red{{{.2f, .004f, .01f}, {.8f, .03f, .03f}, {.8f, .05f, .13f}}};
        constexpr std::array<Color, 3> gold{{{.34f, .12f, .006f}, {.8f, .5f, .06f}, {.8f, .64f, .11f}}};
        constexpr std::array<Color, 3> blue{{{.01f, .16f, .8f}, {.3f, .5f, .8f}, {.8f, .8f, .8f}}};
        out.stops = *mark == IdentityList::content_creator ? red : *mark == IdentityList::centrix ? blue : gold;
    }
    return out;
}
inline Color item_color(const ItemAnimation &animation, std::uint64_t milliseconds) noexcept {
    if (animation.rainbow) return rainbow(milliseconds, animation.period);
    // 0 to 2 and back, once a round.
    const auto along = 1.f - std::cos(static_cast<float>(milliseconds % animation.period) *
                                      (6.2831853f / static_cast<float>(animation.period)));
    const auto &from = animation.stops[along < 1.f ? 0 : 1], &to = animation.stops[along < 1.f ? 1 : 2];
    const auto amount = along < 1.f ? along : along - 1.f;
    return {from[0] + (to[0] - from[0]) * amount, from[1] + (to[1] - from[1]) * amount,
            from[2] + (to[2] - from[2]) * amount};
}
// The local player's own styles (the Special page): sent with their appearance, and kept in
// their profile as text, two hex digits a byte.
inline std::atomic<multiplayer::MarkStyles> own_styles{};
inline std::string mark_styles_text(const multiplayer::MarkStyles &styles) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    const auto add = [&](std::uint8_t byte) { text += digits[byte >> 4], text += digits[byte & 15]; };
    for (const auto &style : styles) {
        add(static_cast<std::uint8_t>(style.mode));
        for (const auto part : style.from) add(part);
        for (const auto part : style.to) add(part);
        add(style.speed);
    }
    return text;
}
inline std::optional<multiplayer::MarkStyles> parse_mark_styles(std::string_view text) noexcept {
    multiplayer::MarkStyles styles{};
    if (text.size() != styles.size() * 16) return std::nullopt;
    std::size_t at = 0;
    bool valid = true;
    const auto next = [&]() -> std::uint8_t {
        unsigned byte = 0;
        for (int digit = 0; digit < 2; ++digit) {
            const char c = text[at++];
            if (c >= '0' && c <= '9') byte = byte * 16 + static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') byte = byte * 16 + static_cast<unsigned>(c - 'a' + 10);
            else valid = false;
        }
        return static_cast<std::uint8_t>(byte);
    };
    for (auto &style : styles) {
        style.mode = static_cast<multiplayer::MarkMode>(next());
        for (auto &part : style.from) part = next();
        for (auto &part : style.to) part = next();
        style.speed = next();
        valid = valid && multiplayer::valid_mark_style(style);
    }
    return valid ? std::optional(styles) : std::nullopt;
}
// A material instance's own parameters: a hash map of typed nodes.
struct ParameterMap {
    std::uintptr_t buckets{}, stop{};
    std::uint32_t size{}, total{};
};
template<class Memory> ParameterMap parameter_map(Memory &m, std::uintptr_t material) {
    ParameterMap out{m.ptr(material, 0x268), 0, m.template get<std::uint32_t>(material, 0x270),
                     m.template get<std::uint32_t>(material, 0x274)};
    if (!out.buckets || !out.size || out.size > 1024 || !out.total || out.total > 512) return {};
    out.stop = m.ptr(out.buckets, out.size * 8ULL);
    return out;
}
// The node of a material's ColorRgb parameter with this key, or 0. The EBX boxed ColorRgb has a
// serialized gamut word; the loaded native ColorRgb is three floats (12 bytes): this goes by
// the actual type descriptor, not the serialized asset size. A parameter of another shape is
// passed over, not reported: what is equipped can be anything.
template<class Memory>
std::uintptr_t color_parameter(Memory &m, std::uintptr_t base, const ParameterMap &map, std::uint64_t key, ParameterKey &native,
                               Color &color) {
    if (!map.size) return 0;
    auto node = m.ptr(map.buckets, (key % map.size) * 8ULL);
    for (std::uint32_t visited = 0; node && node != map.stop && visited < map.total; ++visited, node = m.ptr(node, 0x40)) {
        if (m.template get<std::uint64_t>(node, 8) != key) continue;
        native = m.template get<ParameterKey>(node);
        const auto type = m.ptr(node, 0x20);
        if (type < base || type >= base + 0x09144000 || native.size != sizeof(Color) || native.count != 1 ||
            !(m.template get<std::uint32_t>(node, 0x28) & 1) || m.template get<std::uint16_t>(type, 8) != native.type)
            return 0;
        const auto info = m.ptr(type);
        if (m.template get<std::uint32_t>(info) != 0x885eff59U || m.template get<std::uint16_t>(info, 6) != sizeof(Color)) return 0;
        color = m.template get<Color>(node, 0x10);
        return std::all_of(color.begin(), color.end(), [](float c) { return std::isfinite(c); }) ? node : 0;
    }
    return 0;
}
// All discovery is bounded and follows the actor's own component/controller/material
// backlinks. Never edit the shared AppearanceShaderExpressionPreset or recipe.
template<class Read> Materials materials(Read &read, std::uintptr_t base, std::uintptr_t entity) {
    using namespace multiplayer;
    CosmeticMemory<Read &> m{read, base};
    Materials out;
    out.regions_only = true;
    m.check(m.ptr(entity) == base + addr::engine::skater_entity_vtable, "Skater actor type differs.");
    out.component = m.component(entity);
    out.pending = m.template get<std::uint8_t>(out.component, 0x10a) || m.template get<std::uint8_t>(out.component, 0x10d);
    out.appearance = read_native_component(read, entity, base + addr::engine::skater_appearance_vtable);
    const auto begin = m.ptr(out.appearance, 0x90), end = m.ptr(out.appearance, 0x98), capacity = m.ptr(out.appearance, 0xa0);
    m.check(begin <= end && end <= capacity && (end - begin) % 8 == 0 && (capacity - begin) % 8 == 0 &&
                (capacity - begin) / 8 <= 64 && (begin || !capacity), "Skater controller array exceeds bounds.");
    for (auto entry = begin; entry < end; entry += 8) {
        const auto item = m.ptr(entry);
        if (!item || m.ptr(item, 0x48) != out.appearance + 0x40) continue;
        const auto name = m.text(m.ptr(item, 0x30));
        if (!name.starts_with(slot_folder)) continue;
        const auto slot = std::find(slots.begin(), slots.end(), std::string_view(name).substr(slot_folder.size()));
        if (slot == slots.end()) continue;
        const auto part = static_cast<std::size_t>(slot - slots.begin());
        // A slot with nothing in it, or with an item still on its way, has no materials.
        if (!(m.template get<std::uint32_t>(item, 0xb0) & 2)) continue;
        const auto buckets = m.ptr(item, 0x1f0);
        const auto size = m.template get<std::uint32_t>(item, 0x1f8), total = m.template get<std::uint32_t>(item, 0x1fc);
        if (!total) continue;
        m.check(buckets && size && size <= 256 && total <= 128, "Skater material map exceeds bounds.");
        // Every named material of the item, whatever its mesh calls them ("Top_mat", "Bottom_mat", ...).
        std::array<std::uintptr_t, 257> heads{};
        m.check(read(buckets, heads.data(), (size + 1ULL) * 8), "Skater material map cannot be read.");
        std::uint32_t visited{};
        for (std::uint32_t bucket = 0; bucket < size; ++bucket) {
            for (auto node = heads[bucket]; node && node != heads[size]; node = m.ptr(node, 0x28)) {
                if (m.template get<std::uint32_t>(node) % size != bucket) break; // the next bucket's
                m.check(visited++ < total, "Skater material chain exceeds bounds.");
                std::array<std::uintptr_t, 4> seen{};
                for (std::size_t variant = 0; variant < seen.size(); ++variant) {
                    const auto material = m.ptr(node, 8 + variant * 8);
                    if (!material || std::find(seen.begin(), seen.begin() + variant, material) != seen.begin() + variant) continue;
                    seen[variant] = material;
                    const auto map = parameter_map(m, material);
                    for (const auto key : color_keys) {
                        if (out.bindings.size() == max_bindings) break;
                        ParameterKey native;
                        Color color{};
                        if (const auto parameter = color_parameter(m, base, map, key, native, color))
                            out.bindings.push_back({item, material, parameter, native, color, static_cast<std::uint8_t>(part)});
                    }
                }
            }
        }
    }
    return out;
}
inline bool same_binding(const Binding &live, const Binding &saved) noexcept {
    // A parameter the board added has its node from the next fresh walk on.
    return live.item == saved.item && live.material == saved.material && live.key.hash == saved.key.hash &&
           (saved.node ? live.node == saved.node : live.node != 0);
}
// The renderer shows a color one publish late. While a color keeps changing the next tick's
// publish brings it; one that stands still (a solid color, or the item's own once the
// animation is off) needs publishing again after the write that set it, or the item keeps the
// color before it on screen (seen in game, 2026-10-04).
inline constexpr std::uint8_t settle_publishes = 3;
// Ownership is discovered afresh before every write, and writes go only to what that walk
// found. A color is restored only while it is still the one written here: a new native
// preset takes precedence. `animations`: what each part does, or nothing.
template<std::size_t Parts, class Write, class Publish>
void animate(ItemState &state, const Materials &live, std::uintptr_t entity, std::uint64_t generation,
             const std::array<ItemAnimation, Parts> &animations, std::uint64_t milliseconds, Write &write, Publish &publish) {
    if (state.entity != entity || state.generation != generation || state.component != live.component || state.appearance != live.appearance)
        state = {};
    // Pausing, loading and a recipe on its way keep what was saved until a walk succeeds.
    if (live.pending) return;
    // What was saved of each color found: a walk finds them in the same order as the last.
    std::vector<const SavedColor *> saved(live.bindings.size());
    std::array<bool, Parts> regions{};
    for (std::size_t i = 0, from = 0; i < live.bindings.size(); ++i) {
        const auto &binding = live.bindings[i];
        for (std::size_t n = 0; n < state.colors.size(); ++n) {
            const auto at = (from + n) % state.colors.size();
            if (same_binding(binding, state.colors[at].binding)) { saved[i] = &state.colors[at], from = at + 1; break; }
        }
        const auto &own = saved[i] && binding.color == saved[i]->last ? saved[i]->original : binding.color;
        regions[binding.part] = regions[binding.part] || own != neutral;
    }
    ItemState next;
    next.entity = entity; next.generation = generation; next.component = live.component; next.appearance = live.appearance;
    next.colors.reserve(live.bindings.size());
    std::array<std::uintptr_t, 64> changed_items{};
    std::size_t changed{};
    for (std::size_t i = 0; i < live.bindings.size(); ++i) {
        const auto &binding = live.bindings[i];
        const auto *was = saved[i];
        const auto &animation = animations[binding.part];
        const bool ours = was && binding.color == was->last;
        const auto original = ours ? was->original : binding.color;
        bool show{};
        if (animation.on && !(live.regions_only && regions[binding.part] && original == neutral)) {
            const auto color = item_color(animation, milliseconds);
            if (binding.node && binding.color == color) {
                // Already this color: nothing to write, but it may still be owed to the renderer.
                const std::uint8_t left = ours ? was->settling : 0;
                next.colors.push_back({binding, original, color, static_cast<std::uint8_t>(left ? left - 1 : 0)});
                show = left != 0;
            } else if (write(binding, color)) {
                next.colors.push_back({binding, original, color, settle_publishes});
                show = true;
            } else if (was) next.colors.push_back(*was);
        } else if (was && (binding.color == was->last || binding.color == was->original)) {
            // Back to the item's own color, and published until it shows.
            const bool first = was->last != was->original;
            show = binding.color == was->original || write(binding, was->original);
            const std::uint8_t left = first ? settle_publishes : was->settling ? was->settling - 1 : 0;
            if (!show) next.colors.push_back(*was);
            else if (left) next.colors.push_back({binding, was->original, was->original, left});
        }
        if (show && changed < changed_items.size() &&
            std::find(changed_items.begin(), changed_items.begin() + changed, binding.item) == changed_items.begin() + changed)
            changed_items[changed++] = binding.item;
    }
    state.entity = next.entity, state.generation = next.generation, state.component = next.component, state.appearance = next.appearance;
    state.colors = std::move(next.colors);
    for (std::size_t i = 0; i < changed; ++i) publish(changed_items[i]);
}
} // namespace developer_hoodie_detail
// What was saved of a skater's items while they are coloured.
struct DeveloperHoodieState : developer_hoodie_detail::ItemState {};
} // namespace dingosdk
