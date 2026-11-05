#pragma once
#include "logging.h"
namespace dingosdk::logging::detail {
struct PreserveError {
    DWORD saved{GetLastError()};
    ~PreserveError() { SetLastError(saved); }
};
std::string clean(std::string_view text);
void install_crash_report(const std::filesystem::path& directory) noexcept;
void remove_crash_report() noexcept;
}
