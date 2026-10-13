#include "cull_batch_core.h"
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

extern "C" unsigned long _tls_index;

namespace dingosdk::cull_batch::detail {
thread_local Frame* active_frame{};

std::uint32_t claim(volatile LONG* counter) noexcept {
    auto* frame = active_frame;
    if (!frame || frame->counter != counter || frame->batch <= 1)
        return static_cast<std::uint32_t>(InterlockedExchangeAdd(counter, 1));
    if (frame->next < frame->end) return frame->next++;
    for (;;) {
        const LONG current = *counter;
        if (current < 0 || frame->bound > static_cast<unsigned>(std::numeric_limits<LONG>::max()) ||
            static_cast<unsigned>(current) >= frame->bound) {
            // One terminal claim per worker, exactly as stock. Never reserve work past the bound.
            return static_cast<std::uint32_t>(InterlockedExchangeAdd(counter, 1));
        }
        const auto take = std::min(frame->batch, frame->bound - static_cast<unsigned>(current));
        const LONG following = current + static_cast<LONG>(take);
        if (InterlockedCompareExchange(counter, following, current) != current) continue;
        frame->next = static_cast<unsigned>(current) + 1;
        frame->end = static_cast<unsigned>(following);
        ++frame->reservations;
        frame->items += take;
        return static_cast<unsigned>(current);
    }
}

namespace {
struct Code {
    std::vector<unsigned char> bytes;
    void add(std::initializer_list<unsigned char> values) { bytes.insert(bytes.end(), values); }
    template<class T> void value(T v) {
        const auto* at = reinterpret_cast<const unsigned char*>(&v);
        bytes.insert(bytes.end(), at, at + sizeof(v));
    }
    std::size_t branch(unsigned char condition) {
        add({0x0f,condition}); const auto at = bytes.size(); value(std::int32_t{}); return at;
    }
    void target(std::size_t at, std::size_t label) {
        const auto delta = static_cast<std::int32_t>(label - (at + 4));
        std::memcpy(bytes.data() + at, &delta, 4);
    }
    void jump(void* to) { add({0xff,0x25,0,0,0,0}); value(reinterpret_cast<std::uintptr_t>(to)); }
    void vectors(bool save) {
        for (unsigned reg = 0; reg < 16; ++reg) {
            // VMOVDQU [rsp+disp32],YMMn / YMMn,[rsp+disp32].
            add({0xc5,static_cast<unsigned char>(reg < 8 ? 0xfe : 0x7e),
                static_cast<unsigned char>(save ? 0x7f : 0x6f),
                static_cast<unsigned char>(0x84 + (reg % 8) * 8),0x24});
            value(std::uint32_t{0x20 + reg * 32});
        }
    }
};
}

Gate make_gate(void* continuation) {
    const auto tls = reinterpret_cast<std::uintptr_t*>(__readgsqword(0x58))[_tls_index];
    const auto offset = reinterpret_cast<std::uintptr_t>(&active_frame) - tls;
    if (offset > static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max())) throw std::runtime_error("Cull batching TLS offset is out of range");
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto page = static_cast<std::size_t>(system.dwPageSize);
    auto* allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation) throw std::bad_alloc();
    try {
        Code c;
        c.add({0x50,0x52,0x51}); // Save RAX, RDX, RCX; the fast path touches no vector registers.
        c.add({0x48,0xb8}); c.value(reinterpret_cast<std::uintptr_t>(&_tls_index));
        c.add({0x8b,0x00,0x65,0x48,0x8b,0x14,0x25,0x58,0,0,0,0x48,0x8b,0x04,0xc2,0x48,0x85,0xc0});
        std::vector<std::size_t> stock;
        stock.push_back(c.branch(0x84));
        c.add({0x48,0x05}); c.value(static_cast<std::int32_t>(offset));
        c.add({0x48,0x8b,0x00,0x48,0x85,0xc0}); stock.push_back(c.branch(0x84));
        c.add({0x4c,0x39,0x38}); stock.push_back(c.branch(0x85)); // Frame's counter must be R15.
        c.add({0x83,0x78,0x14,0x01}); stock.push_back(c.branch(0x86));
        c.add({0x8b,0x48,0x08,0x3b,0x48,0x0c}); const auto slow = c.branch(0x83);
        c.add({0xff,0x40,0x08,0x48,0x89,0x4c,0x24,0x10,0x59,0x5a,0x58});
        c.jump(continuation);
        const auto stock_label = c.bytes.size();
        c.add({0x59,0x5a,0x58,0xff,0x25});
        const auto stock_displacement = c.bytes.size(); c.value(std::int32_t{});

        const auto slow_label = c.bytes.size();
        c.add({0x59,0x5a,0x58});
        c.add({0x9c,0x50,0x51,0x52,0x53,0x55,0x56,0x57});
        for (unsigned reg = 0; reg < 8; ++reg) c.add({0x41,static_cast<unsigned char>(0x50 + reg)});
        // Native RSP is 16-byte aligned at these sites. 128 saved bytes + 544 keeps call alignment.
        c.add({0x48,0x81,0xec,0x20,0x02,0,0});
        c.vectors(true);
        c.add({0xc5,0xf8,0x77,0x4c,0x89,0xf9,0x48,0xb8});
        c.value(reinterpret_cast<std::uintptr_t>(&claim));
        c.add({0xff,0xd0,0x48,0x89,0x84,0x24,0x90,0x02,0,0}); // Saved RAX becomes the claimed index.
        c.vectors(false);
        c.add({0x48,0x81,0xc4,0x20,0x02,0,0});
        for (int reg = 7; reg >= 0; --reg) c.add({0x41,static_cast<unsigned char>(0x58 + reg)});
        c.add({0x5f,0x5e,0x5d,0x5b,0x5a,0x59,0x58,0x9d});
        c.jump(continuation);
        for (const auto at : stock) c.target(at, stock_label);
        c.target(slow, slow_label);
        const auto displacement = static_cast<std::int32_t>(page - (stock_displacement + 4));
        std::memcpy(c.bytes.data() + stock_displacement, &displacement, 4);
        if (c.bytes.size() > page) throw std::runtime_error("Cull batching gateway is too large");
        std::memcpy(allocation, c.bytes.data(), c.bytes.size());
        DWORD old{};
        if (!VirtualProtect(allocation, page, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(GetCurrentProcess(), allocation, c.bytes.size()))
            throw std::runtime_error("Cull batching gateway could not become executable");
        return {allocation, reinterpret_cast<void**>(allocation + page)};
    } catch (...) {
        VirtualFree(allocation, 0, MEM_RELEASE); throw;
    }
}
void release_gate(Gate gate) noexcept { if (gate.code) VirtualFree(gate.code, 0, MEM_RELEASE); }
}
