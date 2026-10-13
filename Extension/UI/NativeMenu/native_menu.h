#pragma once
#include <cstdint>
#include <string>

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
// Whether one of ReSkate's pages (Multiplayer, Mod Options) is the pause menu's page on
// screen right now, and which. Any thread: the overlay draws the page from it
// (overlay::HubPageFeed).
overlay::HubPage native_menu_page() noexcept;
} // namespace dingosdk::multiplayer
