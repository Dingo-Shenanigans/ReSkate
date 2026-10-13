#include "Extension/Rendering/cull_batch_core.h"
#include "Engine/Core/Hooks/hooks.h"
#include <Windows.h>
#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace detail = dingosdk::cull_batch::detail;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using Claim = unsigned (*)(volatile LONG*);
void vector_preservation() {
    std::vector<unsigned char> code{0x41,0x55,0x41,0x56,0x41,0x57,0x49,0x89,0xd6,0x49,0x89,0xcf};
    const auto vectors = [&](unsigned char operation) {
        for (unsigned reg = 0; reg < 16; ++reg) {
            code.insert(code.end(), {0xc4,static_cast<unsigned char>(reg < 8 ? 0xc1 : 0x41),0x7e,
                operation,static_cast<unsigned char>(0x86 + (reg % 8) * 8)});
            const std::uint32_t offset = reg * 32;
            const auto* bytes = reinterpret_cast<const unsigned char*>(&offset);
            code.insert(code.end(), bytes, bytes + 4);
        }
    };
    vectors(0x6f); // Load all sixteen YMM registers from distinct caller-supplied patterns.
    code.insert(code.end(), {0xb8,1,0,0,0});
    const auto site = code.size();
    code.insert(code.end(), {0xf0,0x41,0x0f,0xc1,0x07});
    const auto continuation = code.size();
    code.insert(code.end(), {0x49,0x89,0xd6}); // RDX must also survive the gateway.
    vectors(0x7f);
    code.insert(code.end(), {0x41,0x5f,0x41,0x5e,0x41,0x5d,0xc3});
    auto* native = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(native != nullptr, "vector fixture allocation");
    std::memcpy(native, code.data(), code.size());
    DWORD old{}; require(VirtualProtect(native, 4096, PAGE_EXECUTE_READ, &old) != FALSE, "vector fixture RX");
    FlushInstructionCache(GetCurrentProcess(), native, code.size());
    auto gate = detail::make_gate(native + continuation);
    void* relay{};
    require(dingosdk::hook_prepare(native + site, gate.code, &relay) == dingosdk::HookOk, "vector hook prepare");
    *gate.stock = relay;
    require(dingosdk::hook_enable(native + site) == dingosdk::HookOk, "vector hook enable");
    const auto call = reinterpret_cast<unsigned (*)(volatile LONG*, void*)>(native);
    for (unsigned batch : {1u,8u}) {
        volatile LONG counter = 0;
        detail::Frame frame{&counter,0,0,17,batch}; detail::active_frame = &frame;
        for (unsigned index = 0; index <= 17; ++index) {
            std::array<unsigned char, 512> pattern{}, expected{};
            for (std::size_t byte = 0; byte < pattern.size(); ++byte)
                pattern[byte] = static_cast<unsigned char>((byte * 13 + index * 7) & 255);
            expected = pattern;
            require(call(&counter, pattern.data()) == index, "vector fixture claim result");
            require(pattern == expected, "YMM or pointer register corrupted");
        }
        detail::active_frame = nullptr;
    }
    require(dingosdk::hook_remove(native + site) == dingosdk::HookOk, "vector hook remove");
    detail::release_gate(gate); VirtualFree(native, 0, MEM_RELEASE);
}

void benchmark(Claim claim, const char* label, std::initializer_list<unsigned> sizes) {
    // Synthetic contention only, not a game/FPS benchmark. Thread startup is excluded.
    for (unsigned workers : {1u,8u}) for (unsigned batch : sizes) {
        volatile LONG counter = 0;
        constexpr unsigned count = 500000;
        std::barrier start(static_cast<std::ptrdiff_t>(workers + 1));
        std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < workers; ++worker) threads.emplace_back([&] {
            detail::Frame frame{&counter,0,0,count,batch}; detail::active_frame = &frame;
            start.arrive_and_wait();
            while (claim(&counter) < count) {}
            detail::active_frame = nullptr;
        });
        const auto before = std::chrono::steady_clock::now(); start.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
        std::cout << "Synthetic " << label << ": " << workers << " worker(s), batch " << batch << ": " << elapsed << " ms\n";
    }
}

int main() {
    try {
        require(IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) != FALSE, "AVX required");
        require(dingosdk::hook_initialize() == dingosdk::HookOk, "hook init");
        // A native ABI fixture with the exact R15/EAX XADD used in Skate, at aligned RSP.
        const unsigned char bytes[]{0x41,0x57,0x49,0x89,0xcf,0xb8,1,0,0,0,0xf0,0x41,0x0f,0xc1,0x07,0x41,0x5f,0xc3};
        auto* native = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        require(native != nullptr, "fixture allocation");
        std::memcpy(native, bytes, sizeof(bytes));
        std::memcpy(native + 128, bytes, sizeof(bytes)); // Independent unhooked scheduling baseline.
        DWORD old{}; require(VirtualProtect(native, 4096, PAGE_EXECUTE_READ, &old) != FALSE, "fixture RX");
        FlushInstructionCache(GetCurrentProcess(), native, sizeof(bytes));
        const auto claim = reinterpret_cast<Claim>(native);
        auto gate = detail::make_gate(native + 15);
        void* relay{};
        require(dingosdk::hook_prepare(native + 10, gate.code, &relay) == dingosdk::HookOk, "claim hook prepare");
        *gate.stock = relay;
        require(dingosdk::hook_enable(native + 10) == dingosdk::HookOk, "claim hook enable");

        for (unsigned batch : {1u,2u,4u,8u,16u,32u}) {
            for (unsigned workers : {1u,2u,8u}) {
                for (unsigned count : {0u,1u,7u,8u,9u,31u,257u,10007u}) {
                    volatile LONG counter = 0;
                    std::vector<std::atomic<unsigned>> seen(count);
                    for (auto& value : seen) value.store(0);
                    std::barrier start(static_cast<std::ptrdiff_t>(workers));
                    std::vector<std::thread> threads;
                    for (unsigned worker = 0; worker < workers; ++worker) threads.emplace_back([&] {
                        detail::Frame frame{&counter,0,0,count,batch};
                        detail::active_frame = &frame;
                        start.arrive_and_wait();
                        for (;;) {
                            const auto index = claim(&counter);
                            if (index >= count) break;
                            seen[index].fetch_add(1);
                        }
                        require(frame.next == frame.end, "unconsumed reserved items");
                        detail::active_frame = nullptr;
                    });
                    for (auto& thread : threads) thread.join();
                    for (auto& value : seen) require(value.load() == 1, "missing or duplicate item");
                    require(static_cast<unsigned>(counter) == count + workers, "terminal claims changed");
                }
            }
        }
        volatile LONG counter = 0;
        detail::Frame outer{&counter,0,0,20,8}; detail::active_frame = &outer;
        require(claim(&counter) == 0, "outer first");
        detail::Frame inner{&counter,0,0,20,4}; detail::active_frame = &inner;
        require(claim(&counter) == 8, "nested reservation");
        for (unsigned expected = 9; expected < 12; ++expected) require(claim(&counter) == expected, "inner cache");
        detail::active_frame = &outer;
        for (unsigned expected = 1; expected < 8; ++expected) require(claim(&counter) == expected, "outer cache restored");
        detail::active_frame = nullptr;
        require(claim(&counter) == 12, "stock passthrough without scope");
        vector_preservation();
        benchmark(reinterpret_cast<Claim>(native + 128), "unhooked", {1u});
        benchmark(claim, "instrumented", {1u,8u});
        require(dingosdk::hook_remove(native + 10) == dingosdk::HookOk, "remove hook");
        detail::release_gate(gate); VirtualFree(native, 0, MEM_RELEASE);
        std::cout << "Culling gateway: 144 concurrent boundary cases, nested scopes, all YMM registers and native passthrough passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
