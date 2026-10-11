#pragma once
#include <array>
#include <string_view>

namespace dingosdk::texture_tier {
// Texture Quality tiers as the game's Quality enum numbers them (Constants.lua).
enum Tier : int { unknown = -1, low = 0, medium = 1, high = 2, ultra = 3 };

// The engine controls that Scripts/Game/QualitySettings/Graphics.lua sets for each
// 'TextureQuality' tier. QualityLevel_* are 0..3 in the same order as the tiers (a selection of
// Ultra reads back ShaderSystem.ShaderQualityLevel 3).
// TextureStreaming.PoolSize is left out on purpose: the Licensee path sizes it per tier
// (GraphicsPC.lua) and raising it costs video memory on small cards (issue #160).
struct Setting { std::string_view name, value; };
constexpr std::array<std::array<Setting, 3>, 4> tiers{{
    {{{"Texture.SkipMipmapCount", "1"}, {"TextureStreaming.FreeStreamingQuality", "0"},
      {"TextureCompositor.TextureCompositorQualityLevel", "0"}}},
    {{{"Texture.SkipMipmapCount", "1"}, {"TextureStreaming.FreeStreamingQuality", "1"},
      {"TextureCompositor.TextureCompositorQualityLevel", "1"}}},
    {{{"Texture.SkipMipmapCount", "0"}, {"TextureStreaming.FreeStreamingQuality", "2"},
      {"TextureCompositor.TextureCompositorQualityLevel", "2"}}},
    {{{"Texture.SkipMipmapCount", "0"}, {"TextureStreaming.FreeStreamingQuality", "3"},
      {"TextureCompositor.TextureCompositorQualityLevel", "3"}}},
}};

// Tier for a Tier Manager choice key such as "texture_quality_ultra"; unknown for anything else
// (including a custom choice), so a selection that is not recognised changes nothing.
constexpr Tier from_choice_key(std::string_view key) {
    constexpr std::string_view prefix = "texture_quality_";
    if (!key.starts_with(prefix)) return unknown;
    key.remove_prefix(prefix.size());
    if (key == "low") return low;
    if (key == "medium") return medium;
    if (key == "high") return high;
    if (key == "ultra") return ultra;
    return unknown;
}

constexpr const std::array<Setting, 3>* settings_for(Tier tier) {
    return tier >= low && tier <= ultra ? &tiers[static_cast<std::size_t>(tier)] : nullptr;
}
}
