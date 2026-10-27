#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::runtime {
// Call after image validation, before executable entry, outside the loader lock.
// Failure leaves initialization incomplete; the launcher must abort startup.
// On failure, failure holds the logged line that says why.
bool initialize_bootstrap(std::uintptr_t base, std::string& failure);
}
