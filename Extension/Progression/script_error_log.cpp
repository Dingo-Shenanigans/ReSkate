#include "script_natives.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/script_natives.h"
#include <mutex>
#include <set>
#include <string>
#include "local_challenge_trace.h"
#include "local_entitlement_trigger_runtime.h"

namespace dingosdk::profile_runtime {
namespace {
void script_error(std::uintptr_t, std::uintptr_t*, std::uintptr_t* arguments) {
    PreserveError preserve;
    // Authored graphs raise the same error every frame, from the same constant text: a text
    // pointer already looked at is not read again (each character was a system call).
    static std::mutex mutex; static std::set<std::string> seen; static std::set<std::uintptr_t> looked_at, arguments_seen;
    std::lock_guard lock(mutex);
    try {
        // The message position is not known from the binding alone; take the
        // first argument that dereferences to readable text.
        const auto printable = [](std::uintptr_t at) {
            std::string value;
            if (looked_at.contains(at)) return value;
            if (looked_at.size() < 4096) looked_at.insert(at);
            for (std::size_t i = 0; at && i < 400; ++i) {
                char c{};
                if (!memory::peek(at + i, c) || !c) break; // no system call per character
                if (static_cast<unsigned char>(c) < 9 || static_cast<unsigned char>(c) > 126) return std::string{};
                value += static_cast<unsigned char>(c) < 32 ? ' ' : c;
            }
            return value.size() >= 8 ? value : std::string{};
        };
        std::string message;
        for (std::size_t index = 0; arguments && index < 4 && message.empty(); ++index) {
            std::uintptr_t argument{}, inner{};
            if (!memory::peek(reinterpret_cast<std::uintptr_t>(arguments) + index * 8, argument) || argument < 0x10000) break;
            // The same arguments come back every frame: each is probed once (read, not peeked:
            // it is often not a pointer, and a failed peek costs an exception).
            if (arguments_seen.contains(argument)) continue;
            if (arguments_seen.size() < 4096) arguments_seen.insert(argument);
            if (read(argument, inner) && inner >= 0x10000) message = printable(inner);
            if (message.empty()) message = printable(argument);
        }
        if (message.empty()) return;
        std::uintptr_t resource{}; std::uint32_t graph{};
        if (executing_expression) challenge_trace_identity(executing_expression, resource, graph);
        // One line per distinct message; authored graphs repeat theirs per frame.
        if (seen.size() < 256 && seen.insert(message).second)
            logging::log(logging::Level::info, logging::Channel::progression,
                "Script error (graph {:#x}): {}", graph, message);
    } catch (...) {}
}
}

void install_script_error_log(std::uintptr_t base) noexcept {
    static bool attempted{};
    if (attempted || !base) return;
    attempted = true;
    constexpr std::uintptr_t record = addr::script_natives::script_error_record, stub = addr::script_natives::script_error_stub;
    try {
        std::uint32_t hash{};
        if (!read(base + record, hash) || hash != 0xa5910db3) return;
        unsigned patched{};
        for (const auto offset : {0x30u, 0x40u, 0x48u}) {
            // Only a slot still bound to the shared no-op stub is rebound.
            if (patch_table_slot("script error binding", reinterpret_cast<void**>(base + record + offset),
                    reinterpret_cast<void*>(base + stub), reinterpret_cast<void*>(&script_error)) == HookOk)
                ++patched;
        }
        logging::log(logging::Level::info, logging::Channel::progression,
            "Script error log: {} binding slot(s) redirected.", patched);
    } catch (...) {}
}
}
