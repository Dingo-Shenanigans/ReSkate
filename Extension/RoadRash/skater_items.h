#pragma once
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_cosmetics_layout.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The local skater's cosmetic recipe while it plays: what Road Rash needs to read it, to hand the
// game a changed one and to give the skater back to the game afterwards. Client thread only. Everything is read through guarded reads and checked
// as the multiplayer code checks a peer's outfit (native_cosmetics_layout.h); a failed check throws.
namespace dingosdk::road_rash {
using multiplayer::CosmeticRecipe;
using multiplayer::CosmeticSlot;
inline bool readable(std::uintptr_t address, void* out, std::size_t size) {
    return size ? memory::peek_bytes(address, out, size) : address >= 0x10000;
}
using SkaterItems = multiplayer::CosmeticMemory<bool (*)(std::uintptr_t, void*, std::size_t)>;

inline std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}
// The installed cosmetic whose name ends in `ending` (lower case), or nothing.
inline std::string installed_item(const SkaterItems& m, std::string_view ending) {
    const auto manager = m.ptr(m.base, addr::engine::cosmetics_manager);
    m.check(m.ptr(manager) == m.base + addr::engine::cosmetics_manager_vtable && m.get<std::uint8_t>(manager, 0xa8) == 1,
            "Cosmetic catalog is not ready.");
    const auto buckets = m.ptr(manager, 0x30);
    const auto size = m.get<std::uint32_t>(manager, 0x38), total = m.get<std::uint32_t>(manager, 0x3c);
    m.check(size && size <= 16384 && total && total <= 8192, "Cosmetic catalog exceeds bounds.");
    std::uint32_t visited{};
    for (std::uint32_t bucket = 0; bucket < size && visited < total; ++bucket) {
        auto node = m.ptr(buckets, bucket * 8ULL);
        for (std::uint32_t chain = 0; node && chain < total && visited < total; ++chain, ++visited, node = m.ptr(node, 0x10)) {
            const auto asset = m.ptr(node, 8) & ~std::uintptr_t{4};
            if (!asset) continue;
            auto name = m.text(m.ptr(asset, 0x38));
            if (lower(name).ends_with(ending)) return name;
        }
    }
    return {};
}
// A recipe as the game keeps the local skater's: every item by its hash, with no name beside it.
// The game changes an item of that skater where it lies, hash and parameters only; a name the
// recipe carried would stay behind and no longer be the item's.
struct LentRecipe {
    multiplayer::CosmeticArray<std::uint32_t> scalars, mask{0};
    multiplayer::CosmeticArray<multiplayer::NativeCosmeticItem> items;
    std::vector<multiplayer::CosmeticArray<std::uint32_t>> parameters;
    multiplayer::NativeCosmeticRecipe value;
    // Holds arrays of its own in the game's layout; the native copy copies out of them.
    explicit LentRecipe(const CosmeticRecipe& recipe) : scalars(recipe.scalars.size()), items(recipe.items.size()) {
        std::copy(recipe.scalars.begin(), recipe.scalars.end(), scalars.data());
        parameters.reserve(recipe.items.size());
        for (std::size_t i = 0; i < recipe.items.size(); ++i) {
            const auto& item = recipe.items[i];
            parameters.emplace_back(item.parameters.size());
            std::copy(item.parameters.begin(), item.parameters.end(), parameters.back().data());
            items.data()[i] = {"", parameters.back().data(), multiplayer::cosmetic_asset_hash(item.asset), item.slot};
        }
        value = {scalars.data(), items.data(), mask.data()};
    }
    LentRecipe(const LentRecipe&) = delete;
    LentRecipe& operator=(const LentRecipe&) = delete;
};
// Hands the skater a recipe through the game's own recipe copy, the call the multiplayer code makes
// for other players' outfits: the copy marks the skater for its normal rebuild.
inline void apply(const SkaterItems& m, std::uintptr_t component, const CosmeticRecipe& recipe) {
    constexpr auto prefix = addr::native_cosmetics::recipe_copy_prefix;
    m.check(m.get<std::array<std::uint8_t, prefix.size()>>(m.base, addr::native_cosmetics::recipe_copy) == prefix,
            "Native cosmetic copy function differs.");
    m.validate(m.resource(component), recipe);
    LentRecipe lent(recipe);
    reinterpret_cast<void (*)(std::uintptr_t, const multiplayer::NativeCosmeticRecipe*)>(
        m.base + addr::native_cosmetics::recipe_copy)(component, &lent.value);
    m.check(m.ptr(component, 0x158) != reinterpret_cast<std::uintptr_t>(lent.value.items),
            "Native cosmetic copy did not take ownership.");
}

// How the game feeds a skater's items. The local skater's follow the player's outfit: an item
// chosen in the game's menus reaches the skater in the world as it is chosen. A recipe handed over
// with `apply` becomes the skater's own, and from then on the game passes nothing of the outfit on
// to it (the multiplayer code relies on that for other players' skaters). These are the four words
// of the item component the recipe copy changes for it: on build 20260929 they read 2, 0, 2, 0
// while the outfit is followed and 1, 2, 1, 2 with a recipe of the skater's own.
struct Feed {
    std::array<std::uint32_t, 4> words{};
    bool operator==(const Feed&) const = default;
    // The copy leaves a 2 in the second word (the multiplayer code checks that one too).
    bool follows_outfit() const noexcept { return words[1] != 2; }
};
inline constexpr std::array<std::size_t, 4> feed_words{0x138, 0x13c, 0x144, 0x148};
// What the component still has to work through: changed items, and a changed feed.
inline constexpr std::size_t items_changed = 0x10a, feed_changed = 0x10d;
inline Feed feed(const SkaterItems& m, std::uintptr_t component) {
    Feed out;
    for (std::size_t i = 0; i < feed_words.size(); ++i) out.words[i] = m.get<std::uint32_t>(component, feed_words[i]);
    return out;
}
// Gives a skater that wears a recipe of its own back to the game. `before` is how the game fed it
// before the recipe; with those words back and the component told that its feed changed, the game
// puts the outfit's own items back at its next update and passes the outfit's changes on again.
inline void hand_back(const SkaterItems& m, std::uintptr_t component, const Feed& before) {
    m.check(!m.get<std::uint8_t>(component, items_changed) && !m.get<std::uint8_t>(component, feed_changed),
            "Waiting for the skater's items to finish updating.");
    (void)feed(m, component); // all four can be read, so they can be written
    const auto put = [](std::uintptr_t address, const auto& value) {
        SIZE_T done{};
        SkaterItems::check(WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &done) &&
                               done == sizeof(value),
                           "Cannot update the skater's item component.");
    };
    for (std::size_t i = 0; i < feed_words.size(); ++i) put(component + feed_words[i], before.words[i]);
    put(component + feed_changed, std::uint8_t{1});
}
}
