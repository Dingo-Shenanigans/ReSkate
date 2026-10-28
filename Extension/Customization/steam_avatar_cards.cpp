#include "steam_avatar_cards.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Vfs/https_download.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Music/spotify_cover.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <Windows.h>
#include <objbase.h>

namespace dingosdk::steam_avatar_cards {
namespace {
namespace md = multiplayer::menu_data;
using Clock = std::chrono::steady_clock;
using Address = std::uintptr_t;

// A player card (title, icon, background) and the player it shows, as the game's menus hold them.
constexpr std::uint32_t card_schema = 0x71a4f40bU;
constexpr std::uint32_t card_icon = 0x2cd9e178U, icon_image = 0x87d1682cU, image_url = 0xd861ad3fU;
constexpr std::uint32_t card_player = 0x81d5aa57U;
constexpr std::uint16_t player_steam_id = 0x40;   // the player record's SteamID64
// A cosmetic item as the menus list it: its key and its tile's picture.
constexpr std::uint32_t item_schema = 0x389117efU, item_key = 0x7199d8edU, item_picture = 0xd4f0f044U;

bool steam_picture_url(std::string_view url) {
    return url.starts_with("https://avatars.") && url.find(".steamstatic.com/") != std::string_view::npos;
}

// A Steam account's profile picture, from its public profile (no key needed), fetched once in the
// background and served from ReSkate's own loopback image server (spotify_cover.h): the game then
// shows it at once every time a menu makes the card again, instead of fetching it from Steam.
// Empty until then, and for an account without one.
struct Pictures { std::mutex mutex; std::map<std::uint64_t, std::string> urls; std::set<std::uint64_t> asked; };
Pictures& pictures() { static auto* value = new Pictures; return *value; }
std::string picture(std::uint64_t id) {
    if (!multiplayer::individual_steam_id(id)) return {};
    auto& p = pictures();
    {
        std::lock_guard lock(p.mutex);
        if (const auto found = p.urls.find(id); found != p.urls.end()) return found->second;
        if (!p.asked.insert(id).second) return {};
    }
    std::thread([id] {
        std::string url, served;
        try {
            const auto profile = https::get_text(L"https://steamcommunity.com/profiles/" + std::to_wstring(id) + L"?xml=1",
                                                 512 * 1024, 10, L"ReSkate/1");
            constexpr std::string_view open = "<avatarFull><![CDATA[";
            if (profile)
                if (const auto at = profile->find(open); at != std::string::npos) {
                    const auto start = at + open.size(), end = profile->find("]]>", start);
                    if (end != std::string::npos) url = profile->substr(start, end - start);
                }
            // Only Steam's own image servers.
            if (!steam_picture_url(url)) url.clear();
            if (!url.empty())
                if (const auto image = https::get_text(std::wstring(url.begin(), url.end()), 2 * 1024 * 1024, 10, L"ReSkate/1")) {
                    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
                    served = spotify_cover::serve(std::vector<std::uint8_t>(image->begin(), image->end()),
                                                  "steam-" + std::to_string(id) + ".png");
                    if (com) CoUninitialize();
                }
        } catch (...) { served.clear(); }
        // Served here when it could be; else Steam's own address, which the game fetches itself.
        const auto shown = served.empty() ? url : served;
        logging::log(logging::Level::info, logging::Channel::ui, "Steam picture for {}: {}", id,
                     shown.empty() ? std::string("none") : shown);
        auto& p = pictures();
        std::lock_guard lock(p.mutex);
        p.urls[id] = shown;
    }).detach();
    return {};
}
// Whether a card shows a Steam picture this gave it.
bool given(std::string_view shown) {
    if (steam_picture_url(shown)) return true;
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    for (const auto& [id, url] : p.urls)
        if (!url.empty() && url == shown) return true;
    return false;
}

struct State {
    Clock::time_point next, next_item_scan;
    bool logged{};
    md::Value item; // the icon's item record, once found
    Address item_manager{};
};
State& state() { static auto* value = new State; return *value; }
} // namespace

bool enabled() noexcept {
    // The launcher says "0" when its switch is off.
    static const bool value = [] {
        wchar_t text[2]{};
        const auto length = GetEnvironmentVariableW(L"RESKATE_STEAM_CARD_PICTURES", text, 2);
        return !(length == 1 && text[0] == L'0');
    }();
    return value;
}

void tick(std::uintptr_t base) noexcept {
    auto& s = state();
    if (!base || !enabled()) return;
    const auto now = Clock::now();
    if (now < s.next) return;
    // Cards are only in the game's menus: nothing to look for in play (whether a menu is up is sampled
    // anyway, and cheap). In a menu, often enough that a card the menu has just made shows the picture
    // before its own icon is noticed.
    if (!multiplayer::sample_game_ui_state(base).in_menu) {
        s.next = now + std::chrono::milliseconds(100);
        return;
    }
    s.next = now + std::chrono::milliseconds(150);
    try {
        const auto ui = md::read<Address>(base + addr::engine::ui_manager);
        const auto manager = ui ? md::read<Address>(ui + 0x140) : 0;
        if (!manager || !game::native_data().models.value) return;
        const md::Context c(base, manager);
        game::ModelWriteLock lock(manager);
        unsigned cards = 0, set = 0;
        const auto icon = "baseitem:" + std::string(icon_key);
        for (const auto& root : c.roots({card_schema})) {
            try {
                ++cards;
                // Only a card wearing the icon: its own picture (as the game set it), or the Steam picture
                // given before (the game keeps the model while the menu is up).
                const auto field = c.path(root.model, {card_icon, icon_image, image_url});
                const auto shown = c.text(field, 512);
                if (shown != icon && !given(shown)) continue;
                const auto link = c.field(root.model, card_player);
                const auto handle = md::read<md::Handle>(c.address(link));
                if (!handle) continue;
                const md::Value player{handle, c.type_of(handle)};
                const auto id = md::read<std::uint64_t>(c.address(player) + player_steam_id);
                const auto url = picture(id);
                if (url.empty() || shown == url) continue;
                c.text(field, url);
                ++set;
            } catch (const std::exception&) { /* that card keeps its own icon */ }
        }
        // The icon's tile in the menus shows the player's own Steam picture. Its item record is looked
        // for among every item's only now and then (there are thousands), and kept while it lasts.
        if (const auto snapshot = multiplayer::steam_social_snapshot(); snapshot && snapshot->local.id) {
            if (const auto mine = picture(snapshot->local.id); !mine.empty()) {
                if (s.item_manager != manager || !s.item.handle || c.type_of(s.item.handle) != s.item.type) {
                    s.item = {};
                    s.item_manager = manager;
                    if (now >= s.next_item_scan) {
                        s.next_item_scan = now + std::chrono::seconds(2);
                        for (const auto& root : c.roots({item_schema}))
                            try {
                                if (c.text(c.field(root.model, item_key), 64) == icon_key) { s.item = root.model; break; }
                            } catch (const std::exception&) {}
                    }
                }
                if (s.item.handle)
                    try {
                        const auto tile = c.field(s.item, item_picture);
                        if (c.text(tile, 512) != mine) c.text(tile, mine);
                    } catch (const std::exception&) { s.item = {}; }
            }
        }
        if (set || (!s.logged && cards)) {
            s.logged = true;
            logging::log(logging::Level::info, logging::Channel::ui, "Steam picture cards: {} card(s), {} given a picture.", cards, set);
        }
    } catch (...) {}
}
} // namespace dingosdk::steam_avatar_cards
