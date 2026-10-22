#pragma once
#include <cstdint>

namespace dingosdk::bmx::binding_observer {
struct Status {
    bool requested{}, initialized{}, capturing{}, written{};
    std::uint64_t samples{}, dropped{};
    const char* issue{"not_requested"};
};
// Install at early runtime initialization, before authored graph
// binding. This observes lookups; it never calls a returned native function.
bool initialize(std::uintptr_t base) noexcept;
// Regular client thread only. All JSON allocation and file IO is here.
Status poll(bool force_write=false) noexcept;
}
