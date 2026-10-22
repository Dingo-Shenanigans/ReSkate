#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::bmx {
bool initialize_local_profile(std::uintptr_t base) noexcept;
std::string local_profile_status();
}
