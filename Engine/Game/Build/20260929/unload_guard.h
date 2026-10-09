#pragma once
#include <array>
#include <cstdint>

// Level unload crash guard. A job run while a level unloads walks each unloading object's table
// of 0x30-byte entries and reads a number at +0xc8 of the asset each entry points to
// (`mov rax,[rbx+8]; and rax,-5; mov edx,[rax+0xc8]`). An entry whose asset never resolved
// holds null there and the game crashes (seen leaving a mod map on a busy server). The loop is
// rewritten in place to skip such an entry: the 2-byte nop before it makes room for a `jz`
// straight after the `and`, the two moves the jump displaced follow it, and the loop's jump
// back is retargeted to the new start. Everything from the read on is where it was.
namespace dingosdk::game::build::v20260929::unload_guard {
inline constexpr std::uintptr_t rva = 0x3f5fc3e;
inline constexpr std::array<unsigned char, 50> contract{
    0x66,0x90,                               // nop
    0x48,0x8b,0x43,0x08,                     // loop: mov rax,[rbx+8]
    0x4c,0x8b,0xcd,                          // mov r9,rbp
    0x44,0x8b,0x43,0x24,                     // mov r8d,[rbx+0x24]
    0x48,0x83,0xe0,0xfb,                     // and rax,-5
    0x48,0x8b,0xcf,                          // mov rcx,rdi
    0x8b,0x90,0xc8,0x00,0x00,0x00,           // mov edx,[rax+0xc8]
    0x8b,0x43,0x20,0x48,0xc1,0xe2,0x20,0x48,0x0b,0xd0,0xe8,0x99,0xa5,0xff,0xff,
    0x48,0x83,0xc3,0x30,                     // next: add rbx,0x30
    0x48,0x3b,0xde,0x75,0xd0};               // cmp rbx,rsi; jne loop
inline constexpr std::array<unsigned char, 50> patch{
    0x48,0x8b,0x43,0x08,                     // loop: mov rax,[rbx+8]
    0x48,0x83,0xe0,0xfb,                     // and rax,-5
    0x74,0x1f,                               // jz next
    0x4c,0x8b,0xcd,                          // mov r9,rbp
    0x44,0x8b,0x43,0x24,                     // mov r8d,[rbx+0x24]
    0x48,0x8b,0xcf,                          // mov rcx,rdi
    0x8b,0x90,0xc8,0x00,0x00,0x00,           // mov edx,[rax+0xc8]
    0x8b,0x43,0x20,0x48,0xc1,0xe2,0x20,0x48,0x0b,0xd0,0xe8,0x99,0xa5,0xff,0xff,
    0x48,0x83,0xc3,0x30,                     // next: add rbx,0x30
    0x48,0x3b,0xde,0x75,0xce};               // cmp rbx,rsi; jne loop (two bytes earlier)
// The jz sits at +8 and ends at +10; the entry's step is at +41.
static_assert(patch[9] == 41 - 10);
// The jne ends at +50 and the loop now starts at +0.
static_assert(static_cast<signed char>(patch[49]) == 0 - 50 && static_cast<signed char>(contract[49]) == 2 - 50);
}
