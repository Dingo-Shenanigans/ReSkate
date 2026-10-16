#pragma once
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

// The icons the player's own item tabs can have (item_browser_overlay.cpp): the game's own category
// icons, read from the installed game at start-up ("game_tops", "game_shoes", ...), and ReSkate's
// own for the kinds the game has none for ("ring", "earring", "scarf", ...), built into ReSkate.dll
// from assets/tab_icons (cmake/Runtime.cmake). White on clear: drawn tinted.
namespace dingosdk::overlay {
// Starts reading them on a background thread. Call once, early.
void start_tab_icons(std::filesystem::path game_root);
// Render-thread setup, as the chat emotes: room in the atlas before it is built, pixels after.
std::size_t reserve_tab_icons(ImFontAtlas &, std::chrono::milliseconds wait) noexcept;
void fill_tab_icons(ImFontAtlas &) noexcept;
struct TabIcon {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
};
bool tab_icon(std::string_view name, TabIcon &icon);
// Every icon there is, the game's first, in a fixed order.
std::vector<std::string> tab_icon_names();
} // namespace dingosdk::overlay
