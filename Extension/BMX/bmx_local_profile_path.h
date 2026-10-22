#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::local_profile_path {
inline constexpr std::uintptr_t highest_address = 0x00007fffffffffffULL;
inline constexpr std::uintptr_t pc_storage_vtable_rva = 0x68254b8;
inline constexpr std::size_t maximum_path_bytes = 260;
inline bool span(std::uintptr_t address, std::size_t size) noexcept {
    return address >= 0x10000 && size && size <= highest_address && address <= highest_address - size;
}
struct StoragePath {
    const char* status{"unavailable"};
    bool coherent{};
    std::uintptr_t path_pointer{};
    std::array<char, maximum_path_bytes + 1> text{};
    std::size_t length{}; // Byte length, excluding NUL; never a UTF-16 character count.
};
// Native PC storage consumes configuration+90 as char*, not wchar_t*.
// Snapshot its nonempty, NUL-terminated 1..260-byte payload without conversion,
// native calls, ownership changes or filesystem access. The caller must validate
// UTF-8, absolute-path semantics and containment before using an accepted path.
// Agreement across bounded rereads does not retain the native buffer or lock it.
template<class Read> StoragePath inspect_storage_path(std::uintptr_t base, std::uintptr_t platform,
    std::uintptr_t configuration, Read&& read) noexcept {
    StoragePath out;
    try {
        const auto raw = [&read](std::uintptr_t address, void* bytes, std::size_t size) {
            return span(address, size) && read(address, bytes, size);
        };
        const auto get = [&raw]<class T>(std::uintptr_t address, T& value) {
            return raw(address, &value, sizeof(value));
        };
        if (base % 8 || !span(base, pc_storage_vtable_rva + 8) || platform % 8 || configuration % 8 ||
            !span(platform, 8) || !span(configuration, 0x98)) return out;
        std::uintptr_t table{};
        if (!get(platform, table) || table != base + pc_storage_vtable_rva ||
            !get(configuration + 0x90, out.path_pointer)) { out.status = "unsupported_configuration"; return out; }
        if (!span(out.path_pointer, 1)) { out.status = "invalid_path_pointer"; return out; }
        for (; out.length < out.text.size(); ++out.length) {
            if (!get(out.path_pointer + out.length, out.text[out.length])) {
                out.status = "unreadable_path"; return out;
            }
            if (!out.text[out.length]) break;
        }
        if (!out.length || out.length == out.text.size()) { out.status = "invalid_path_length"; return out; }
        const auto same_identity = [&]() {
            std::uintptr_t current_table{}, current_path{};
            return get(platform, current_table) && current_table == table &&
                get(configuration + 0x90, current_path) && current_path == out.path_pointer;
        };
        const auto same_content = [&]() {
            std::array<char, maximum_path_bytes + 1> again{};
            return raw(out.path_pointer, again.data(), out.length + 1) && again == out.text;
        };
        // Reroot after each complete content read so pointer/type replacement
        // during a read cannot make a stale native buffer look current.
        if (!same_content() || !same_identity() || !same_content() || !same_identity()) {
            out.status = "changed"; return out;
        }
        out.coherent = true; out.status = "complete";
    } catch (...) { out.status = "reader_exception"; }
    return out;
}
}
