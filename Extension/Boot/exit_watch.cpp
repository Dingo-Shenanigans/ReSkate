#include "exit_watch.h"
#include "Engine/Core/Log/logging.h"
#include <atomic>
#include <thread>

namespace dingosdk::exit_watch {
namespace {
// How long a close may take once the window is gone or has stopped answering, and how long a
// window that still answers is watched (the game may ask before it closes, and be told no).
constexpr ULONGLONG grace_ms = 8000, watch_ms = 120000;
std::atomic<bool> watching{};
std::atomic<ULONGLONG> destroyed_at{};

void watch(HWND window) noexcept {
    const auto asked = GetTickCount64();
    ULONGLONG stuck_since{};
    for (;;) {
        Sleep(500);
        const auto now = GetTickCount64();
        DWORD_PTR result{};
        // Gone, or not taking messages: the close is under way, or stuck.
        const bool answers = !destroyed_at.load() && IsWindow(window) &&
                             SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &result);
        if (answers) {
            stuck_since = 0;
            // Still the game as it was: the close was turned down. Watch the next one afresh.
            if (now - asked >= watch_ms) { watching = false; return; }
            continue;
        }
        if (!stuck_since) stuck_since = now;
        if (now - stuck_since < grace_ms) continue;
        logging::log(logging::Level::warning, logging::Channel::runtime,
                     "The game did not finish closing {} s after it was asked to; ending it.", (now - asked) / 1000);
        logging::flush();
        TerminateProcess(GetCurrentProcess(), 0);
    }
}
} // namespace

void note_window_message(HWND window, UINT message, WPARAM wp) noexcept {
    const bool closing = message == WM_CLOSE || message == WM_DESTROY || (message == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE) ||
                         (message == WM_ENDSESSION && wp);
    if (!closing) return;
    if (message == WM_DESTROY) destroyed_at = GetTickCount64();
    if (watching.exchange(true)) return;
    try {
        std::thread(watch, window).detach();
    } catch (...) {
        watching = false;
    }
}
} // namespace dingosdk::exit_watch
