#include "worn_items.h"
#include "item_browser.h"
#include "outfit_kind.h"
#include "local_customization.h"
#include "local_customization_runtime.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_slot_override.h"
#include <Windows.h>
#include <algorithm>
#include <array>

// What the skater wears, read from the game's own presets: ported from Fit Check (pull request 326,
// turnaround.cpp), for plugins (reskate.outfit.worn, board, title).
namespace dingosdk::worn_items {
namespace {
// The board's own recipe in a skater preset (its template), and its slots, in the card's order. Read
// from the game's presets with Fit Check's `turnaround parts`, 2026-10-20.
constexpr std::uint32_t board_template = 0x5e61aaef;
struct BoardSlot { std::uint32_t slot; const char* label; };
constexpr BoardSlot board_slots[] = {
    {0x66071628, "Deck"}, {0x755d5ee0, "Grip tape"}, {0x976f18c4, "Grip cutout"}, {0x0add98be, "Trucks"}, {0xc83bb94e, "Wheels"}};

// An item's name as the game shows it: its catalog title, the title an item grid showed (a mod's
// item has no catalog entry), or its key made readable the way the game does for a mod's item.
} // namespace
std::string item_title(const std::string& asset) {
    auto title = profile::item_display_metadata(asset, Json::object()).value("title", "");
    if (title.empty()) title = item_browser::known_title(asset);
    if (title.empty()) {
        title = asset.starts_with("Own_") || asset.starts_with("own_") ? asset.substr(4) : asset;
        std::replace(title.begin(), title.end(), '_', ' ');
    }
    return title;
}

namespace {
// The selected skater preset (the native getter skater_slot_override.cpp uses), or -1. Game thread.
std::int32_t selected_preset(std::uintptr_t base, std::uintptr_t manager) {
    namespace slot = addr::skater_slot_override;
    std::array<unsigned char, slot::native_selected_prefix.size()> bytes{};
    if (!memory::read_bytes(base + slot::native_selected, bytes.data(), bytes.size()) || bytes != slot::native_selected_prefix) return -1;
    return reinterpret_cast<std::int32_t (*)(std::uintptr_t)>(base + slot::native_selected)(manager);
}

// The board the skater rides now: the board recipe of the selected preset (the first when none is
// selected), its parts in the card's order, empty slots left out. Game thread.
} // namespace
std::vector<Worn> worn_board(bool titles) {
    std::vector<Worn> parts;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t manager{}, begin{}, end{};
    if (!memory::read(base + addr::engine::loadout_manager, manager) || !manager || !memory::read(manager + 0x18, begin) ||
        !memory::read(manager + 0x20, end) || begin >= end || (end - begin) % 0x48 || (end - begin) / 0x48 > 10)
        return parts;
    const auto count = static_cast<std::int32_t>((end - begin) / 0x48);
    auto preset = selected_preset(base, manager);
    if (preset < 0 || preset >= count) preset = 0;
    profile::CosmeticLoadout loadout;
    std::vector<profile_runtime::CosmeticNativeRecipe> native;
    if (!profile_runtime::read_cosmetic_recipes(reinterpret_cast<const void*>(begin + static_cast<std::uintptr_t>(preset) * 0x48), loadout, native))
        return parts;
    for (const auto& recipe : loadout.recipes) {
        if (recipe.template_key != board_template) continue;
        for (const auto& wanted : board_slots)
            for (const auto& item : recipe.items) {
                if (item.slot != wanted.slot || item.asset.empty()) continue;
                parts.push_back({wanted.label, item.asset, titles ? item_title(item.asset) : std::string{}});
            }
        break;
    }
    return parts;
}

// What the skater wears now: every item of the selected preset
// but the board's, labelled by its catalog category (the game's names for its grids: "tops",
// "headgear"...), clothes, accessories and full-body costumes only (not the body, hair, tattoos or
// gestures; outfit_kind.h), in the order they are worn from head to foot. Game thread.
std::vector<Worn> worn_outfit() {
    std::vector<Worn> parts;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t manager{}, begin{}, end{};
    if (!memory::read(base + addr::engine::loadout_manager, manager) || !manager || !memory::read(manager + 0x18, begin) ||
        !memory::read(manager + 0x20, end) || begin >= end || (end - begin) % 0x48 || (end - begin) / 0x48 > 10)
        return parts;
    const auto count = static_cast<std::int32_t>((end - begin) / 0x48);
    auto preset = selected_preset(base, manager);
    if (preset < 0 || preset >= count) preset = 0;
    profile::CosmeticLoadout loadout;
    std::vector<profile_runtime::CosmeticNativeRecipe> native;
    if (!profile_runtime::read_cosmetic_recipes(reinterpret_cast<const void*>(begin + static_cast<std::uintptr_t>(preset) * 0x48), loadout, native))
        return parts;
    const auto& known = profile_runtime::cosmetic_runtime().items;
    std::vector<std::pair<int, Worn>> found;
    for (const auto& recipe : loadout.recipes) {
        if (recipe.template_key == board_template) continue;
        for (const auto& item : recipe.items) {
            if (item.asset.empty()) continue;
            const auto entry = known.find(item.asset);
            const auto kind = outfit_kind::of(entry != known.end() ? entry->second.category : std::string());
            if (!kind) continue;
            found.push_back({kind->order, {kind->label, item.asset, item_title(item.asset)}});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& entry : found) parts.push_back(std::move(entry.second));
    return parts;
}
} // namespace dingosdk::worn_items
