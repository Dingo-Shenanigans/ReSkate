#pragma once
#include "Extension/Rendering/texture_tier.h"
#include <cstdint>
#include <string>

namespace dingosdk {
// The Texture Quality the player chose, as the game saved it: the "Texture Quality" entry of its
// TierManagerUserSettings (Launcher/game_settings.h). The game writes that save as soon as the
// graphics menu changes, Ultra included, so it is the choice the menu shows. The save is read
// again only when its files change. `generation` increases each time it is read; it stays 0 while
// there is no save, and `tier` is unknown when the save holds no recognised Texture Quality.
// `filtering` is the saved Texture Filtering choice key ("texture_filtering_..."), empty if none.
struct SavedTextureQuality {
    texture_tier::Tier tier{texture_tier::unknown};
    std::string filtering;
    std::uint32_t generation{};
};
SavedTextureQuality saved_texture_quality();
}
