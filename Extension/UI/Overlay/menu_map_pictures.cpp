#include "overlay_internal.h"
#include "skate_menu_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The pictures on the menu's map grid (Map > Travel). The grid asks with the levels it lists;
// Assets/map_icons.cpp answers off the game's thread (an official map's loading screen, a custom
// map's mod icon), and the pictures go into one texture, a square each, in the SRV heap's third
// slot (overlay_render.cpp, upload_menu_map_image).
namespace dingosdk::overlay {
namespace {
constexpr UINT cell = 256, columns = 8;
constexpr std::size_t most = 64; // 4 MB of pixels at most

std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

struct Pictures {
    std::mutex mutex;
    std::string asked;                       // the levels last asked for, one asset a line
    std::vector<MenuMapWanted> request;      // not yet taken by map_icons
    // The texture: the pixels kept to upload again after a device reset, and where each level's
    // picture is in them (by folded asset). `ready` are those of the pixels on the GPU now.
    detail::CardPixels current, waiting_pixels;
    std::map<std::string, UINT, std::less<>> waiting_cells, ready_cells;
    UINT width{}, height{}, ready_width{}, ready_height{};
    bool waiting{};
    unsigned serial{};
    ImTextureID texture{};
};
Pictures& pictures() { static auto* value = new Pictures; return *value; } // process lifetime
}

void want_menu_map_pictures(std::vector<MenuMapWanted> levels) {
    std::string key;
    for (const auto& level : levels) key += folded(level.asset) + '\n';
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (key == p.asked) return;
    p.asked = std::move(key);
    if (levels.size() > most) levels.resize(most);
    p.request = std::move(levels);
}

std::vector<MenuMapWanted> take_menu_map_request() noexcept {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    return std::exchange(p.request, {});
}

void set_menu_map_pictures(const std::vector<MapVoteIcon>& found) noexcept {
    try {
        std::vector<std::pair<std::string, detail::CardPixels>> squares;
        for (const auto& icon : found) {
            if (squares.size() >= most) break;
            detail::CardPixels one;
            bool ok{};
            if (icon.side == cell && icon.rgba.size() == std::size_t{cell} * cell * 4) {
                one.rgba = icon.rgba;
                one.width = one.height = cell;
                ok = true;
            } else if (!icon.png.empty() && icon.png.size() <= 4 * 1024 * 1024) {
                ok = detail::decode_picture(icon.png, one, cell) && one.width == cell && one.height == cell;
            }
            if (ok) squares.emplace_back(folded(icon.name), std::move(one));
        }
        auto& p = pictures();
        std::lock_guard lock(p.mutex);
        if (squares.empty()) return;
        const auto count = static_cast<UINT>(squares.size());
        detail::CardPixels atlas;
        atlas.width = cell * std::min(count, columns);
        atlas.height = cell * ((count + columns - 1) / columns);
        atlas.rgba.assign(std::size_t{atlas.width} * atlas.height * 4, 0);
        std::map<std::string, UINT, std::less<>> cells;
        for (UINT i = 0; i < count; ++i) {
            const UINT x = (i % columns) * cell, y = (i / columns) * cell;
            for (UINT row = 0; row < cell; ++row)
                std::memcpy(atlas.rgba.data() + (std::size_t{y + row} * atlas.width + x) * 4,
                            squares[i].second.rgba.data() + std::size_t{row} * cell * 4, std::size_t{cell} * 4);
            cells[squares[i].first] = i;
        }
        atlas.serial = ++p.serial;
        p.width = atlas.width;
        p.height = atlas.height;
        p.current = atlas;
        p.waiting_pixels = std::move(atlas);
        p.waiting_cells = std::move(cells);
        p.waiting = true;
    } catch (...) { /* No pictures: the grid shows each map's name on a plain tile. */ }
}

bool menu_map_picture(const std::string& asset, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (!p.texture || !p.ready_width || !p.ready_height) return false;
    const auto found = p.ready_cells.find(folded(asset));
    if (found == p.ready_cells.end()) return false;
    const float x = static_cast<float>((found->second % columns) * cell), y = static_cast<float>((found->second / columns) * cell);
    texture = p.texture;
    uv0 = ImVec2(x / static_cast<float>(p.ready_width), y / static_cast<float>(p.ready_height));
    uv1 = ImVec2((x + cell) / static_cast<float>(p.ready_width), (y + cell) / static_cast<float>(p.ready_height));
    return true;
}

namespace detail {
bool take_menu_map_pixels(CardPixels& out) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (!p.waiting) return false;
    p.waiting = false;
    out = std::exchange(p.waiting_pixels, {});
    return true;
}
void menu_map_pixels_uploaded(ImTextureID texture, unsigned serial) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (serial != p.serial) return;
    p.texture = texture;
    p.ready_cells = p.waiting_cells;
    p.ready_width = p.width;
    p.ready_height = p.height;
}
void menu_map_pixels_lost() {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    p.texture = {};
    if (!p.current.rgba.empty()) {
        p.waiting_pixels = p.current;
        p.waiting = true;
    }
}
}
}
