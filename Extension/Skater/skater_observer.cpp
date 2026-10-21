#include "skater_observer.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/skater_observer.h"
#include <atomic>

namespace dingosdk {
namespace {
namespace obs = addr::skater_observer;
using Factory = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
using Create = void (*)(std::uintptr_t, std::uint8_t);
using Bind = void (*)(std::uintptr_t, std::uintptr_t);
enum Counter : std::size_t { FactoryEntered, CreateEntered, BindEntered, CounterCount };
struct Observer {
    std::uintptr_t base{};
    Factory factory{}; Create create{}; Bind bind{};
    std::array<std::atomic<std::uint64_t>, CounterCount> counts{};
    std::atomic<bool> active{};
    bool attempted{};
};
Observer& observer() { static auto* value = new Observer; return *value; }
struct LifecycleLogGate {
    std::atomic<ULONGLONG> next{};
    std::atomic<std::uint64_t> skipped{};
    bool admit(logging::Channel channel, const char* operation) noexcept {
        const auto now = GetTickCount64();
        auto due = next.load();
        if (now < due || !next.compare_exchange_strong(due, now + 100)) { ++skipped; return false; }
        if (const auto count = skipped.exchange(0))
            logging::log(logging::Level::info, channel, "{}: {} additional native calls suppressed during rapid activity.", operation, count);
        return true;
    }
};
std::uintptr_t factory_hook(std::uintptr_t factory, std::uintptr_t context) {
    const auto incoming_error = GetLastError();
    auto& o = observer();
    const auto id = ++o.counts[FactoryEntered];
    static LifecycleLogGate gate;
    const bool report = gate.admit(logging::Channel::skater, "Skater factory");
    if (report) logging::log(logging::Level::debug, logging::Channel::skater,
        "Skater component factory {} entered (context=0x{:x}).", id, context);
    const auto started = GetTickCount64();
    SetLastError(incoming_error);
    const auto result = o.factory(factory, context);
    const auto native_error = GetLastError();
    if (report || !result) logging::log(result ? logging::Level::info : logging::Level::warning, logging::Channel::skater,
        "Skater component factory {} returned {} after {}ms (component=0x{:x}).",
        id, result ? "a component" : "null", GetTickCount64() - started, result);
    SetLastError(native_error);
    return result;
}
void create_hook(std::uintptr_t component, std::uint8_t ground_check) {
    const auto incoming_error = GetLastError();
    auto& o = observer();
    const auto id = ++o.counts[CreateEntered];
    static LifecycleLogGate gate;
    const bool report = gate.admit(logging::Channel::skater, "Skater creation");
    if (report) logging::log(logging::Level::info, logging::Channel::skater,
        "Skater creation {} entered (component=0x{:x}, ground_check={}).", id, component, ground_check);
    const auto started = GetTickCount64();
    SetLastError(incoming_error);
    o.create(component, ground_check);
    const auto native_error = GetLastError();
    const auto elapsed = GetTickCount64() - started;
    if (report || elapsed >= 1000) logging::log(elapsed >= 1000 ? logging::Level::warning : logging::Level::info, logging::Channel::skater,
        "Skater creation {} returned after {}ms; awaiting player binding and a live skater.", id, elapsed);
    if (report && logging::enabled(logging::Level::debug)) {
        const auto sample = read_skater_component(GetCurrentProcess(), o.base, component);
        if (sample.available)
            logging::log(logging::Level::debug, logging::Channel::skater,
                "Skater creation {} component fields: dependencies={}/{}/{}/{}/{}/{}/{}, flags(C3..CB)={:02x}/{:02x}/{:02x}/{:02x}/{:02x}/{:02x}/{:02x}/{:02x}/{:02x}.",
                id, sample.present[0], sample.present[1], sample.present[2], sample.present[3],
                sample.present[4], sample.present[5], sample.present[6], sample.flags[0], sample.flags[1],
                sample.flags[2], sample.flags[3], sample.flags[4], sample.flags[5], sample.flags[6], sample.flags[7], sample.flags[8]);
    }
    SetLastError(native_error);
}
void bind_hook(std::uintptr_t player, std::uintptr_t entity) {
    const auto incoming_error = GetLastError();
    auto& o = observer();
    const auto id = ++o.counts[BindEntered];
    static LifecycleLogGate gate;
    const bool report = gate.admit(logging::Channel::player, "Player binding");
    if (report) logging::log(logging::Level::info, logging::Channel::player,
        "Player binding {} entered (player=0x{:x}, entity=0x{:x}, action={}).",
        id, player, entity, entity ? "assign" : "release");
    const auto started = GetTickCount64();
    SetLastError(incoming_error);
    o.bind(player, entity);
    const auto native_error = GetLastError();
    const auto elapsed = GetTickCount64() - started;
    if (report || elapsed >= 1000) logging::log(elapsed >= 1000 ? logging::Level::warning : logging::Level::info, logging::Channel::player,
        "Player binding {} returned after {}ms (player=0x{:x}, entity=0x{:x}).", id, elapsed, player, entity);
    SetLastError(native_error);
}
struct Hook { std::uintptr_t rva; void* detour; void* original{}; std::array<unsigned char, 24> fingerprint; };
bool fingerprint(std::uintptr_t address, const std::array<unsigned char, 24>& expected) noexcept {
    std::array<unsigned char, 24> actual{};
    SIZE_T count{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), actual.data(),
                             actual.size(), &count) && count == actual.size() && actual == expected;
}
}
std::uint64_t skater_creations() noexcept { return observer().counts[CreateEntered].load(); }
bool start_skater_observer(std::uintptr_t base) noexcept {
    try {
        auto& o = observer();
        if (o.attempted) return o.active.load();
        o.attempted = true;
        o.base = base;
        Hook hooks[]{
            {obs::skater_factory, reinterpret_cast<void*>(&factory_hook), nullptr, obs::skater_factory_prefix},
            {obs::component_create, reinterpret_cast<void*>(&create_hook), nullptr, obs::component_create_prefix},
            {obs::player_bind, reinterpret_cast<void*>(&bind_hook), nullptr, obs::player_bind_prefix}
        };
        constexpr std::size_t hook_count = std::size(hooks);
        for (std::size_t i = 0; i < hook_count; ++i)
            if (!fingerprint(base + hooks[i].rva, hooks[i].fingerprint)) return false;
        std::size_t created{};
        for (std::size_t i = 0; i < hook_count; ++i) {
            auto& hook = hooks[i];
            if (hook_prepare(reinterpret_cast<void*>(base + hook.rva), hook.detour, &hook.original) != HookOk) break;
            ++created;
        }
        if (created != hook_count) {
            for (std::size_t i = 0; i < created; ++i) hook_remove(reinterpret_cast<void*>(base + hooks[i].rva));
            return false;
        }
        o.factory = reinterpret_cast<Factory>(hooks[0].original);
        o.create = reinterpret_cast<Create>(hooks[1].original);
        o.bind = reinterpret_cast<Bind>(hooks[2].original);
        std::size_t enabled{};
        for (std::size_t i = 0; i < hook_count; ++i) {
            const auto& hook = hooks[i];
            if (hook_enable(reinterpret_cast<void*>(base + hook.rva)) != HookOk) break;
            ++enabled;
        }
        if (enabled != hook_count) {
            // Keep trampolines allocated: an already entered detour may still be
            // forwarding on another thread when a later enable operation fails.
            for (std::size_t i = 0; i < enabled; ++i) hook_disable(reinterpret_cast<void*>(base + hooks[i].rva));
            return false;
        }
        o.active.store(true);
        logging::write(logging::Level::info, logging::Channel::player,
            "Native player lifecycle logging enabled: skater component factory, creation and player binding.");
        return true;
    } catch (...) { return false; }
}
}
