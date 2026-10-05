#pragma once
#include "native_skater.h"
namespace dingosdk::multiplayer {
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &, std::string &detail);
// The local outfit as other players should see it: an extension may dress the local view
// differently (VR's Feet only). Client update thread.
using OutfitForOthers = void (*)(CosmeticRecipe &skater) noexcept;
void set_outfit_for_others(OutfitForOthers filter) noexcept;
void outfit_for_others(CosmeticRecipe &skater) noexcept;
// Only called for the owned remote actor on the verified client update thread.
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &);
} // namespace dingosdk::multiplayer
