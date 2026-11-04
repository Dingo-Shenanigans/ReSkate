#pragma once
#include "native_skater.h"
#include <functional>
namespace dingosdk::multiplayer {
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &, std::string &detail);
// Only called for the owned remote actor on the verified client update thread.
// `hidden` names the items this PC shows as if it did not have them (plugin_session_feed.h's
// hide_items): a stand-in, the default, or nothing.
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &, const std::function<bool(const std::string &)> &hidden = {});
} // namespace dingosdk::multiplayer
