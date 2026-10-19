#pragma once
#include <cstdint>
namespace dingosdk::skate {
// Installs the supported ignored-observation and allowed-category adapters.
bool prepare_native_filter(std::uintptr_t vm) noexcept;
void native_filter_completed(std::uintptr_t vm,std::uint32_t pc) noexcept;
}
