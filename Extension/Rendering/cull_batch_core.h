#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace dingosdk::cull_batch::detail {
struct Frame {
    volatile LONG* counter{};
    std::uint32_t next{}, end{}, bound{}, batch{1};
    std::uint64_t reservations{}, items{};
};
static_assert(offsetof(Frame, next) == 8 && offsetof(Frame, end) == 12 && offsetof(Frame, batch) == 20);
extern thread_local Frame* active_frame;
std::uint32_t claim(volatile LONG* counter) noexcept;
struct Gate { void* code{}; void** stock{}; };
// The two native sites have identical R15/EAX operands and aligned RSP.
// Detours performs the actual instruction relocation and patching.
Gate make_gate(void* continuation);
void release_gate(Gate gate) noexcept;
}
