#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace dingosdk::overlay { struct CallbacksV3; struct HubPage; }

namespace dingosdk::multiplayer {
// Call after the session tick, including while disconnected. All UI model
// operations execute on the client thread under the native model lock.
void tick_native_menu(std::uintptr_t base, bool loading) noexcept;
// Run on the client thread BEFORE submitting a map load, while widget assets
// and dynamic schemas still exist. Returns false if cleanup could not finish.
bool prepare_native_menu_level_load(std::uintptr_t base) noexcept;
// Register with the validated client-transition hook, before the native handler
// releases assets. Suspends updates through native loads, permanently on exit.
void native_menu_before_level_transition(std::uintptr_t base, unsigned next) noexcept;
// Uses the same validated runtime queues as the overlay. The reader must be a
// snapshot reader; it must not consume the overlay's console log cursor.
void set_native_menu_callbacks(const overlay::CallbacksV3& callbacks);
// Whether one of ReSkate's pages (Multiplayer, Mod Options, Mod Manager) is the pause menu's page on
// screen right now, and which. Any thread: the overlay draws the page from it
// (overlay::HubPageFeed).
overlay::HubPage native_menu_page() noexcept;
// While held, the Mod Manager page's Back does nothing, so B and Escape stay the overlay's: they
// leave a mod's page for the list instead of closing the pause menu. Any thread.
void hold_native_menu_back(bool held) noexcept;
// Whether the game's own pause menu is up, on any of its pages. Any thread.
bool native_pause_menu_open() noexcept;
// Goes to one of ReSkate's pages in the pause menu (`slot` 0 Multiplayer, 1 Mod Options, 2 Mod
// Manager), as picking its tab does; nothing when that page is not there. Any thread.
void show_native_menu_page(int slot) noexcept;
// The game's HUB tab, under ReSkate's front page (native_hub.cpp, Extension/UI/Overlay/hub_home_page.cpp).
// Opens the player's profile as the HUB's Profile tile did; the front page is hidden until the player
// leaves it. Any thread.
void open_native_profile() noexcept;
// While held (asked again at least every 300 ms), the HUB's Back does nothing, so B and Escape close
// what the front page has open instead of the pause menu. Any thread.
void hold_hub_back(bool held) noexcept;
// Whether ReSkate's front page is drawn over the HUB (on unless the player turned it off: the
// overlay passes the saved choice on). Off, the HUB shows as the game leaves it. Any thread.
void set_hub_front_page(bool on) noexcept;
// The game's 3D menu room behind the Mod Manager tab ("off", "cas", "store", "skatepass",
// "skatepedia"; "store" by default; the overlay passes the player's saved choice on). Returns what
// it now does. Any thread.
std::string set_mod_backdrop(std::string_view choice);
} // namespace dingosdk::multiplayer
