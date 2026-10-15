#include "replay_export.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_export.h"
#include "Engine/Game/Build/supported_build.h"
#include <array>
#include <atomic>
#include <cstring>
#include <Windows.h>

namespace dingosdk::replay_export {
namespace {
namespace native = addr::replay_export;
// The encoder passes its arguments straight on to the codec, so all four
// register arguments are forwarded as they came.
using Submit = void (*)(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch, std::uintptr_t unused);
std::atomic<Submit> original{};
std::atomic<std::uintptr_t> image{};

bool force_export_bitrate(std::uintptr_t base) noexcept {
    const DWORD saved_error = GetLastError();
    const auto finish = [saved_error](bool result) {
        SetLastError(saved_error);
        return result;
    };
    if (!base || native::bitrate_switch >= supported_build::game_image_size ||
        native::bitrate_switch_prefix.size() > supported_build::game_image_size - native::bitrate_switch)
        return finish(false);

    const auto address = base + native::bitrate_switch;
    auto* const destination = reinterpret_cast<void*>(address);
    std::array<unsigned char, native::bitrate_switch_prefix.size()> actual{};
    if (!memory::read_bytes(address, actual.data(), actual.size())) return finish(false);

    bool needs_write{};
    for (std::size_t i = 0; i < actual.size(); ++i) {
        bool bitrate_byte{};
        for (std::size_t slot = 0; slot < native::bitrate_immediate_offsets.size(); ++slot) {
            const auto offset = native::bitrate_immediate_offsets[slot];
            if (i < offset || i >= offset + sizeof(std::uint32_t)) continue;
            bitrate_byte = true;
            if (i == offset) {
                std::uint32_t current{};
                std::memcpy(&current, actual.data() + offset, sizeof(current));
                if (current != native::stock_bitrates_kbps[slot] && current != native::replay_bitrate_kbps)
                    return finish(false);
                needs_write |= current != native::replay_bitrate_kbps;
            }
            break;
        }
        if (!bitrate_byte && actual[i] != native::bitrate_switch_prefix[i]) return finish(false);
    }
    if (!needs_write) return finish(true);

    DWORD previous{};
    if (!VirtualProtect(destination, actual.size(), PAGE_EXECUTE_READWRITE, &previous)) return finish(false);
    for (const auto offset : native::bitrate_immediate_offsets)
        std::memcpy(actual.data() + offset, &native::replay_bitrate_kbps, sizeof(native::replay_bitrate_kbps));
    std::memcpy(destination, actual.data(), actual.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, actual.size()) != FALSE;
    DWORD discarded{};
    const bool restored = VirtualProtect(destination, actual.size(), previous, &discarded) != FALSE;
    return finish(flushed && restored);
}

void submit(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch, std::uintptr_t unused) {
    // Encoding is done on this thread before the call returns, so letting every
    // frame through also holds the renderer to the pace the encoder keeps.
    std::uintptr_t vtable{};
    if (memory::peek(encoder, vtable) && vtable == image.load() + native::replay_encoder_vtable)
        *reinterpret_cast<std::uint8_t*>(encoder + native::frame_permit) = 1;
    original.load()(encoder, pixels, pitch, unused);
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    try {
        const bool bitrate = force_export_bitrate(base);
        logging::write(bitrate ? logging::Level::info : logging::Level::warning,
            logging::Channel::graphics, bitrate
                ? "Replay video export bitrate override active: 100 Mbps H.264."
                : "Replay video export bitrate override skipped: game code did not match the supported build.");

        std::array<unsigned char, native::submit_frame_prefix.size()> bytes{};
        if (!base || !memory::read_bytes(base + native::submit_frame, bytes.data(), bytes.size()) ||
            bytes != native::submit_frame_prefix) {
            logging::write(logging::Level::warning, logging::Channel::graphics,
                "Replay export frame pacing hook skipped: game code did not match the supported build.");
            return bitrate;
        }
        image = base;
        auto* target = reinterpret_cast<void*>(base + native::submit_frame);
        void* previous{};
        if (hook_prepare(target, reinterpret_cast<void*>(&submit), &previous) != HookOk) return bitrate;
        original = reinterpret_cast<Submit>(previous);
        if (hook_enable(target) != HookOk) {
            hook_remove(target);
            return bitrate;
        }
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::replay_export
