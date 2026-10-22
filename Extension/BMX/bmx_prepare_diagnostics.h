#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::bmx {
// Isolated development data + explicit Flow diagnostics opt-in only. These
// hooks observe existing native work and always preserve its result.
bool initialize_prepare_diagnostics(std::uintptr_t base) noexcept;
std::string prepare_diagnostics();
}
