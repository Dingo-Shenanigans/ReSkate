#pragma once

#include <imgui.h>

#include <cstddef>

namespace dingosdk::overlay {

struct UiBackgroundImage {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
};

std::size_t reserve_ui_backgrounds(ImFontAtlas& atlas) noexcept;
bool fill_ui_backgrounds(ImFontAtlas& atlas) noexcept;
bool ui_background_image(unsigned index, UiBackgroundImage& image) noexcept;
void clear_ui_backgrounds() noexcept;

} // namespace dingosdk::overlay
