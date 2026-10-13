#include "stack_check_report.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/texture_streaming.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <intrin.h>
#include <string>

namespace dingosdk::stack_check_report {
namespace {
namespace ts = addr::texture_streaming;
using Report = void (*)(std::uintptr_t cookie);
Report original{};
std::uintptr_t image{};
std::atomic<bool> installed{};

std::string place(std::uintptr_t address) {
    char text[40]{};
    if (address >= image && address - image < 0x10000000) sprintf_s(text, "Skate.exe+0x%llx", static_cast<unsigned long long>(address - image));
    else sprintf_s(text, "0x%llx", static_cast<unsigned long long>(address));
    return text;
}
// The game is about to end itself with a fast fail, which no crash handler sees: say where
// first. The return address is in the function whose stack was written over; the frames behind
// it may be part of what was written over.
void reported(std::uintptr_t cookie) {
    const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    try {
        void* frames[24]{};
        const auto count = CaptureStackBackTrace(1, 24, frames, nullptr);
        std::string trace;
        for (unsigned i = 0; i < count; ++i) trace += (i ? " < " : "") + place(reinterpret_cast<std::uintptr_t>(frames[i]));
        logging::log(logging::Level::error, logging::Channel::runtime,
            "The game's stack check failed in the function that returns to {} (it ends the game at once, with no crash report). Callers: {}",
            place(from), trace.empty() ? std::string("unknown") : trace);
        logging::flush();
    } catch (...) {}
    original(cookie);
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    image = base;
    std::array<unsigned char, 8> head{};
    if (!memory::peek(base + ts::stack_check_failed, head) || head != ts::stack_check_failed_bytes) return false;
    const auto target = reinterpret_cast<void*>(base + ts::stack_check_failed);
    if (hook_prepare(target, reinterpret_cast<void*>(&reported), reinterpret_cast<void**>(&original)) != HookOk || hook_enable(target) != HookOk) {
        original = nullptr;
        try {
            logging::log(logging::Level::warning, logging::Channel::runtime, "Stack check report: the game's routine could not be hooked.");
        } catch (...) {}
        return false;
    }
    return true;
}
} // namespace dingosdk::stack_check_report
