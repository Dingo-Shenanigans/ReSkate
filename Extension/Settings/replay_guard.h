#pragma once
#include <cstdint>

// The game's replay recording crashes when its stream has no room for the next chunk: it writes
// to the chunk it was not given (Engine/Game/Build/<build>/replay_recorder.h). The guard runs
// that one function under an exception handler and, for that one fault alone, has it return as
// if no chunk was ended: the game goes on, with a gap or a glitch in the replay where it would
// have closed. Every other fault is left to the crash reporter.
namespace dingosdk::replay_guard {
// Checks this build's function and hooks it; once. False when the build differs (no guard).
bool install(std::uintptr_t base) noexcept;
// How many chunks were skipped since the game started.
std::uint64_t skipped() noexcept;
}
