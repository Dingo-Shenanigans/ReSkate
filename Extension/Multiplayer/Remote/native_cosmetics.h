#pragma once
#include "native_skater.h"
#include <functional>
namespace dingosdk::multiplayer {
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &, std::string &detail);
// Only called for the owned remote actor on the verified client update thread.
// `hidden` names the items this PC shows as if it did not have them (plugin_session_feed.h's
// hide_items): a stand-in, the default, or nothing. `player`: whose recipe it is (their session
// id; 0 unknown), which a plugin's random stand-ins are seeded by.
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &, const std::function<bool(const std::string &)> &hidden = {},
                           std::uint64_t player = 0);
} // namespace dingosdk::multiplayer
