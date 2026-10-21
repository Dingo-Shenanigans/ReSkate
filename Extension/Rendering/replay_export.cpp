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
        std::array<unsigned char, native::submit_frame_prefix.size()> bytes{};
        if (!base || !memory::read_bytes(base + native::submit_frame, bytes.data(), bytes.size()) ||
            bytes != native::submit_frame_prefix) {
            logging::write(logging::Level::warning, logging::Channel::graphics,
                "Replay export frame pacing hook skipped: game code did not match the supported build.");
            return false;
        }
        image = base;
        auto* target = reinterpret_cast<void*>(base + native::submit_frame);
        void* previous{};
        if (hook_prepare(target, reinterpret_cast<void*>(&submit), &previous) != HookOk) return false;
        original = reinterpret_cast<Submit>(previous);
        if (hook_enable(target) != HookOk) {
            hook_remove(target);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::replay_export
