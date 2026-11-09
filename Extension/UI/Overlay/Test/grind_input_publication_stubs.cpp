#include "Extension/UI/Overlay/overlay_internal.h"
#include "Extension/UI/Overlay/input_capture.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Engine/Core/Log/logging.h"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unrelated_entry() {
    std::fputs("FAIL unrelated UI or hook entry reached\n", stderr);
    std::abort();
}
}
// Source inclusion still names these unrelated leaves. Keep their fail-fast
// definitions separate so optimization does not diagnose the unused UI as unreachable.
namespace dingosdk::logging {
bool enabled(Level) noexcept { unrelated_entry(); }
void write(Level, Context, Channel, std::string_view) noexcept { unrelated_entry(); }
void write(Level, Channel, std::string_view) noexcept { unrelated_entry(); }
void printf(Level, Channel, const char*, ...) noexcept { unrelated_entry(); }
}
namespace dingosdk::overlay::detail {
thread_local unsigned overlay_input_access{};
State& state() { unrelated_entry(); }
bool install(Hook&, void*, void*) { unrelated_entry(); }
bool interactive_visible(const State&) { unrelated_entry(); }
bool owns_menu_cursor(const State&) { unrelated_entry(); }
}
namespace dingosdk::overlay {
bool scripted_pad_active() noexcept { unrelated_entry(); }
bool scripted_pad(DWORD, XINPUT_STATE&, input::PadAuthorship*, bool) noexcept { unrelated_entry(); }
bool held_stick(int&, SHORT&) noexcept { unrelated_entry(); }
}
namespace dingosdk::multiplayer {
bool native_throwdown_gameplay_input_allowed() noexcept { unrelated_entry(); }
}
namespace dingosdk::throwdowns::kit {
Match match() noexcept { unrelated_entry(); }
}
