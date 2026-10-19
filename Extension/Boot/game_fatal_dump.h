#pragma once

#include <cstdint>
#include <string>

namespace dingosdk {
// When one of the game's fatal handlers is about to write its minidump, the crash reporter first
// takes its own of the still-running game (crash_report::capture_game_fatal), with the memory the
// stacks point at. Points the game's MiniDumpWriteDump import at ReSkate; the game's dump is
// still written after. Install after crash reporting has started, before the game runs.
bool start_game_fatal_dump(std::uintptr_t base, std::string& error);
} // namespace dingosdk
