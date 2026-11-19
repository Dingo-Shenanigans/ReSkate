#pragma once
#include <cstdint>
#include <filesystem>

namespace dingosdk::local_settings_selection_fixture {
inline constexpr std::uintptr_t image_base = 0x10000000;
struct Leaves {
    bool global_requested{true}, global_active{true}, redirect_ready{true}, expect_revocation_log{};
    std::filesystem::path redirect_root;
    unsigned image_probes{}, revocation_logs{};
};
Leaves& leaves() noexcept;
[[noreturn]] void fail(const char* message) noexcept;
void require(bool value, const char* message) noexcept;
// Deliberately normal-returning declaration: the destructor's object must not
// infer the fatal implementation in the other fixture object under /W4 /WX.
void unexpected_hook_teardown() noexcept;
}
