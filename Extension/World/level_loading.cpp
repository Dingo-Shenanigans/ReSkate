#include "level_loading.h"
#include "level_loading_progress.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Debug/trace.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/World/client_state.h"
#include "Engine/Game/World/world_model.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/level_loading.h"
#include "Extension/Assets/map_download.h"
#include "Extension/Boot/exit_watch.h"
#include "Engine/Core/Debug/crash_report.h"
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <thread>

namespace dingosdk {
namespace {
using logging::Level;
using logging::Channel;
using namespace addr::level_loading;
using Transition = void (*)(std::uintptr_t, std::uint32_t, std::uint32_t);
using Disconnect = void (*)(std::uintptr_t, std::uint32_t);
using StateRequest = void (*)(std::uintptr_t, std::uint32_t);
using memory::read;
constexpr auto highest = memory::highest_user_address;

using level_logging::Sample;
using level_logging::Progress;

struct Observer {
    std::uintptr_t base{};
    Transition original{};
    Disconnect original_disconnect{};
    StateRequest original_request{};
    BeforeLevelTransition before_transition{};
    std::atomic<bool> active{};
    std::atomic<std::uint64_t> next_poll{}, dropped{};
    bool drops_reported{};
    std::mutex mutex;
    Progress progress;
};
Observer& observer() { static auto* value = new Observer; return *value; }
void emit(Level level, std::string_view message) { logging::write(level, Channel::level, message); }

// Names the mod behind a custom level that is stuck or never arrived, with
// what built it and what the mod merge could not use from it, so a hanging
// load can be traced to a folder from the log alone.
std::string level_hint(std::string_view destination) {
    try {
        const auto& catalog = mods::catalog();
        const auto* mod = mods::mod_registering(catalog, destination);
        if (!mod) return {};
        std::string text = "; this level comes from mod " + mod->name;
        if (mod->tool.empty() && mod->version.empty() && mod->built.empty())
            text += " (no build info recorded: an older Studio or another tool built it)";
        else
            text += " (built by " + (mod->tool.empty() ? std::string("an unnamed tool") : mod->tool) +
                (mod->version.empty() ? "" : " " + mod->version) + (mod->built.empty() ? "" : " on " + mod->built) + ")";
        if (mod->problems.empty()) {
            text += "; the mod merge reported no problem with it";
        } else {
            text += "; the mod merge reported: ";
            constexpr std::size_t shown = 2;
            for (std::size_t index = 0; index < mod->problems.size() && index < shown; ++index)
                text += (index ? " | " : "") + mod->problems[index];
            if (mod->problems.size() > shown)
                text += " | and " + std::to_string(mod->problems.size() - shown) + " more in the startup log";
            text += "; reinstall the whole mod folder or rebuild it with a current ReSkate Studio";
        }
        return text;
    } catch (...) {
        return {};
    }
}

bool snapshot(std::uintptr_t client, Sample& s) {
    auto& o = observer();
    std::uintptr_t vt{};
    if (client < 0x10000 || client > highest - 0x440 ||
        !read(client, vt) || vt != o.base + addr::engine::client_vtable ||
        !read(client + 0xc4, s.state) || s.state > 26 ||
        !read(client + 0xc0, s.game_type) || s.game_type > 3) return false;
    s.client = client;
    s.queues_known = read(client + 0x120, s.pending) && s.pending <= 0x10000 &&
        read(client + 0x298, s.descriptions) && s.descriptions <= 0x10000;
    const auto description = read_world_description(GetCurrentProcess(), client + 0x198);
    if (description.available) {
        s.destination = description.level;
        if (!description.start_point.empty()) s.destination += " @ " + description.start_point;
        if (!description.lm_level.empty()) s.destination += " + " + description.lm_level;
        if (!description.lm_start_point.empty()) s.destination += " @ " + description.lm_start_point;
        if (!description.game_mode.empty()) s.destination += "; mode=" + description.game_mode;
        if (!description.hosted_mode.empty()) s.destination += "; hosted=" + description.hosted_mode;
    }
    unsigned after{};
    return read(client + 0xc4, after) && after == s.state && read(client, vt) &&
        vt == o.base + addr::engine::client_vtable;
}
std::chrono::milliseconds since_process_start() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return {};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return std::chrono::milliseconds(ticks(now) > ticks(created) ? (ticks(now) - ticks(created)) / 10000 : 0);
}
// The time spent in the level now loaded (GetTickCount64 when it became playable, 0 when none
// is) and its destination. Under the observer's lock.
std::uint64_t session_started{};
std::string session_destination;
// The frame pacing of the time spent in a level, for Sentry. Short stays say nothing about its
// frame rate. Called with the observer's lock held.
void send_level_session(std::uint64_t at) {
    if (!session_started) return;
    const auto pacing = trace::take_frame_pacing();
    const auto lasted = at >= session_started ? at - session_started : 0;
    if (lasted >= 10000 && pacing.frames && pacing.total_ns)
        trace::record("game.level_session", "app.session", std::chrono::milliseconds(lasted), true,
            {{"destination", session_destination}},
            {{"fps_avg", static_cast<double>(pacing.frames) * 1e9 / static_cast<double>(pacing.total_ns)},
             {"hitches", static_cast<double>(pacing.hitches)},
             {"worst_frame", static_cast<double>(pacing.worst_ns) / 1e6, "millisecond"}});
    session_started = 0;
}
// The player closing the game (exit_watch, on a thread of its own): the level they are in is
// sent now, and given a moment to go out. It used to be sent only when the next level began
// loading, so a stay that ended with the game closing (the long ones, as often as not) was
// never sent. A close the game turns down starts the stay afresh.
// A load still running is sent too, as one that ended with the game closed, with the stage it
// was in: a load timing went out only when the load finished, so the loads players gave up on
// were never seen at all (2026-10-21).
void send_level_session_on_close() noexcept {
    try {
        auto& o = observer();
        {
            std::unique_lock lock(o.mutex, std::defer_lock);
            for (int tries = 0; tries < 50 && !lock.try_lock(); ++tries) Sleep(10);
            if (!lock.owns_lock()) return;
            const auto now = GetTickCount64();
            const auto& p = o.progress;
            if (p.running && p.started && now >= p.started) {
                trace::record("game.level_load", "app.load", std::chrono::milliseconds(now - p.started), false,
                    {{"destination", p.destination}, {"first", p.sequence == 1 ? "true" : "false"}, {"ended", "closed"},
                     {"stage", client_state_name(p.state)}},
                    {{"stage_seconds", now >= p.entered ? static_cast<double>(now - p.entered) / 1000.0 : 0.0, "second"}});
            } else if (session_started) {
                send_level_session(now);
                session_started = now;
            } else {
                return;
            }
        }
        trace::flush(std::chrono::seconds(3));
    } catch (...) {}
}
// A load that has sat in one stage this long is reported while the game waits on: a stuck-load
// report (crash_report::capture_stuck_load), every thread's stack with the log, once a load and a
// few times a launch at most. 2.1.1's slowest loads that did finish took one to five minutes in
// all, most of it in one stage on San Van; the ones that never finished sent nothing, as their
// players closed the game (Sentry timings, 2026-10-21).
constexpr std::uint64_t stuck_after_ms = 120000;
constexpr unsigned most_stuck_reports = 3;
void watch_for_stuck_loads() {
    std::thread([] {
        std::uint64_t reported{}; // the load already reported (Progress::sequence)
        for (unsigned reports = 0; reports < most_stuck_reports;) {
            Sleep(5000);
            std::string stage, destination;
            std::uint64_t seconds{}, load{};
            try {
                auto& o = observer();
                std::unique_lock lock(o.mutex, std::try_to_lock);
                if (!lock.owns_lock()) continue;
                const auto& p = o.progress;
                const auto now = GetTickCount64();
                if (!p.running || !p.seen || !level_logging::busy(p.state) || p.sequence == reported ||
                    now < p.entered || now - p.entered < stuck_after_ms) continue;
                stage = client_state_name(p.state);
                destination = p.destination;
                seconds = (now - p.entered) / 1000;
                load = p.sequence;
            } catch (...) { continue; }
            // (A map this PC is fetching for a multiplayer session holds its load back on purpose.)
            if (map_download::view().stage != map_download::Stage::idle) continue;
            reported = load;
            ++reports;
            const bool sent = crash_report::capture_stuck_load(std::wstring(stage.begin(), stage.end()), static_cast<DWORD>(seconds));
            try {
                logging::log(Level::warning, Channel::level,
                    "Level load {} has been in {} for {} s{}; {}", load, stage, seconds,
                    destination.empty() ? std::string() : " (" + destination + ")",
                    sent ? "a stuck-load report with every thread's stack and this log was made, to find out why (the game goes on)."
                         : "no stuck-load report was made (crash reporting is off, or busy).");
            } catch (...) {}
        }
    }).detach();
}
// Timings for Sentry (Engine/Core/Debug/trace.h), from the same observations as the log:
// each load, the launch up to the first playable level, and the frame pacing of the time
// spent in a level. Called with the observer's lock held.
void trace_progress(const level_logging::Progress& progress, bool was_running, std::uint64_t started, std::uint64_t at) {
    // A new load ends the level before it.
    if (!was_running && progress.running) send_level_session(at);
    // A load that just reached a terminal state; "first" is the one behind the splash screen.
    if (!was_running || progress.running || progress.started != started || at < started) return;
    const bool active = progress.state == 13 || progress.state == 21;
    trace::record("game.level_load", "app.load", std::chrono::milliseconds(at - started), active,
        {{"destination", progress.destination}, {"first", progress.sequence == 1 ? "true" : "false"}});
    if (!active) return;
    if (progress.sequence == 1) trace::record("game.boot", "app.start", since_process_start());
    (void)trace::take_frame_pacing();
    session_started = at;
    session_destination = progress.destination;
}
bool observe(std::uintptr_t client, unsigned next, unsigned previous, std::uint64_t at, bool exact) noexcept {
    auto& o = observer();
    try {
        // Never wait on another observer or retain this lock across a native call.
        std::unique_lock lock(o.mutex, std::try_to_lock);
        if (!lock.owns_lock()) { ++o.dropped; return false; }
        Sample sample;
        sample.at = at;
        sample.previous = previous;
        if (!snapshot(client, sample) || (exact && (next > 26 || previous > 26 || sample.state != next))) return false;
        const auto was_running = o.progress.running;
        const auto started = o.progress.started;
        o.progress.observe(sample, exact, &emit);
        trace_progress(o.progress, was_running, started, sample.at);
        if (o.dropped.load() && !o.drops_reported) {
            o.drops_reported = true;
            emit(Level::warning, "Some level observations were skipped; recorded stage timings may be incomplete.");
        }
        return true;
    } catch (...) { ++o.dropped; return false; }
}
void transition(std::uintptr_t client, std::uint32_t next, std::uint32_t previous) {
    const auto incoming_error = GetLastError();
    auto& o = observer();
    const auto started = GetTickCount64();
    // Emit before forwarding so a stuck native handler leaves its stage in the log.
    const bool observed = observe(client, next, previous, started, true);
    // Lifecycle cleanup must not depend on the logging observer obtaining its
    // try-lock. State 24's native handler can unload assets in this same call.
    if (o.before_transition) o.before_transition(o.base, next);
    const auto native_started = GetTickCount64();
    SetLastError(incoming_error);
    o.original(client, next, previous);
    const auto outgoing_error = GetLastError();
    const auto elapsed = GetTickCount64() - native_started;
    if (observed && elapsed >= 1000) {
        try {
            std::unique_lock lock(o.mutex, std::try_to_lock);
            if (lock.owns_lock() && o.progress.client == client) o.progress.returned(next, elapsed, &emit);
        } catch (...) { ++o.dropped; }
    }
    SetLastError(outgoing_error);
}
bool compatible(std::uintptr_t base) noexcept {
    if (base < 0x10000 || base > highest - supported_build::game_image_size) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.SizeOfImage != supported_build::game_image_size)
        return false;
    for (const auto& contract : {level_transition_contract, level_transition_caller_contract}) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + contract.rva, actual) || actual != contract.bytes) return false;
    }
    return true;
}
std::string native_callers() {
    const auto& o = observer();
    std::array<void*, 16> frames{};
    const auto count = CaptureStackBackTrace(1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    std::string callers;
    for (unsigned i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (address >= o.base && address - o.base < supported_build::game_image_size)
            callers += std::format(" Skate+{:#x}", address - o.base);
    }
    return callers;
}
void request_state(std::uintptr_t client, std::uint32_t next) {
    auto& o = observer();
    const auto incoming_error = GetLastError();
    if (next == 14 || next == 22 || next == 24) {
        try {
            std::uint32_t previous{}, game_type{}, connection_offset{}, state{}, reason{};
            std::uintptr_t context{}, connection{};
            read(client + 0xc4, previous);
            read(client + 0xc0, game_type);
            if (read(client + 8, context) && context &&
                read(o.base + addr::engine::context_client_connection_offset, connection_offset) && connection_offset < 0x1000000 &&
                read(context + connection_offset, connection) && connection) {
                read(connection + 0x120, state);
                read(connection + 0x124, reason);
            }
            logging::log(Level::info, Channel::level,
                "Native level exit requested: {} -> {}, game_type={}, connection={:#x}, state={}, reason={:#x}, callers:{}.",
                previous, next, game_type, connection, state, reason, native_callers());
        } catch (...) {}
    }
    SetLastError(incoming_error);
    o.original_request(client, next);
}
void disconnect(std::uintptr_t connection, std::uint32_t reason) {
    auto& o = observer();
    const auto incoming_error = GetLastError();
    // The normal shutdown reason is uninteresting. Record the origin of an
    // unsolicited native disconnect before it drops the local hosted world.
    if (reason != 0x102cbecd) {
        try {
            logging::log(Level::warning, Channel::level,
                "Native connection disconnect: reason={:#x}, connection={:#x}, callers:{}.",
                reason, connection, native_callers());
        } catch (...) {}
    }
    SetLastError(incoming_error);
    o.original_disconnect(connection, reason);
}
void trace_state_requests() {
    auto& o = observer();
    std::array<unsigned char, 32> bytes{};
    if (!read(o.base + level_request_contract.rva, bytes) || bytes != level_request_contract.bytes) {
        emit(Level::warning, "Native level-exit tracing unavailable: function contract differs.");
        return;
    }
    auto* target = reinterpret_cast<void*>(o.base + level_request_contract.rva);
    void* original{};
    if (hook_prepare(target, reinterpret_cast<void*>(&request_state), &original) != HookOk) {
        emit(Level::warning, "Native level-exit tracing unavailable: hook preparation failed.");
        return;
    }
    o.original_request = reinterpret_cast<StateRequest>(original);
    if (hook_enable(target) != HookOk) {
        (void)hook_remove(target);
        emit(Level::warning, "Native level-exit tracing unavailable: hook could not be enabled.");
    }
}
void trace_disconnects() {
    auto& o = observer();
    std::array<unsigned char, 32> bytes{};
    if (!read(o.base + connection_disconnect_contract.rva, bytes) || bytes != connection_disconnect_contract.bytes) {
        emit(Level::warning, "Native disconnect tracing unavailable: function contract differs.");
        return;
    }
    auto* target = reinterpret_cast<void*>(o.base + connection_disconnect_contract.rva);
    void* original{};
    if (hook_prepare(target, reinterpret_cast<void*>(&disconnect), &original) != HookOk) return;
    o.original_disconnect = reinterpret_cast<Disconnect>(original);
    if (hook_enable(target) != HookOk) {
        (void)hook_remove(target);
        emit(Level::warning, "Native disconnect tracing unavailable: hook could not be enabled.");
    }
}
}

bool start_level_loading_logging(std::uintptr_t base, BeforeLevelTransition before_transition) noexcept {
    try {
        auto& o = observer();
        if (o.active.load()) return true;
        if (!compatible(base)) {
            emit(Level::warning, "Native level logging contract did not match; using sampled loading messages.");
            return false;
        }
        o.base = base;
        o.before_transition = before_transition;
        exit_watch::when_closing(&send_level_session_on_close);
        auto* target = reinterpret_cast<void*>(base + level_transition_contract.rva);
        void* original{};
        auto status = hook_prepare(target, reinterpret_cast<void*>(&transition), &original);
        if (status == HookOk) {
            o.original = reinterpret_cast<Transition>(original);
            status = hook_enable(target);
            if (status != HookOk) {
                const auto failure = hook_last_failure();
                logging::log(Level::warning, Channel::level,
                    "Native level logging hook failed: status={}, operation={}, thread={}; using sampled loading messages.",
                    static_cast<LONG>(status), failure.operation, failure.thread_id);
                (void)hook_remove(target);
                return false;
            }
        } else {
            logging::log(Level::warning, Channel::level,
                "Native level logging hook preparation failed: {}; using sampled loading messages.", static_cast<LONG>(status));
            return false;
        }
        o.progress.hint = &level_hint;
        o.active.store(true);
        trace_disconnects();
        trace_state_requests();
        watch_for_stuck_loads();
        emit(Level::info, "Native level-loading stage logging enabled.");
        return true;
    } catch (...) { return false; }
}
std::string last_level_destination() noexcept {
    try {
        auto& o = observer();
        std::unique_lock lock(o.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return {};
        return o.progress.destination;
    } catch (...) {
        return {};
    }
}
bool level_content_active() noexcept {
    try {
        auto& o = observer();
        std::unique_lock lock(o.mutex, std::try_to_lock);
        return lock.owns_lock() && o.progress.seen && (o.progress.state == 13 || o.progress.state == 21);
    } catch (...) {
        return false;
    }
}
std::string custom_level_hint(std::string_view destination) noexcept { return level_hint(destination); }
std::string custom_level_notice(std::string_view destination) noexcept {
    try {
        const auto* mod = mods::mod_registering(mods::catalog(), destination);
        if (!mod) return {};
        std::string text = "It comes from mod " + mod->name;
        text += mod->problems.empty()
            ? ", which the mod merge reported no problem with."
            : ", which the mod merge could not use in full. Reinstall the whole mod folder, or rebuild it "
              "with a current ReSkate Studio.";
        return text + " Details are in ReSkate.log.";
    } catch (...) {
        return {};
    }
}
void poll_level_loading(std::uintptr_t client) noexcept {
    const auto error = GetLastError();
    auto& o = observer();
    const auto now = GetTickCount64();
    auto next = o.next_poll.load();
    if (o.active.load() && now >= next && o.next_poll.compare_exchange_strong(next, now + 500))
        (void)observe(client, 26, 26, now, false);
    SetLastError(error);
}
}
