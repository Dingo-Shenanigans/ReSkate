#pragma once
#include <cstdint>

// When a function of the game finds its stack written over, the game ends itself with a fast
// fail (exit code 0xc0000409): no exception, so neither crash reporter writes anything. This
// puts where it happened in the log first. A diagnostic for work on the game's fixed-size
// tables (texture_handle_limit); it changes nothing else.
namespace dingosdk::stack_check_report {
bool install(std::uintptr_t base) noexcept;
}
