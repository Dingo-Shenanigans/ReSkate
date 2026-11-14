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
// Current host-owned UI capture flags only. This bounded classifier runs under
// the publication gate and must not call originals, plugins or a UI-model lock.
bool trusted_capture() noexcept;
// Atomic host-observed preview epoch only; never a script mutex or UI-model read.
bool preview_report_valid(std::uint64_t epoch, std::uint64_t now_ms) noexcept;
}
