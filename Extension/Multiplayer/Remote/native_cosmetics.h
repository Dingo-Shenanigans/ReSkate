#pragma once
#include "native_skater.h"
namespace dingosdk::multiplayer {
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &, std::string &detail);
// Debug (mp cosmetic-slots): the local skater's and board's template slots, one line each, with the
// category each takes (what the shuffle matches on), the item worn there, the mod it is from (if
// any) and how many installed items fit it. Client thread.
std::string describe_cosmetic_slots(std::uintptr_t base, const NativeFrame &local);
// Only called for the owned remote actor on the verified client update thread.
// `shuffle` (outfit_shuffle.h; a default one is off) gives items of a skater or board recipe a
// random item that fits their slot, instead of the usual stand-in, as its mode says.
// One that cannot be shuffled is shown with the stand-ins.
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &, const Shuffle &shuffle = {});
} // namespace dingosdk::multiplayer
