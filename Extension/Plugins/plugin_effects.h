#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace dingosdk::plugins {
using EffectMatrix = std::array<float, 16>;
struct EffectRequest {
    std::string asset;
    EffectMatrix transform{};
    bool decal{}, attach{};
    float lift{};
    // One of the game's own particle effects (the effects.game permission), not the mod's.
    bool game{};
};
// A mod's own asset: effects/reskate/... or world/materials/decal/reskate_...
bool private_effect_asset(std::string_view asset, bool decal) noexcept;
// A name one of the game's own particle effects could have: any other asset path. The game's
// effects include those its store sells, so a plugin plays them only with the effects.game
// permission, which the player sees when approving it. An effect shows a picture; it never
// equips or grants an item.
bool game_effect_asset(std::string_view asset) noexcept;
// A bundle of the game's that holds effects and can be asked for: one under win32/effects/
// (a gesture's, say). Not a level's or an item's: those are far more than an effect.
bool game_effect_bundle(std::string_view bundle) noexcept;
// What became of a request for one of the game's effect bundles.
enum class EffectBundleState { unavailable, loading, ready, failed };
bool valid_effect_matrix(const EffectMatrix& matrix) noexcept;
}
