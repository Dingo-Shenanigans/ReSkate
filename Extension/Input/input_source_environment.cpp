#include "input_source_environment.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/input_capture.h"
#include "Extension/UI/NativeMenu/throwdown_menu.h"
#include <Windows.h>
namespace dingosdk::native_input_source::environment {
Facts sample() noexcept {
    const auto error = GetLastError();
    DWORD process{};
    const auto foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &process);
    Facts result{GetTickCount64(), GetCurrentThreadId(),
        foreground && process == GetCurrentProcessId(),
        multiplayer::native_throwdown_gameplay_input_allowed(),
        !overlay::interface_open(), overlay::detail::passive_source_available()};
    SetLastError(error);
    return result;
}
}
