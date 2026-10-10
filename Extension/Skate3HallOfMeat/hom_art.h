#pragma once
#include <cstddef>
#include <imgui.h>
#include <string_view>

#include <filesystem>

// Hall Of Meat HUD art. ReSkate ships no game data: Skate 3's art, X-ray meshes and sounds come in a
// mod (Mods/<mod>/HallOfMeat/, see hom_directory) and the PNGs are packed into the ImGui font atlas,
// like the chat emotes.
namespace dingosdk::overlay {
// Where Hall Of Meat's assets are: the HallOfMeat folder of the first enabled mod that has one (its
// bones.bin), else <game>/HallOfMeat. Empty when there is none, and then Hall Of Meat is off.
// Rescans the Mods folder at most every two seconds, so turning the mod off in the MODS tab
// switches Hall Of Meat off.
std::filesystem::path hom_directory() noexcept;
struct HomArt {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
    float width{}, height{};
};
// Render thread, before the atlas is built: reads the PNGs and reserves their room.
std::size_t reserve_hom_art(ImFontAtlas &) noexcept;
// After the atlas is built: copies the pixels in.
void fill_hom_art(ImFontAtlas &) noexcept;
// Skate 3's HUD typeface (Futura Std Heavy) at a pixel size it was baked for, or null when the
// font files are not there. Sizes: 22, 28, 60.
ImFont *hom_font(int size) noexcept;
// The picture called `name` (file name without .png), or false when it is not there.
bool hom_art(std::string_view name, HomArt &art) noexcept;
} // namespace dingosdk::overlay
