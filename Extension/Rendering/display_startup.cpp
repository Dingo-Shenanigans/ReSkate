#include "Engine/Core/Log/logging.h"
#include "display_startup.h"
#include "Extension/Scripting/lua_startup.h"

#include <Windows.h>
#include <string>

namespace dingosdk {
namespace {
std::string display_script;

bool apply_display(const lua_startup::Context& context) {
    const bool applied = context.execute(display_script);
    logging::event(logging::Channel::graphics,
        std::string("{\"event\":\"display_startup_settings\",\"stage\":\"after_startup_script\",\"tick_ms\":") +
        std::to_string(GetTickCount64()) + ",\"applied\":" +
        (applied ? "true" : "false") + "}");
    return applied;
}

unsigned dimension(const wchar_t* name, unsigned minimum) {
    wchar_t value[16]{};
    const auto count = GetEnvironmentVariableW(name, value, 16);
    if (!count || count >= 16) return 0;
    unsigned result = 0;
    for (DWORD index = 0; index < count; ++index) {
        if (value[index] < L'0' || value[index] > L'9' || result > 16384) return 0;
        result = result * 10 + static_cast<unsigned>(value[index] - L'0');
    }
    return result >= minimum && result <= 16384 ? result : 0;
}
}

bool start_display_settings(std::uintptr_t base) {
    const auto width = dimension(L"RESKATE_WINDOW_WIDTH", 320);
    const auto height = dimension(L"RESKATE_WINDOW_HEIGHT", 200);
    if (!width && !height) return true; // Respect saved settings unless explicitly overridden.
    if (!width || !height) return false;
    display_script = "Window=Window or {}\nRenderDevice=RenderDevice or {}\nWindow.Width=" + std::to_string(width) +
        "\nWindow.Height=" + std::to_string(height) +
        "\nRenderDevice.FullscreenWidth=" + std::to_string(width) +
        "\nRenderDevice.FullscreenHeight=" + std::to_string(height) +
        "\nWindow.AutoSize=false\nWindow.FullscreenAutoSize=false\nWindow.AllowWindowsLargerThanDesktop=true"
        "\nRenderDevice.FullscreenModeEnable=false\nRenderDevice.WindowedBorderless=false";
    std::string error;
    const bool ready = lua_startup::add_callback(base, &apply_display, error);
    if (!ready) logging::log(logging::Level::error, logging::Channel::graphics, "Display startup hook: {}", error);
    return ready;
}
}
