#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>

namespace dingosdk {
struct SkaterComponentSample {
    bool available{};
    // Presence only, in order: config58, optional60, subsystem70, dependencyA0,
    // dependencyA8, helperB0, dependencyB8. These are not readiness predicates.
    std::array<bool, 7> present{};
    std::array<bool, 3> globals_known{}, globals_present{};
    std::array<std::uint8_t, 9> flags{}; // C3 through CB, inclusive
    bool optional_target_known{}, optional_target_present{}, wait_finite{};
    float wait_seconds{};
};
// Read only the verified component layout, while its native call owns its lifetime.
SkaterComponentSample read_skater_component(HANDLE process, std::uintptr_t image_base,
                                           std::uintptr_t component) noexcept;
// Requires the validated build and an initialized Detours hook service. No native calls are
// made by the observer except forwarding each intercepted call exactly once.
bool start_skater_observer(std::uintptr_t image_base) noexcept;
// How many times the game has built a skater since start (0 while the observer is not running).
std::uint64_t skater_creations() noexcept;
}
