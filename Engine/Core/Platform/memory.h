#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dingosdk::memory {
inline constexpr std::uintptr_t highest_user_address = 0x00007fffffffffffULL;
inline bool read_bytes(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (!destination || !size || size > highest_user_address || address < 0x10000 ||
        address > highest_user_address - size) return false;
    SIZE_T count{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size, &count) && count == size;
}
template<class T> requires std::is_trivially_copyable_v<T>
bool read(std::uintptr_t address, T& value) noexcept { return read_bytes(address, &value, sizeof(value)); }

// The same read without a kernel transition: a plain copy that stops at an access violation.
// ReadProcessMemory above costs a system call (about 1 us) per read, which adds up on paths
// that run every game frame. Use peek there, for game objects expected to be readable; a
// failed peek costs an exception, so keep read for probing addresses that are often invalid.
inline bool peek_bytes(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (!destination || !size || size > highest_user_address || address < 0x10000 ||
        address > highest_user_address - size) return false;
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template<class T> requires std::is_trivially_copyable_v<T>
bool peek(std::uintptr_t address, T& value) noexcept { return peek_bytes(address, &value, sizeof(value)); }

// A NUL-terminated string copied the same way: the length (terminator written at destination[length]),
// or -1 if it is unreadable or has no terminator within capacity bytes. Reads byte by byte, so it never
// touches memory past the terminator.
inline std::ptrdiff_t peek_cstring(std::uintptr_t address, char* destination, std::size_t capacity) noexcept {
    if (!destination || !capacity || address < 0x10000 || address > highest_user_address - capacity) return -1;
    __try {
        const auto* source = reinterpret_cast<const char*>(address);
        for (std::size_t i = 0; i < capacity; ++i) {
            destination[i] = source[i];
            if (destination[i] == '\0') return static_cast<std::ptrdiff_t>(i);
        }
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}
