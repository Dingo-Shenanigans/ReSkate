#include "plugin_effects.h"
#include <cmath>

namespace dingosdk::plugins {
bool private_effect_asset(std::string_view asset, bool decal) noexcept {
    const std::string_view prefix = decal ? "world/materials/decal/reskate_" : "effects/reskate/";
    if (!asset.starts_with(prefix) || asset.size() <= prefix.size() || asset.size() > 240 ||
        asset.back() == '/' || asset.find("//") != std::string_view::npos) return false;
    for (const char c : asset)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-')) return false;
    return true;
}
bool valid_effect_matrix(const EffectMatrix& m) noexcept {
    for (const float v : m) if (!std::isfinite(v)) return false;
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return false;
    float lengths[3]{};
    for (unsigned a = 0; a < 3; ++a) {
        if (std::abs(m[12 + a]) > 100000) return false;
        for (unsigned i = 0; i < 3; ++i) lengths[a] += m[a * 4 + i] * m[a * 4 + i];
        if (lengths[a] < .000001f || lengths[a] > 400) return false;
        for (unsigned b = 0; b < a; ++b) {
            float dot{};
            for (unsigned i = 0; i < 3; ++i) dot += m[a * 4 + i] * m[b * 4 + i];
            if (std::abs(dot) > .01f * std::sqrt(lengths[a] * lengths[b])) return false;
        }
    }
    return true;
}
}
