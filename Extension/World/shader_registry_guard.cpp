#include "shader_registry_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/shader_registry.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace dingosdk::shader_registry_guard {
namespace {
namespace sr = addr::shader_registry;
using Construct = void* (*)(void*, void*, void*, void*, std::uint32_t);
using Destroy = void* (*)(void*, std::uint32_t);
using Register = void (*)(void*, void*);
Construct construct_original{};
Destroy destroy_original{};
Register register_original{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> repointed{}, skipped{};

// Databases destroyed and not made again at the same address since. A registration of one of
// these is of freed memory. (A database the guard never saw is not in it, so it is registered.)
struct Destroyed {
    std::mutex mutex;
    std::unordered_set<std::uintptr_t> at;
};
Destroyed& destroyed() {
    static auto* value = new Destroyed; // process lifetime: the game's threads may still call in
    return *value;
}

// Said for the first few, then every hundredth.
bool worth_saying(std::uint64_t count) noexcept { return count <= 8 || count % 100 == 0; }

// The lookup's record indices past the database's records, pointed at a record that is there:
// another of the same key's, or the first. The lookup is the database's own buffer, read once by
// the registration that follows. False when the lookup could not be read as one (left as it is).
bool repoint_outside(std::uintptr_t database, std::uint32_t& outside, std::uint32_t& records) noexcept {
    __try {
        const auto data = *reinterpret_cast<std::uint8_t* const*>(database + sr::lookup_data);
        const auto size = static_cast<std::size_t>(*reinterpret_cast<const std::uint32_t*>(database + sr::lookup_size));
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(database + sr::items_begin);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(database + sr::items_end);
        if (!data || size < 4 || end < begin || (end - begin) % sr::item_size) return false;
        records = static_cast<std::uint32_t>((end - begin) / sr::item_size);
        std::uint32_t keys{};
        std::memcpy(&keys, data, 4);
        std::size_t at = 4;
        for (std::uint32_t key = 0; key < keys; ++key) {
            if (size - at < 12) return false;
            std::uint32_t count{};
            std::memcpy(&count, data + at + 8, 4);
            at += 12;
            if (count > (size - at) / 4) return false;
            auto* const indices = data + at;
            std::uint32_t fallback = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t index{};
                std::memcpy(&index, indices + i * 4, 4);
                if (index < records) { fallback = index; break; }
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t index{};
                std::memcpy(&index, indices + i * 4, 4);
                if (index < records) continue;
                std::memcpy(indices + i * 4, &fallback, 4);
                ++outside;
            }
            at += std::size_t{count} * 4;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* constructed(void* self, void* allocator, void* blob, void* lookup, std::uint32_t size) {
    {
        auto& d = destroyed();
        std::lock_guard lock(d.mutex);
        d.at.erase(reinterpret_cast<std::uintptr_t>(self));
    }
    return construct_original(self, allocator, blob, lookup, size);
}
void* destroying(void* self, std::uint32_t flags) {
    {
        auto& d = destroyed();
        std::lock_guard lock(d.mutex);
        if (d.at.size() < 65536) d.at.insert(reinterpret_cast<std::uintptr_t>(self));
    }
    return destroy_original(self, flags);
}
void registering(void* registrar, void* database) {
    const auto at = reinterpret_cast<std::uintptr_t>(database);
    bool gone{};
    {
        auto& d = destroyed();
        std::lock_guard lock(d.mutex);
        gone = d.at.contains(at);
    }
    if (gone) {
        const auto count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (worth_saying(count))
            try {
                logging::log(logging::Level::warning, logging::Channel::runtime,
                    "Shaders: a shader database ({:#x}) was destroyed before its queued registration ran; the registration "
                    "is not done, where the game would have registered freed memory and crashed later ({} time(s)).", at, count);
            } catch (...) {}
        return;
    }
    std::uint32_t outside{}, records{};
    if (database && repoint_outside(at, outside, records) && outside) {
        const auto count = repointed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (worth_saying(count))
            try {
                logging::log(logging::Level::warning, logging::Channel::runtime,
                    "Shaders: a shader database ({:#x}, {} records) has {} lookup index(es) past its records, where the game "
                    "would have kept a pointer outside it and crashed later; they now point at a record it has. A map whose "
                    "materials name shader programs it does not ship does this ({} database(s)).", at, records, outside, count);
            } catch (...) {}
    }
    register_original(registrar, database);
}

template <class Function>
bool hook(std::uintptr_t base, const game::build::Fingerprint& contract, Function replacement, Function& original) noexcept {
    auto* const target = reinterpret_cast<void*>(base + contract.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original)) != HookOk) return false;
    if (hook_enable(target) != HookOk) {
        hook_remove(target);
        original = nullptr;
        return false;
    }
    return true;
}
bool matches(std::uintptr_t base, const game::build::Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> head{};
    return memory::peek(base + contract.rva, head) && head == contract.bytes;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return register_original != nullptr;
    if (!base) return false;
    if (!matches(base, sr::construct) || !matches(base, sr::destroy) || !matches(base, sr::register_database)) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Shader registry guard: this build's shader code differs; it is left as it is.");
        return false;
    }
    // The destroyed set is kept by the constructor and destructor; registration only reads it, so
    // it goes in last, once both are in.
    const bool made = hook(base, sr::construct, &constructed, construct_original) &&
                      hook(base, sr::destroy, &destroying, destroy_original) &&
                      hook(base, sr::register_database, &registering, register_original);
    if (!made)
        logging::log(logging::Level::warning, logging::Channel::runtime, "Shader registry guard: the game's functions could not all be hooked.");
    return made;
}
} // namespace dingosdk::shader_registry_guard
