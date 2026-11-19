#include "local_settings_selection_fixture.h"
#include "Extension/Boot/offline_boot.h"
#include "Extension/Boot/user_data_redirect.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Game/Build/image_identity.h"
#include <cstdio>
#include <cstdlib>

namespace dingosdk::local_settings_selection_fixture {
Leaves& leaves() noexcept { static Leaves value; return value; }
[[noreturn]] void fail(const char* message) noexcept {
    std::fprintf(stderr, "FAIL actual local-settings selection: %s\n", message);
    std::abort();
}
void require(bool value, const char* message) noexcept { if (!value) fail(message); }
}

namespace dingosdk {
bool offline_boot_requested() noexcept { return local_settings_selection_fixture::leaves().global_requested; }
bool offline_boot_active(std::uintptr_t base) noexcept {
    local_settings_selection_fixture::require(base == local_settings_selection_fixture::image_base,
        "global boot checked a different image base");
    return local_settings_selection_fixture::leaves().global_active;
}
bool user_data_redirect_ready() noexcept { return local_settings_selection_fixture::leaves().redirect_ready; }
std::filesystem::path redirected_local_app_data() {
    return local_settings_selection_fixture::leaves().redirect_root / L"Game";
}

// Keep unused dependencies in another object so their fatal definitions cannot
// make the compiler infer unreachable production code under /W4 /WX.
HookBatch::HookBatch() noexcept { local_settings_selection_fixture::fail("unexpected hook transaction"); }
HookBatch::~HookBatch() { local_settings_selection_fixture::unexpected_hook_teardown(); }
HookStatus WINAPI hook_prepare(void*, void*, void**) { local_settings_selection_fixture::fail("unexpected hook preparation"); }
HookStatus WINAPI hook_enable(void*) { local_settings_selection_fixture::fail("unexpected hook enable"); }
HookStatus WINAPI hook_apply_queued() { local_settings_selection_fixture::fail("unexpected hook commit"); }
HookStatus WINAPI hook_remove(void*) { local_settings_selection_fixture::fail("unexpected hook removal"); }
}
namespace dingosdk::launcher {
bool offline_mode() noexcept {
    wchar_t value[2]{};
    const auto length = GetEnvironmentVariableW(L"RESKATE_OFFLINE", value, 2);
    return length == 1 && value[0] == L'1';
}
}
namespace dingosdk::supported_build {
bool running_image_matches(std::uintptr_t base) noexcept {
    auto& f = local_settings_selection_fixture::leaves();
    local_settings_selection_fixture::require(base == local_settings_selection_fixture::image_base,
        "supported-image probe used a different image base");
    ++f.image_probes;
    SetLastError(903);
    return false; // Deliberately stop selection before any image read or hook.
}
}
namespace dingosdk::logging {
bool enabled(Level) noexcept { local_settings_selection_fixture::fail("unexpected formatted logging"); }
void write(Level level, Channel channel, std::string_view message) noexcept {
    auto& f = local_settings_selection_fixture::leaves();
    local_settings_selection_fixture::require(f.expect_revocation_log && level == Level::warning && channel == Channel::runtime &&
        message.find("selected launch authority was revoked") != std::string_view::npos,
        "unexpected production log entry");
    ++f.revocation_logs;
    SetLastError(904);
}
void write(Level, Context, Channel, std::string_view) noexcept {
    local_settings_selection_fixture::fail("unexpected production log sink");
}
}
