#pragma once
#include <Windows.h>

namespace dingosdk::exit_watch {
// The game sometimes never finishes closing: its window goes (or stops answering) and the
// process stays, holding the game's files and Steam's "running" state. Once the player has
// asked the game to close, this gives it a few seconds and then ends the process.
// Called from the game window's procedure with each message of that window.
void note_window_message(HWND window, UINT message, WPARAM wp) noexcept;
}
