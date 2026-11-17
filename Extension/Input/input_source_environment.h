#pragma once
#include <cstdint>
namespace dingosdk::native_input_source::environment {
struct Facts {
    std::uint64_t now_ms{};
    std::uint32_t thread{};
    bool focused{}, gameplay{}, menu_closed{}, interference_clear{};
};
// Platform leaves run before acquiring the publication gate. Their revision
// witness is checked after acquisition; no external leaf runs while held.
Facts sample() noexcept;
#if defined(DINGOSDK_INPUT_SOURCE_TEST)
// The source fixtures keep the cheap tick clock aligned with their scripted samples.
std::uint64_t test_tick_now() noexcept;
std::uint32_t test_tick_thread() noexcept;
#endif
// Current host-owned UI capture flags only. This bounded classifier runs under
// the publication gate and must not call originals, plugins or a UI-model lock.
bool trusted_capture() noexcept;
}
