#pragma once
#include "Engine/Core/Log/types.h"
#include "Engine/Core/Debug/crash_report.h"
#include "Engine/Game/World/client_state.h"
#include <cstdint>
#include <format>
#include <string>

namespace dingosdk::level_logging {
using logging::Level;
using Emit = void (*)(Level, std::string_view);
// Extra words for a destination that is taking too long or never arrived:
// which mod a custom level came from, and what was known to be wrong with it.
using Hint = std::string (*)(std::string_view destination);
inline bool busy(unsigned state) noexcept {
    return state < 24 && state != 2 && state != 13 && state != 21;
}
struct Sample {
    std::uintptr_t client{};
    std::uint64_t at{};
    unsigned state{26}, previous{26}, game_type{}, pending{}, descriptions{};
    bool queues_known{};
    std::string destination;
};

// Only SDK-owned data is retained between callbacks. No native pointer is
// dereferenced later to reconstruct a past transition or its destination.
struct Progress {
    std::uintptr_t client{};
    unsigned state{26}, stage_lines{}, slow_handlers{}, debug_lines{}, destination_lines{}, wait_warnings{}, slowest_state{26};
    std::uint64_t started{}, entered{}, sequence{}, slowest_ms{};
    bool running{}, seen{}, warned{};
    std::string destination;
    Hint hint{};

    void observe(const Sample& s, bool exact_transition, Emit emit) {
        if (s.state > 26 || s.previous > 26) return;
        if (client != s.client) {
            const auto last_sequence = sequence;
            const auto last_hint = hint;
            *this = {};
            sequence = last_sequence;
            hint = last_hint;
            client = s.client;
        }
        if (busy(s.state) && !running) {
            running = true;
            started = s.at;
            stage_lines = slow_handlers = debug_lines = destination_lines = wait_warnings = 0;
            slowest_ms = 0; slowest_state = 26;
            seen = false;
            destination.clear();
            ++sequence;
        }
        const bool changed = !seen || state != s.state;
        const auto previous_ms = seen && s.at >= entered ? s.at - entered : 0;
        const auto elapsed = running && s.at >= started ? s.at - started : 0;
        if (changed) {
            if (running && previous_ms > slowest_ms) { slowest_ms = previous_ms; slowest_state = state; }
            entered = s.at; warned = false;
        }
        // The previous world's descriptor can survive unloading/server startup.
        // Name the destination only once the new load reaches its asset stage.
        if (((s.state >= 8 && s.state <= 13) || (s.state >= 16 && s.state <= 21)) &&
            !s.destination.empty() && s.destination != destination) {
            destination = s.destination;
            // (A crash report from here on is tagged with this level: crash_report.h.)
            dingosdk::crash_report::set_destination(std::wstring(destination.begin(), destination.end()));
            if (destination_lines++ < 4)
                emit(Level::info, std::format("Level destination: {}.", destination));
        }
        const auto queues = s.queues_known
            ? std::format("{} queued transitions, {} queued descriptions", s.pending, s.descriptions)
            : std::string("queue counts unavailable");
        if ((exact_transition || changed) && debug_lines++ < 128) {
            emit(Level::debug, std::format("{}: {} ({}) -> {} ({}); {:.3f}s in previous observed state; {}; game type {}.",
                exact_transition ? "Native level transition" : "Level state sample",
                client_state_name(exact_transition ? s.previous : state), exact_transition ? s.previous : state,
                client_state_name(s.state), s.state, static_cast<double>(previous_ms) / 1000.0,
                queues, s.game_type));
        }
        const auto severity = s.state == 2 ? Level::warning :
            (s.state == 13 || s.state == 21) ? Level::success : Level::info;
        if (changed && s.state != 26 && stage_lines++ < 128) {
            emit(severity, running ? std::format("{} [{}] (load {}, {:.2f}s elapsed, {:.3f}s in previous observed state; {}).",
                client_state_name(s.state), s.state, sequence, static_cast<double>(elapsed) / 1000.0,
                static_cast<double>(previous_ms) / 1000.0, queues) :
                std::format("{} [{}] ({}).", client_state_name(s.state), s.state, queues));
        }
        // Even a noisy/retrying state machine must leave a completion record
        // once it reaches a terminal state, after the bounded stage detail.
        if (running && !busy(s.state)) {
            const bool active = s.state == 13 || s.state == 21;
            const auto where = destination.empty() ? std::string{}
                : "; " + destination + (!active && hint ? hint(destination) : std::string{});
            emit(severity, std::format("Load {} {} after {:.2f}s{}; longest observed stage: {} ({:.2f}s).",
                sequence, active ? "content active" : "ended before content became active",
                static_cast<double>(elapsed) / 1000.0, where,
                client_state_name(slowest_state), static_cast<double>(slowest_ms) / 1000.0));
        }
        state = s.state;
        seen = true;
        if (!busy(state)) running = false;
        // One warning per stalled state, at most three per load. Polling keeps
        // a long wait visible without producing per-frame or per-asset output.
        if (running && !warned && s.at >= entered && s.at - entered >= 30000) {
            warned = true;
            if (wait_warnings++ < 3)
                emit(Level::warning, std::format("Level load {} still waiting: {} for {:.1f}s ({:.1f}s total); {}{}.",
                    sequence, client_state_name(state), static_cast<double>(s.at - entered) / 1000.0,
                    static_cast<double>(elapsed) / 1000.0, queues,
                    destination.empty() ? std::string{}
                        : "; " + destination + (hint ? hint(destination) : std::string{})));
        }
    }
    void returned(unsigned next, std::uint64_t duration, Emit emit) {
        if (next > 26 || duration < 1000 || (slow_handlers & (1u << next))) return;
        slow_handlers |= 1u << next;
        emit(Level::warning, std::format("Native {} transition handler took {:.2f}s.",
            client_state_name(next), static_cast<double>(duration) / 1000.0));
    }
};
}
