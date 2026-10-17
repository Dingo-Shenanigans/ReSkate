#include "rock_to_fakie.h"
#include "no_bail.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/rock_to_fakie.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace dingosdk::rock_to_fakie {
namespace {
namespace rtf = addr::rock_to_fakie;

using RecordTrick = void (*)(std::uintptr_t recorder, std::uintptr_t packet);
using ResolveLocalizedKey = bool (*)(std::uintptr_t object, const char* key, char* buffer, std::uint32_t capacity);

constexpr const char* enabled_preference = "RockToFakie";
constexpr char replacement[] = "Rock to Fakie";
constexpr std::uint16_t button_b = 0x2000; // XINPUT_GAMEPAD_B (Circle on a PlayStation pad)
// Measured windows (rock_to_fakie.h): B is sampled on the client tick, the stall's end on the
// physics thread, and the recorder is called right after the selector leaves the stall.
constexpr double b_window_seconds = 0.125, exit_window_seconds = 0.1;

struct State {
    std::atomic<std::uintptr_t> base{};
    std::atomic<bool> started{}, on{};
    std::atomic<RecordTrick> record_original{};
    std::atomic<ResolveLocalizedKey> resolve_original{};
    std::atomic<bool> record_ready{}, label_ready{};
    // Client tick -> the trick hook: when B was last pressed while the stall was held (QPC).
    std::atomic<std::int64_t> b_pressed{};
    bool b_down{}; // client thread only
    std::atomic<std::uint64_t> labels{}, remaps{}, route_misses{}, short_buffers{};
};
State& state() {
    static auto* value = new State; // process lifetime: hooks may call in until exit
    return *value;
}
struct PreserveError {
    DWORD value = GetLastError();
    ~PreserveError() { SetLastError(value); }
};
std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
std::int64_t qpc_ticks(double seconds) noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<std::int64_t>(seconds * frequency);
}
bool held(std::uint32_t physics_state) noexcept {
    return physics_state == rtf::stall_held_a || physics_state == rtf::stall_held_b;
}

// Game thread(s). The game's method runs first and fills its caller's buffer; only then, and only
// for the stall label on the ticker's route, is the text in that buffer replaced.
__declspec(noinline) bool resolve_localized_key(std::uintptr_t object, const char* key, char* buffer, std::uint32_t capacity) {
    auto& s = state();
    const bool success = s.resolve_original.load(std::memory_order_acquire)(object, key, buffer, capacity);
    if (!success || !buffer || !capacity || !s.on.load(std::memory_order_relaxed)) return success;
    PreserveError preserve;
    const auto watch = watched_physics_state();
    if (!watch.valid || !held(watch.state)) return success;
    char label[16]{};
    const auto length = memory::peek_cstring(reinterpret_cast<std::uintptr_t>(buffer), label, sizeof(label));
    if (length <= 0 || static_cast<std::uint32_t>(length) >= capacity) return success;
    if (std::strcmp(label, "Board Stall") != 0 && std::strcmp(label, "Lip Stall") != 0) return success;
    // Captured here, in the hook itself: frame 0 is this function, frame 1 its caller.
    std::array<void*, 2 + rtf::ticker_label_route.size()> frames{};
    const auto count = RtlCaptureStackBackTrace(0, static_cast<ULONG>(frames.size()), frames.data(), nullptr);
    const auto base = s.base.load(std::memory_order_relaxed);
    bool route = count == frames.size();
    for (std::size_t i = 0; route && i < rtf::ticker_label_route.size(); ++i)
        route = reinterpret_cast<std::uintptr_t>(frames[2 + i]) == base + rtf::ticker_label_route[i];
    if (!route) {
        s.route_misses.fetch_add(1, std::memory_order_relaxed);
        return success;
    }
    if (capacity < sizeof(replacement)) { // never cut the label short
        s.short_buffers.fetch_add(1, std::memory_order_relaxed);
        return success;
    }
    std::memcpy(buffer, replacement, sizeof(replacement));
    s.labels.fetch_add(1, std::memory_order_relaxed);
    return success;
}

// Game thread. A stall left with B within the windows above is recorded as fakie_rock, through a
// copy of the packet; anything else goes to the recorder untouched.
__declspec(noinline) void record_trick(std::uintptr_t recorder, std::uintptr_t packet) {
    auto& s = state();
    const auto original = s.record_original.load(std::memory_order_acquire);
    if (s.on.load(std::memory_order_relaxed) && packet >= 0x10000 &&
        packet < memory::highest_user_address - rtf::trick_packet_size) {
        PreserveError preserve;
        const auto watch = watched_physics_state();
        const auto now = qpc_now();
        const auto pressed = s.b_pressed.load(std::memory_order_acquire);
        const bool left_with_b = watch.valid && watch.state == rtf::stall_exit && held(watch.previous) &&
            pressed && now >= pressed && now - pressed <= qpc_ticks(b_window_seconds) &&
            watch.since && now >= watch.since && now - watch.since <= qpc_ticks(exit_window_seconds);
        std::uint8_t valid{};
        std::uint32_t id{};
        if (left_with_b && memory::peek(packet + rtf::trick_packet_valid, valid) && valid &&
            memory::peek(packet + rtf::trick_packet_id, id) && (id == rtf::board_stall_id || id == rtf::lip_stall_id)) {
            std::array<unsigned char, rtf::trick_packet_size> copy{};
            if (memory::peek(packet, copy)) {
                const std::uint32_t fakie_rock = rtf::fakie_rock_id;
                std::memcpy(copy.data() + rtf::trick_packet_id, &fakie_rock, sizeof(fakie_rock));
                s.remaps.fetch_add(1, std::memory_order_relaxed);
                // The recorder copies the packet before it returns, so the copy may live here.
                original(recorder, reinterpret_cast<std::uintptr_t>(copy.data()));
                return;
            }
        }
    }
    original(recorder, packet);
}

template<class Function, std::size_t N>
bool install(std::uintptr_t base, std::uintptr_t rva, const std::array<unsigned char, N>& prefix,
             Function replacement, std::atomic<Function>& original) noexcept {
    std::array<unsigned char, N> actual{};
    if (!memory::peek(base + rva, actual) || actual != prefix) return false;
    auto* const target = reinterpret_cast<void*>(base + rva);
    void* previous{};
    if (hook_prepare(target, reinterpret_cast<void*>(replacement), &previous) != HookOk) return false;
    if (!previous) {
        (void)hook_remove(target);
        return false;
    }
    original.store(reinterpret_cast<Function>(previous), std::memory_order_release);
    if (hook_enable(target) != HookOk) {
        (void)hook_remove(target);
        return false;
    }
    return true;
}

std::string status() {
    auto& s = state();
    if (!s.started.load(std::memory_order_acquire)) return "Rock to Fakie is unavailable: No Bail did not start.";
    return std::format("Rock to Fakie {} (kept). Ticker label {}, fakie_rock recording {}. This game: {} stall label(s) "
                       "renamed, {} trick(s) recorded as fakie_rock; {} stall label(s) left alone (other route), {} (no room).",
        s.on.load() ? "ON" : "OFF", s.label_ready.load() ? "ready" : "UNAVAILABLE (this build's code differs)",
        s.record_ready.load() ? "ready" : "UNAVAILABLE (this build's code differs)", s.labels.load(), s.remaps.load(),
        s.route_misses.load(), s.short_buffers.load());
}
} // namespace

bool start(std::uintptr_t image_base) noexcept {
    auto& s = state();
    if (s.started.load(std::memory_order_acquire)) return true;
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Rock to Fakie is unavailable: it follows the skater's physics state through No Bail, which did not start.");
        return false;
    }
    s.base.store(image_base, std::memory_order_release);
    s.on.store(profile_runtime::local_preference(enabled_preference).value_or(false), std::memory_order_release);
    s.label_ready.store(install(image_base, rtf::resolve_localized_key, rtf::resolve_localized_key_prefix,
        &resolve_localized_key, s.resolve_original), std::memory_order_release);
    s.record_ready.store(install(image_base, rtf::record_trick, rtf::record_trick_prefix, &record_trick, s.record_original),
        std::memory_order_release);
    s.started.store(true, std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater, "Rock to Fakie ready ({}; ticker label {}, fakie_rock recording {}).",
        s.on.load() ? "on" : "off", s.label_ready.load() ? "hooked" : "unavailable", s.record_ready.load() ? "hooked" : "unavailable");
    return s.label_ready.load() || s.record_ready.load();
}

void on_client_tick(std::uintptr_t client) noexcept {
    auto& s = state();
    if (!s.started.load(std::memory_order_acquire) || !s.on.load(std::memory_order_acquire)) {
        s.b_down = false;
        return;
    }
    NoBailSkater skater;
    if (!client || !no_bail_skater(skater)) {
        s.b_down = false;
        return;
    }
    watch_physics_state(client, skater.entity);
    ControllerInput pad{};
    DingoSDKOverlayReadControllerInput(&pad);
    const bool down = pad.available && (pad.buttons & button_b) != 0;
    if (down && !s.b_down) {
        const auto watch = watched_physics_state();
        if (watch.valid && held(watch.state)) s.b_pressed.store(qpc_now(), std::memory_order_release);
    }
    s.b_down = down;
}

std::string command(const std::vector<std::string>& words) {
    auto& s = state();
    const auto word = words.empty() ? std::string("status") : words.front();
    if (word == "status") return status();
    if (word != "on" && word != "off") return "Usage: rocktofakie on|off|status";
    if (!s.started.load(std::memory_order_acquire)) return status();
    const bool on = word == "on";
    if (on && !s.label_ready.load() && !s.record_ready.load())
        return "Rock to Fakie cannot start: this game build's code differs from the one it was made for.";
    s.b_pressed.store(0, std::memory_order_release);
    s.on.store(on, std::memory_order_release);
    profile_runtime::set_local_preference(enabled_preference, on);
    return on ? "Rock to Fakie ON: a Board Stall or Lip Stall shows as Rock to Fakie, and left with B it is recorded as one."
              : "Rock to Fakie OFF: stalls are the game's own again.";
}

bool available() noexcept {
    const auto& s = state();
    return s.started.load(std::memory_order_acquire) && (s.label_ready.load() || s.record_ready.load());
}
bool enabled() noexcept { return state().on.load(std::memory_order_acquire); }
}
