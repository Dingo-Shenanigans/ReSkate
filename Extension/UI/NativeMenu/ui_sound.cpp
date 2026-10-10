#include "ui_sound.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "Engine/Game/Build/20260929/native_throwdowns.h"
#include "Extension/Throwdowns/native_type_scan.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <optional>
#include <thread>

namespace dingosdk::multiplayer {
namespace {
namespace throwdowns = addr::native_throwdowns;
namespace party = addr::native_party;
// EBX TypeNameHash of the event, and the NameHash of its UiAudioEnum field.
constexpr std::uint32_t play_one_shot = 1415301872U, sound_field = 1903611779U, none = ~0U;

std::atomic<std::uint32_t> wanted{none};
// The event's type object. Its type is defined by data a level's bundles carry: it is made
// again, somewhere else, by each level, so it is found on the heap (off this thread) and
// checked before every use.
std::atomic<std::uintptr_t> type_object{};
std::atomic<bool> scanning{}, off{};
std::atomic<std::uint64_t> scan_after{};

template<std::size_t Size> bool matches(std::uintptr_t address, const std::array<unsigned char, Size> &prefix) {
    std::array<unsigned char, Size> bytes{};
    return memory::read_bytes(address, bytes.data(), bytes.size()) && bytes == prefix;
}
// The five game functions used here are the ones this build's party and throwdown code calls.
bool supported(std::uintptr_t base) {
    static const bool ok = [base] {
        if (!matches(base + throwdowns::type_construct, throwdowns::type_construct_prefix) ||
            !matches(base + throwdowns::type_destroy, throwdowns::type_destroy_prefix))
            return false;
        for (const auto rva : {party::current_context, party::event_dispatcher, party::event_post}) {
            bool found{};
            for (const auto &contract : party::party_request_contracts)
                if (contract.rva == rva) found = matches(base + rva, contract.bytes);
            if (!found) return false;
        }
        return true;
    }();
    return ok;
}
// Whether `object` is still the event's type: its record names the event and constructs
// through code (the same test the throwdown code makes before it constructs one).
bool type_alive(std::uintptr_t base, std::uintptr_t object) {
    std::uintptr_t record{}, construct{};
    std::uint32_t name{};
    if (!object || !memory::peek(object, record) || !memory::peek(record, name) || name != play_one_shot ||
        !memory::peek(record + 0x30, construct))
        return false;
    if (construct >= base && construct - base < supported_build::game_image_size) return true;
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<const void *>(construct), &info, sizeof(info)) && info.State == MEM_COMMIT &&
           !(info.Protect & PAGE_GUARD) &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
// Where the sound's number goes in an instance of the event: a 4-byte field.
std::optional<std::uint16_t> sound_offset(std::uintptr_t type) {
    std::uintptr_t record{}, fields{};
    std::uint16_t count{};
    if (!memory::peek(type, record) || !memory::peek(record + 0x2a, count) || !memory::peek(record + 0x60, fields) || !fields || count > 16)
        return std::nullopt;
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = fields + i * 0x18;
        std::uint32_t name{};
        std::uint16_t offset{}, size{};
        std::uintptr_t field_type{}, field_record{};
        if (!memory::peek(entry, name) || name != sound_field) continue;
        if (!memory::peek(entry + 8, offset) || !memory::peek(entry + 0x10, field_type) || !memory::peek(field_type, field_record) ||
            !memory::peek(field_record + 6, size) || size != 4)
            return std::nullopt;
        return offset;
    }
    return std::nullopt;
}
// Makes an instance of the event, sets the sound and raises it as the game raises its own
// events (native_party.h: the dispatcher copies the instance, which is then destroyed).
// False when a step faulted: the sounds are switched off then.
bool raise(std::uintptr_t base, std::uintptr_t type, std::uint16_t offset, std::uint32_t sound) noexcept {
    __try {
        const auto allocator = *reinterpret_cast<std::uintptr_t *>(base + throwdowns::event_allocator);
        const auto value = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t)>(base + throwdowns::type_construct)(type, allocator);
        if (!value) return true;
        *reinterpret_cast<std::uint32_t *>(value + offset) = sound;
        alignas(16) unsigned char context[16]{};
        reinterpret_cast<void (*)(void *)>(base + party::current_context)(context);
        const auto dispatcher = reinterpret_cast<std::uintptr_t (*)(void *)>(base + party::event_dispatcher)(context);
        if (dispatcher && *reinterpret_cast<std::uint8_t *>(dispatcher + 0x28)) {
            struct Options { float delay; std::uint32_t count; std::uint32_t flags; } options{0.f, 1, 0};
            reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t, const void *, void *, std::uintptr_t)>(base + party::event_post)(
                dispatcher, type, reinterpret_cast<const void *>(value), &options, 0);
        }
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t)>(base + throwdowns::type_destroy)(type, allocator, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

void queue_ui_sound(std::uint32_t sound) noexcept { wanted.store(sound, std::memory_order_relaxed); }

void tick_ui_sounds(std::uintptr_t base, bool loading) noexcept {
    const auto sound = wanted.exchange(none, std::memory_order_relaxed);
    if (sound == none || loading || !base || off.load(std::memory_order_relaxed)) return;
    try {
        if (!supported(base)) {
            off = true;
            logging::log(logging::Level::info, logging::Channel::ui, "Menu sounds: this game build differs from the supported one; ReSkate's pages stay silent.");
            return;
        }
        const auto type = type_object.load(std::memory_order_acquire);
        if (!type_alive(base, type)) {
            // Not found yet, or left behind with the last level: look for it off this thread (the
            // scan takes a few hundred milliseconds). This sound is skipped; the next ones play.
            type_object.store(0, std::memory_order_release);
            if (GetTickCount64() < scan_after.load(std::memory_order_relaxed) || scanning.exchange(true)) return;
            std::thread([] {
                NativeTypeQuery query{play_one_shot, 0};
                try {
                    find_native_types(std::span<NativeTypeQuery>(&query, 1), true);
                } catch (...) {}
                if (query.object) type_object.store(query.object, std::memory_order_release);
                else scan_after.store(GetTickCount64() + 5000, std::memory_order_relaxed);
                scanning.store(false);
            }).detach();
            return;
        }
        const auto offset = sound_offset(type);
        if (!offset || !raise(base, type, *offset, sound)) {
            off = true;
            logging::log(logging::Level::warning, logging::Channel::ui, "Menu sounds: the game's sound event could not be raised; ReSkate's pages stay silent.");
        }
    } catch (...) {
        scanning.store(false);
    }
}
} // namespace dingosdk::multiplayer
