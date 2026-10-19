#include "map_icons.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ui_textures.h"
#include "Engine/Vfs/game_textures.h"
#include "Engine/Vfs/https_download.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Vfs/thunderstore_package.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dingosdk::map_icons {
namespace {
namespace fs = std::filesystem;
namespace package = thunderstore_package;

constexpr wchar_t agent[] = L"ReSkate";
constexpr char community[] = "reskate";
constexpr std::uintmax_t largest_icon = 4 * 1024 * 1024;

std::atomic<unsigned> wanted_serial{~0U}; // the server's list the pictures in hand are for
std::atomic<bool> fetching{};
// Icons already found this run, by package without its version: a pool's maps come round again.
std::mutex cache_mutex;
std::map<std::string, std::string> cache;

std::string read_bytes(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || !size || size > largest_icon) return {};
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// One map's icon as PNG bytes, or nothing: the game's own maps have none, and a package that
// is neither installed nor on Thunderstore has none to give.
std::string icon_of(const std::string& name_text) {
    const auto name = package::parse_name(name_text);
    if (!name) return {};
    const auto folder = name->folder();
    {
        std::lock_guard lock(cache_mutex);
        if (const auto found = cache.find(folder); found != cache.end()) return found->second;
    }
    std::string bytes;
    try {
        bytes = read_bytes(mods::engine_data_root() / mods::mods_folder / folder / L"icon.png");
        if (bytes.empty()) {
            if (const auto answer = https::get_text(package::details_url(*name), 512 * 1024, 15, agent)) {
                const auto choice = package::choose(*answer, *name, community);
                if (!choice.icon.empty())
                    if (const auto png = https::get_text(std::wstring(choice.icon.begin(), choice.icon.end()), 2 * 1024 * 1024, 8, agent))
                        bytes = *png;
            }
        }
    } catch (const std::exception&) {
        bytes.clear();
    }
    std::lock_guard lock(cache_mutex);
    cache[folder] = bytes; // (an empty one too: it is not asked for again this run)
    return bytes;
}

// One of the game's own maps: the picture on its loading screen, cut to the square in its
// middle and scaled to the overlay's size. Nothing for a map without one, or when it cannot be
// read (another game build).
constexpr std::uint32_t picture_side = 256;
std::vector<unsigned char> game_picture(const std::string& map) {
    namespace textures = addr::ui_textures;
    for (const auto& picture : textures::map_pictures) {
        if (picture.map != map) continue;
        static std::mutex pictures_mutex;
        static std::map<std::string, std::vector<unsigned char>> pictures;
        std::lock_guard lock(pictures_mutex);
        if (const auto found = pictures.find(map); found != pictures.end()) return found->second;
        std::vector<unsigned char> square;
        try {
            vfs::GameTextures reader(mods::engine_data_root());
            // Fitted into 512 a side with its shape kept: 512 by 288.
            const auto wide = reader.read(textures::map_picture_toc, picture.bundle, picture.name, picture_side * 2);
            const auto side = std::min(wide.width, wide.height);
            if (!side) throw std::runtime_error("an empty picture");
            frostbite::Image cut;
            cut.width = cut.height = side;
            cut.rgba.resize(static_cast<std::size_t>(side) * side * 4);
            const auto left = (wide.width - side) / 2, top = (wide.height - side) / 2;
            for (std::uint32_t row = 0; row < side; ++row)
                std::copy_n(wide.rgba.begin() + (static_cast<std::size_t>(top + row) * wide.width + left) * 4, static_cast<std::size_t>(side) * 4,
                            cut.rgba.begin() + static_cast<std::size_t>(row) * side * 4);
            const auto scaled = side == picture_side ? cut : frostbite::resize(cut, picture_side, picture_side);
            square.assign(scaled.rgba.begin(), scaled.rgba.end());
            // (A loading screen's picture is opaque; make sure of it.)
            for (std::size_t i = 3; i < square.size(); i += 4) square[i] = 255;
        } catch (const std::exception& error) {
            square.clear();
            try {
                logging::log(logging::Level::warning, logging::Channel::assets, "Next map vote: no picture for {}: {}.", map, error.what());
            } catch (...) {}
        }
        pictures[map] = square; // (an empty one too: it is not read again this run)
        return square;
    }
    return {};
}

void fetch(unsigned serial, std::vector<multiplayer::NextMapWanted> maps) {
    std::vector<overlay::MapVoteIcon> icons;
    std::size_t found{};
    for (auto& map : maps) {
        overlay::MapVoteIcon icon;
        icon.png = icon_of(map.package);
        if (icon.png.empty() && map.package.empty()) {
            icon.rgba = game_picture(map.name);
            if (!icon.rgba.empty()) icon.side = picture_side;
        }
        found += !icon.png.empty() || !icon.rgba.empty();
        icon.name = std::move(map.name);
        icons.push_back(std::move(icon));
    }
    // (The server may have moved on meanwhile: the next tick fetches for what it says now.)
    if (wanted_serial.load() == serial) {
        overlay::set_map_vote_icons(icons);
        try {
            logging::log(logging::Level::info, logging::Channel::assets, "Next map vote: {} map(s) to choose from, {} with a picture.", icons.size(), found);
        } catch (...) {}
    }
    fetching.store(false);
}

// The menu's map grid: each level's picture, a custom map's mod icon (its path found on the client
// thread) or an official map's loading screen.
std::atomic<bool> menu_fetching{};
void fetch_menu(std::vector<std::pair<overlay::MenuMapWanted, fs::path>> maps) {
    std::vector<overlay::MapVoteIcon> icons;
    for (auto& [map, icon_path] : maps) {
        overlay::MapVoteIcon icon;
        if (!icon_path.empty()) icon.png = read_bytes(icon_path);
        if (icon.png.empty()) {
            icon.rgba = game_picture(map.name);
            if (!icon.rgba.empty()) icon.side = picture_side;
        }
        if (icon.png.empty() && icon.rgba.empty()) continue;
        icon.name = std::move(map.asset);
        icons.push_back(std::move(icon));
    }
    overlay::set_menu_map_pictures(icons);
    try {
        logging::log(logging::Level::info, logging::Channel::assets, "Menu map grid: {} of {} map(s) with a picture.", icons.size(), maps.size());
    } catch (...) {}
    menu_fetching.store(false);
}
bool same_asset(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
void menu_tick() {
    if (menu_fetching.load()) return;
    auto wanted = overlay::take_menu_map_request();
    if (wanted.empty()) return;
    std::vector<std::pair<overlay::MenuMapWanted, fs::path>> maps;
    const auto& catalog = mods::catalog();
    for (auto& map : wanted) {
        fs::path icon;
        for (const auto& mod : catalog.mods)
            if (std::ranges::any_of(mod.levels, [&](const std::string& level) { return same_asset(level, map.asset); })) {
                icon = mod.directory / L"icon.png";
                break;
            }
        maps.emplace_back(std::move(map), std::move(icon));
    }
    menu_fetching.store(true);
    std::thread(fetch_menu, std::move(maps)).detach();
}
} // namespace

void tick() {
    menu_tick();
    const auto serial = multiplayer::next_maps_serial();
    if (serial == wanted_serial.load() || fetching.load()) return;
    wanted_serial.store(serial);
    auto maps = multiplayer::next_maps();
    if (maps.empty()) return overlay::set_map_vote_icons({});
    fetching.store(true);
    // Detached, as the map download's work is: the game may close while it runs.
    std::thread(fetch, serial, std::move(maps)).detach();
}
} // namespace dingosdk::map_icons
