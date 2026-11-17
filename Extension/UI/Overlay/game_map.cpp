#include "game_map.h"
#include "Extension/Skater/xray_skeleton.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/game_map.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_list.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include <utility>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dingosdk::overlay {
namespace {
constexpr std::uint32_t window_side = 768; // pixels a side of the part held on the GPU
constexpr float most_range = 2000;         // metres: more is the whole of any map

// A part of the map on its way to the GPU or there: the square of the world it is.
struct Window {
    std::string key; // the picture's, in the plugins' picture cache
    float x{}, z{}, metres{};
    bool covers(float at_x, float at_z, float reach) const noexcept {
        return std::abs(at_x - x) + reach <= metres / 2 && std::abs(at_z - z) + reach <= metres / 2;
    }
};
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    bool worker{};
    std::string level;                    // the one asked for
    std::uint64_t generation{};           // counts the levels: what was read for an earlier one is dropped
    bool level_read{};                    // the worker has looked at this level
    std::optional<GameMapArea> area;      // its map's, once read
    std::optional<Window> shown, coming;  // the part drawn, and the next one (made, waiting for the GPU)
    std::optional<Window> wanted;         // the part the worker is to make
    bool making{};
    unsigned serial{};
};
State& state() {
    static auto* value = new State; // the process's lifetime: the worker never ends
    return *value;
}

void work() {
    auto& s = state();
    std::unique_ptr<vfs::GameMapReader> reader;
    std::uint64_t reading = 0;
    for (;;) {
        std::string level;
        std::uint64_t generation{};
        std::optional<Window> make;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [&] { return !s.level_read || s.wanted; });
            level = s.level;
            generation = s.generation;
            if (s.level_read && reading == generation) {
                make = std::exchange(s.wanted, std::nullopt);
                s.making = true;
            }
        }
        if (!make) {
            // A level came up: its map, if it has one.
            reader.reset();
            std::optional<GameMapArea> area;
            try {
                if (!level.empty()) {
                    // (With the merged mods' data over the game's: a level a mod adds is there.)
                    const auto mods = mods::engine_data_root() / L"Mods" / mods::generated_folder;
                    if (auto map = vfs::read_game_map(xray::game_folder(), level, mods)) {
                        area = GameMapArea{map->centre_x, map->centre_z, map->width, map->height};
                        logging::log(logging::Level::info, logging::Channel::plugins, "The game's map of {}: {}, {:.0f} x {:.0f} m about ({:.0f}, {:.0f}).", level,
                                     map->name, map->width, map->height, map->centre_x, map->centre_z);
                        reader = std::make_unique<vfs::GameMapReader>(xray::game_folder(), std::move(*map), mods);
                    }
                }
            } catch (const std::exception& error) {
                logging::log(logging::Level::warning, logging::Channel::plugins, "The game's map of {} could not be read: {}.", level, error.what());
                reader.reset();
            } catch (...) {
                reader.reset();
            }
            std::lock_guard lock(s.mutex);
            if (s.generation != generation) continue; // another level since: read again
            reading = generation;
            s.level_read = true;
            s.area = reader ? area : std::nullopt;
            continue;
        }
        detail::CardPixels pixels;
        try {
            if (reader) {
                auto image = reader->window(make->x, make->z, make->metres, window_side);
                pixels.rgba = std::move(image.rgba);
                pixels.width = image.width;
                pixels.height = image.height;
            }
        } catch (const std::exception& error) {
            // (Said once a level: a part of the map that cannot be made left the minimap empty with no word of why.)
            static std::uint64_t said = 0;
            if (std::exchange(said, generation) != generation)
                logging::log(logging::Level::warning, logging::Channel::plugins, "The game's map of {}: a part of it could not be made: {}.", level, error.what());
            pixels = {};
        } catch (...) {
            pixels = {};
        }
        std::lock_guard lock(s.mutex);
        s.making = false;
        if (s.generation != generation || pixels.rgba.empty()) continue;
        pixels.serial = ++s.serial;
        if (detail::plugin_pixels(make->key, std::move(pixels))) s.coming = std::move(*make);
    }
}
}

void set_game_map_level(std::string_view level) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.level == level && s.generation) return;
        s.level = level;
        ++s.generation;
        s.level_read = false;
        s.area.reset();
        s.shown.reset();
        s.coming.reset();
        s.wanted.reset();
        if (!std::exchange(s.worker, true)) std::thread(work).detach();
        s.wake.notify_one();
    } catch (...) {
    }
}

std::optional<GameMapArea> game_map_area() noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        return s.area;
    } catch (...) {
        return std::nullopt;
    }
}

bool draw_game_map(ImDrawList* draw, ImVec2 min, ImVec2 max, float x, float z, float range, float heading, ImU32 tint) {
    if (!draw || !(max.x > min.x) || !(max.y > min.y) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(heading)) return false;
    range = std::clamp(range, 5.0f, most_range);
    // Turned any way, the box shows this far from its middle (its corners).
    const float reach = range * 1.4143f * std::max(1.0f, (max.y - min.y) / (max.x - min.x));
    auto& s = state();
    Window window;
    ImTextureID texture{};
    {
        std::lock_guard lock(s.mutex);
        if (!s.area) return false;
        // The next part takes over once it is on the GPU.
        if (s.coming) {
            if (const auto id = detail::plugin_pixels_id(s.coming->key)) {
                s.shown = std::exchange(s.coming, std::nullopt);
                texture = id;
            }
        }
        if (s.shown && !texture) texture = detail::plugin_pixels_id(s.shown->key);
        if (!texture) s.shown.reset(); // (its place was given to another picture: made again)
        // Another part, once what is shown has what is drawn within a quarter of its reach of its edge
        // (so there is time to make it), or is far larger than is drawn (the range was turned down).
        const bool fits = s.shown && s.shown->covers(x, z, reach * 1.25f) && s.shown->metres <= reach * 6;
        if (!fits && !s.making && !s.wanted && !s.coming) {
            Window next;
            // (Two names, taken in turn: the part shown stays up while the next is made and sent.)
            next.key = s.shown && s.shown->key == "reskate:game map a" ? "reskate:game map b" : "reskate:game map a";
            next.x = x;
            next.z = z;
            next.metres = reach * 3;
            s.wanted = std::move(next);
            s.wake.notify_one();
        }
        if (!s.shown || !texture) return false;
        window = *s.shown;
    }
    // The world about (x, z) on the box: what is faced is upward, and the game's right-hand side of
    // that is to the right (the map's own right is +x and its top -z, so facing -z it is unturned).
    const float fx = std::sin(heading), fz = std::cos(heading);
    const ImVec2 middle((min.x + max.x) / 2, (min.y + max.y) / 2);
    const float scale = (max.x - min.x) / 2 / range;
    const auto place = [&](float wx, float wz) {
        const float dx = wx - x, dz = wz - z;
        return ImVec2(middle.x + (dx * -fz + dz * fx) * scale, middle.y - (dx * fx + dz * fz) * scale);
    };
    const float half = window.metres / 2;
    draw->PushClipRect(min, max, true);
    draw->AddImageQuad(texture, place(window.x - half, window.z - half), place(window.x + half, window.z - half), place(window.x + half, window.z + half),
                       place(window.x - half, window.z + half), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
    draw->PopClipRect();
    return true;
}
}
