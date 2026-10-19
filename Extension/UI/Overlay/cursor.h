#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
namespace dingosdk::overlay::detail {
// Whether the game's window is in front: as last sampled on the overlay's own thread (cursor.cpp),
// so no game thread waits on Windows for it.
bool game_window_foreground(HWND window);
// The keys held (a bit per virtual key) as last sampled, while the window is in front. False when
// there is no recent sample: the caller then asks Windows itself.
bool sampled_keys(HWND window, std::array<std::uint64_t, 4>& held);
void sync_menu_cursor(bool release = false);
UINT cursor_sync_message();
void request_window_cursor_sync(HWND window);
void window_cursor_input(HWND window, UINT message, WPARAM wp);
// The same, when the caller already knows whether the overlay owns the pointer
// (owns_menu_cursor), as the window procedure does once per mouse message.
void window_cursor_input(HWND window, UINT message, WPARAM wp, bool overlay_owns_pointer);
void update_menu_pointer();
BOOL WINAPI clip_cursor(const RECT* rectangle);
BOOL WINAPI set_cursor_pos(int x, int y);
}
