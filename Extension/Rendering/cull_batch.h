#pragma once
#include <cstdint>
#include <string>
namespace dingosdk::cull_batch {
bool install(std::uintptr_t base) noexcept;
bool available() noexcept;
unsigned size() noexcept; // 1 = native one-item scheduling (the hooks remain installed)
bool set_size(unsigned items) noexcept;
std::string status();
}
