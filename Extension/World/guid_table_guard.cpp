#include "guid_table_guard.h"
#include "guid_table_probe.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/guid_table_lookup.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <intrin.h>

namespace dingosdk::guid_table_guard {
namespace {
namespace gt = addr::guid_table_lookup;
using Find = std::uintptr_t (*)(void*, const std::uint32_t*, void*, std::uint32_t);
using Record = std::uintptr_t (*)(void*, std::uint32_t, void*, std::uint32_t);
using KeyOf = const std::uint32_t* (*)(void*, std::uint32_t);
Find find_original{};
Record record{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> misses{};

using detail::Probe;
// Keep native reads under SEH; the same bounded traversal is exercised without
// game pointers by the regression test. Present keys still use find_original.
Probe probe(std::uintptr_t table, const std::uint32_t* key) noexcept {
    __try {
        const auto count = *reinterpret_cast<const std::uint32_t*>(table + gt::count);
        const auto size = detail::slot_count(count);
        const auto* const slots = *reinterpret_cast<const std::uint16_t* const*>(table + gt::slots);
        auto* const source = *reinterpret_cast<void* const*>(table + gt::source);
        if (!size || !slots || !source) return Probe::unreadable;
        const auto key_of = reinterpret_cast<KeyOf>((*reinterpret_cast<std::uintptr_t* const*>(source))[gt::key_of / sizeof(std::uintptr_t)]);
        const std::array<std::uint32_t,4> wanted{key[0],key[1],key[2],key[3]};
        return detail::probe_slots(std::span<const std::uint16_t>{slots,size},wanted,
            [source,key_of](std::uint16_t index){return key_of(source,index);});
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Probe::unreadable;
    }
}

std::uintptr_t finding(void* table, const std::uint32_t* key, void* a, std::uint32_t b) {
    if (!key || probe(reinterpret_cast<std::uintptr_t>(table), key) != Probe::missing) return find_original(table, key, a, b);
    const auto n = misses.fetch_add(1) + 1;
    if (n <= 8 || n % 100 == 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(key);
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "GUID table guard: key {:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x} is not in its table "
            "(the game would freeze looking for it); it is passed over, with nothing made for it ({} so far).",
            key[0], key[1] & 0xffff, key[1] >> 16, bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15], n);
    }
    // A handle the game can keep, for no items: record 0 with its items copied in place of the
    // missing one's left a record shaped for another and crashed later (2026-10-17, 2.5 min on).
    (void)b;
    return record(table, 0, a, 0);
}
bool matches(std::uintptr_t base, const game::build::Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> head{};
    return memory::peek(base + contract.rva, head) && head == contract.bytes;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return find_original != nullptr;
    if (!base) return false;
    if (!matches(base, gt::find) || !matches(base, gt::record_contract)) {
        logging::log(logging::Level::warning, logging::Channel::runtime, "GUID table guard: this build's lookup differs; it is left as it is.");
        return false;
    }
    record = reinterpret_cast<Record>(base + gt::record);
    auto* const target = reinterpret_cast<void*>(base + gt::find.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&finding), reinterpret_cast<void**>(&find_original)) != HookOk) return false;
    if (hook_enable(target) != HookOk) {
        hook_remove(target);
        find_original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "GUID table guard: the game's lookup could not be hooked.");
        return false;
    }
    return true;
}
} // namespace dingosdk::guid_table_guard
