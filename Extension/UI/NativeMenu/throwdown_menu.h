#pragma once
#include <cstdint>
namespace dingosdk::multiplayer {
void tick_native_throwdown_menu(std::uintptr_t base, bool loading, bool in_menu) noexcept;
bool release_native_throwdown_menu(std::uintptr_t base) noexcept;
bool native_throwdown_setup_open() noexcept;
bool native_throwdown_hud_ready() noexcept;
bool native_throwdown_match_panel_open() noexcept;
bool native_throwdown_gameplay_input_allowed() noexcept;
bool native_throwdown_match_active() noexcept;
// Prepare an owned request at native page publication or its client/UI tick.
// Presentation is independent of the native flow's camera/input confirmation.
void prepare_native_throwdown_match_panel() noexcept;
void observe_native_custom_join_details(std::uintptr_t vm) noexcept;
void observe_native_flag_interaction(std::uintptr_t vm) noexcept;
}
