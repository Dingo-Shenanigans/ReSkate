#pragma once
#include <Windows.h>
#include <filesystem>

namespace dingosdk::crash_report {
enum class StartResult { disabled, ready, failed };
// Called outside DllMain. A missing endpoint or RESKATE_CRASH_REPORTING=0 disables reporting.
StartResult start(const std::filesystem::path& log_directory) noexcept;
void stop() noexcept;
// Last-chance path: only signals the prestarted helper and waits at most 15 seconds.
void capture(EXCEPTION_POINTERS* exception) noexcept;
}
