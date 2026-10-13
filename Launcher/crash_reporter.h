#pragma once
namespace dingosdk::crash_report {
// Internal launcher mode, entered before game launch or logging initialization.
int run_reporter(int argc, wchar_t** argv) noexcept;
}
