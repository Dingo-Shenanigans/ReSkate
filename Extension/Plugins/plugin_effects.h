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
};
// Private mod assets only. Stock effect blueprints include store cosmetics.
bool private_effect_asset(std::string_view asset, bool decal) noexcept;
bool valid_effect_matrix(const EffectMatrix& matrix) noexcept;
}
