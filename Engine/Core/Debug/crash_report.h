#pragma once
#include <Windows.h>
#include <filesystem>
#include <string_view>

namespace dingosdk::crash_report {
enum class StartResult { disabled, ready, failed };
// Called outside DllMain. A missing endpoint or RESKATE_CRASH_REPORTING=0 disables reporting.
StartResult start(const std::filesystem::path& log_directory) noexcept;
void stop() noexcept;
// Last-chance path: only signals the prestarted helper and waits at most 15 seconds.
void capture(EXCEPTION_POINTERS* exception) noexcept;
// The same, from the game's own fatal handler just before it writes its dump: thread_id is the
// thread that faulted, which need not be the calling one. The first capture of either kind wins.
void capture_game_fatal(DWORD thread_id, EXCEPTION_POINTERS* exception) noexcept;
// A dump of the running game for a level load stuck in `stage` for `seconds`, sent as a stuck-load
// report (not a crash) with the log. The game goes on; a later crash is still captured. Waits for
// the helper (all the game's threads are held still while it writes the dump), so never call it
// from a game thread. False while reporting is off, another capture is in hand, or the helper did
// not answer.
bool capture_stuck_load(std::wstring_view stage, DWORD seconds) noexcept;
// The level being loaded ("root + sublevel @ start point") and the graphics card the game renders
// on, for a report's tags: game.map, game.destination, gpu.name and gpu.driver. Nothing while
// reporting is off; too long a value is cut short.
void set_destination(std::wstring_view destination) noexcept;
void set_gpu(std::wstring_view name, std::wstring_view driver) noexcept;
// Whether the GPU's breadcrumbs (DRED) are recorded this launch: the report's gpu.breadcrumbs tag.
void set_gpu_breadcrumbs(bool on) noexcept;
}
