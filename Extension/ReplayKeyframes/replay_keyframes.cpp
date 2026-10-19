#include "replay_keyframes.h"
#include "replay_curve.h"
#include "replay_settings.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_keyframes.h"
#include <Windows.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace dingosdk::replay_keyframes {
namespace {
namespace native = addr::replay_keyframes;
using SegmentBuild = void* (*)(void* manager, void* segment, void* keys_end, void** key);
using SlotBuild = void (*)(void* manager, const void* value, double time, int behaviour, int subject, void* slot);
using SlotUnwrap = void (*)(void* adjust, const void* reference, int behaviour);
using Rebuild = void (*)(void* manager, int session);

// More keys than any replay has: a walk that goes on longer has lost its way.
constexpr std::size_t most_keys = 4096;
static_assert(native::slot_size % 16 == 0, "slots hold vec4s, one after another in a vector");

std::atomic<bool> started{}, framing_hooked{};
std::atomic<SegmentBuild> build_original{};
std::atomic<Rebuild> rebuild_original{};
SlotBuild slot_build{};
SlotUnwrap slot_unwrap{};
Rebuild rebuild{};
std::uintptr_t image{};

// The player's settings (replay_settings.h), loaded at start and saved at every change.
std::atomic<bool> enabled{};
std::atomic<float> smooth{1.0f};
// The session whose framing keys the game built last (the one the camera plays): 1 << 32 | its id.
std::atomic<std::uint64_t> last_session{};
// Segments by Behaviour (0, 1, 2, other) and Subject (0..7, other).
constexpr std::size_t behaviours = 4, subjects = 9;
std::size_t behaviour_index(int behaviour) noexcept {
    return behaviour >= 0 && behaviour < static_cast<int>(behaviours) - 1 ? static_cast<std::size_t>(behaviour) : behaviours - 1;
}
std::size_t subject_index(int subject) noexcept {
    return subject >= 0 && subject < static_cast<int>(subjects) - 1 ? static_cast<std::size_t>(subject) : subjects - 1;
}
struct Counters {
    std::array<std::array<std::atomic<std::uint64_t>, subjects>, behaviours> built;
    std::atomic<std::uint64_t> smoothed, stock, mismatch, filtered;
} counters;

// A rebuild builds one segment per key, and every segment needs every key of the session as a
// slot: built once per rebuild (per Behaviour and Subject) and kept here for the rest of its
// segments, instead of once per segment. Only inside a rebuild, on its own thread, while nothing
// can change the keys; a segment built anywhere else builds its own.
struct KeySlots {
    std::uintptr_t keys_end{};
    int behaviour{}, subject{};
    std::vector<std::uintptr_t> nodes;
    std::vector<float> slots;  // as slot_build made them, before unwrapping
};
thread_local int rebuilding{};
thread_local std::vector<KeySlots> rebuild_slots;

std::uintptr_t at(std::uintptr_t address) noexcept { return *reinterpret_cast<const std::uintptr_t*>(address); }
// The next node of an EASTL map, as the game steps through one (0x2714d50): node [0] right, [1] left,
// [2] parent. After the last node comes the map's anchor.
std::uintptr_t successor(std::uintptr_t node) noexcept {
    auto next = at(node);
    if (!next) {
        next = at(node + 16);
        if (node == at(next)) {
            std::uintptr_t climbed;
            do {
                climbed = next;
                next = at(climbed + 16);
            } while (climbed == at(next));
            if (at(climbed) == next) next = climbed;
            return next;
        }
        if (!next) next = node;
    } else if (at(next + 8)) {
        do next = at(next + 8);
        while (at(next + 8));
    }
    return next;
}

bool matches(std::uintptr_t base, const game::build::Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + contract.rva, bytes) && bytes == contract.bytes;
}
template <class Function>
bool hook(std::uintptr_t base, const game::build::Fingerprint& contract, Function replacement, std::atomic<Function>& original) noexcept {
    auto* const target = reinterpret_cast<void*>(base + contract.rva);
    void* previous{};
    if (hook_prepare(target, reinterpret_cast<void*>(replacement), &previous) != HookOk) return false;
    original = reinterpret_cast<Function>(previous);
    if (hook_enable(target) != HookOk) {
        hook_remove(target);
        original = nullptr;
        return false;
    }
    return true;
}

int segment_int(const void* segment, std::size_t offset) noexcept {
    int value{};
    std::memcpy(&value, static_cast<const unsigned char*>(segment) + offset, sizeof(value));
    return value;
}

SegmentResult smooth_framing(void* manager, unsigned char* segment, std::uintptr_t keys_end, std::uintptr_t key,
                             int behaviour, int subject, float amount) {
    thread_local std::vector<std::uintptr_t> nodes;
    thread_local std::vector<float> slots;  // this segment's: the session's keys, unwrapped from its own
    thread_local std::vector<double> times;
    thread_local Work work;
    const auto lanes = framing_lanes(behaviour);
    if (lanes.empty()) return SegmentResult::stock;

    // Every key as a slot, the way the game builds the segment's neighbours: under this segment's
    // Behaviour and Subject (once per rebuild, see KeySlots), then each key's angles unwrapped against
    // the key before it (going on from the segment's first key) or after it (going back).
    const KeySlots* built{};
    if (rebuilding)
        for (const auto& kept : rebuild_slots)
            if (kept.keys_end == keys_end && kept.behaviour == behaviour && kept.subject == subject) built = &kept;
    if (!built) {
        nodes.clear();
        for (auto node = at(keys_end + sizeof(std::uintptr_t)); node != keys_end; node = successor(node)) {
            if (!node || nodes.size() == most_keys) return SegmentResult::stock;
            nodes.push_back(node);
        }
        KeySlots made{keys_end, behaviour, subject, nodes, std::vector<float>(nodes.size() * native::slot_floats, 0.0f)};
        for (std::size_t i = 0; i < nodes.size(); ++i)
            slot_build(manager, reinterpret_cast<const void*>(nodes[i] + native::framing_key_value),
                       *reinterpret_cast<const double*>(nodes[i] + native::framing_key_time), behaviour, subject,
                       made.slots.data() + i * native::slot_floats);
        if (rebuilding) {
            rebuild_slots.push_back(std::move(made));
            built = &rebuild_slots.back();
        } else {
            thread_local KeySlots alone;
            alone = std::move(made);
            built = &alone;
        }
    }
    const auto found = std::find(built->nodes.begin(), built->nodes.end(), key);
    if (found == built->nodes.end()) return SegmentResult::mismatch;
    const auto from = static_cast<std::size_t>(found - built->nodes.begin());
    const auto n = built->nodes.size();
    slots = built->slots;
    const auto slot = [](std::size_t index) { return slots.data() + index * native::slot_floats; };
    for (std::size_t i = from + 1; i < n; ++i) slot_unwrap(slot(i), slot(i - 1), behaviour);
    for (std::size_t i = from; i-- > 0;) slot_unwrap(slot(i), slot(i + 1), behaviour);
    times.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        std::memcpy(&times[i], reinterpret_cast<const unsigned char*>(slot(i)) + native::slot_time, sizeof(double));
    const FramingKeys keys{times, slots, from};
    return smooth_segment(segment, keys, lanes, amount, work);
}
void* segment_build(void* manager, void* segment, void* keys_end, void** key) {
    void* const result = build_original.load(std::memory_order_relaxed)(manager, segment, keys_end, key);
    if (!segment || !keys_end || !key || !*key) return result;
    const int behaviour = segment_int(segment, native::segment_behaviour), subject = segment_int(segment, native::segment_subject);
    counters.built[behaviour_index(behaviour)][subject_index(subject)].fetch_add(1, std::memory_order_relaxed);
    const auto end = reinterpret_cast<std::uintptr_t>(keys_end);
    std::int32_t session{};
    std::memcpy(&session, reinterpret_cast<const void*>(end - native::session_id_before_keys), sizeof(session));
    last_session.store(1ull << 32 | static_cast<std::uint32_t>(session), std::memory_order_relaxed);
    if (!enabled.load(std::memory_order_relaxed)) return result;
    const float amount = smooth.load(std::memory_order_relaxed);
    if (!(amount > 0)) return result;
    if (behaviour != native::free_camera_behaviour) {
        counters.filtered.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    auto outcome = SegmentResult::stock;
    try {
        outcome = smooth_framing(manager, static_cast<unsigned char*>(segment), end, reinterpret_cast<std::uintptr_t>(*key),
                                 behaviour, subject, amount);
    } catch (...) {}
    auto& counter = outcome == SegmentResult::smoothed ? counters.smoothed
                  : outcome == SegmentResult::mismatch ? counters.mismatch : counters.stock;
    counter.fetch_add(1, std::memory_order_relaxed);
    return result;
}
// The game's rebuild of a session, which builds all of its segments: the key slots are kept for
// the length of it (KeySlots).
void session_rebuild(void* manager, int session) {
    struct Rebuilding {
        Rebuilding() { if (rebuilding++ == 0) rebuild_slots.clear(); }
        ~Rebuilding() { if (--rebuilding == 0) rebuild_slots.clear(); }
    } scope;
    rebuild_original.load(std::memory_order_relaxed)(manager, session);
}

// Whether the manager still has the session (its map: anchor at manager_sessions, root two pointers
// in, the int id at node + 0x20, ordered by it).
bool has_session(std::uintptr_t manager, std::int32_t id) noexcept {
    std::uintptr_t node{};
    if (!memory::peek(manager + native::manager_sessions + 2 * sizeof(std::uintptr_t), node)) return false;
    for (int depth = 0; node && depth < 64; ++depth) {
        std::int32_t key{};
        if (!memory::peek(node + 0x20, key)) return false;
        if (key == id) return true;
        if (!memory::peek(node + (id < key ? sizeof(std::uintptr_t) : 0), node)) return false;
    }
    return false;
}
// The game's rebuild of the session the camera plays, so that its framing segments are built with
// the new setting. Game thread.
std::string rebuild_framing() {
    if (!framing_hooked.load()) return "";
    const auto seen = last_session.load(std::memory_order_relaxed);
    if (!seen) return " Framing keys: none built yet; they follow when the game builds them.";
    const auto id = static_cast<std::int32_t>(static_cast<std::uint32_t>(seen));
    std::uintptr_t manager{};
    if (!memory::peek(image + native::manager, manager) || !manager || !has_session(manager, id))
        return " Framing keys: the replay they were built for is gone; they follow at the next key edit.";
    rebuild(reinterpret_cast<void*>(manager), id);
    return std::format(" Framing keys of session {} rebuilt.", id);
}
std::string number(float value) { return std::format("{:g}", value); }
std::string behaviour_name(std::size_t index) { return index < behaviours - 1 ? std::to_string(index) : "other"; }
std::string subject_name(std::size_t index) { return index < subjects - 1 ? std::to_string(index) : "other"; }

std::filesystem::path settings_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"KeySmooth.settings";
    if (raw) CoTaskMemFree(raw);
    return result;
}
void load_settings() {
    try {
        std::ifstream in(settings_path());
        const auto loaded = from_text(std::string(std::istreambuf_iterator<char>(in), {}));
        enabled.store(loaded.on, std::memory_order_relaxed);
        smooth.store(loaded.amount, std::memory_order_relaxed);
    } catch (...) {}
}
// Written on a thread of its own, as Board Break does, so that a slow disk never holds up the frame;
// the writes are kept in order by their own lock.
void save_settings() {
    try {
        const Settings now{enabled.load(), smooth.load()};
        std::thread([text = to_text(now)] {
            static std::mutex writing;
            std::lock_guard lock(writing);
            try {
                const auto path = settings_path();
                if (path.empty()) return;
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                std::ofstream file(path, std::ios::trunc);
                file << text;
            } catch (...) {}
        }).detach();
    } catch (...) {}
}
// A changed setting: saved, and the keys in play rebuilt with it.
std::string changed(std::string said) {
    save_settings();
    return said + rebuild_framing();
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    if (started.exchange(true)) return available();
    try {
        if (!base) return false;
        image = base;
        load_settings();
        if (matches(base, native::segment_build) && matches(base, native::slot_build) &&
            matches(base, native::slot_unwrap) && matches(base, native::session_rebuild)) {
            slot_build = reinterpret_cast<SlotBuild>(base + native::slot_build.rva);
            slot_unwrap = reinterpret_cast<SlotUnwrap>(base + native::slot_unwrap.rva);
            // (Called through its entry, so a setting's rebuild goes through the hook as the game's do.)
            rebuild = reinterpret_cast<Rebuild>(base + native::session_rebuild.rva);
            // The rebuild first: without it each segment builds its own key slots, as before.
            hook(base, native::session_rebuild, &session_rebuild, rebuild_original);
            framing_hooked = hook(base, native::segment_build, &segment_build, build_original);
        }
        logging::log(available() ? logging::Level::info : logging::Level::warning, logging::Channel::runtime,
            "Replay keyframe smoothing: framing track {}, key slots {}; smoothing {}.",
            framing_hooked ? "hooked" : "not hooked (game code differs)",
            rebuild_original.load() ? "built once per rebuild" : "built per segment", on() ? "on" : "off");
        return available();
    } catch (...) {
        return false;
    }
}

bool available() noexcept { return framing_hooked.load(); }
bool on() noexcept { return enabled.load(std::memory_order_relaxed); }
float smoothness() noexcept { return smooth.load(std::memory_order_relaxed); }

std::string set_on(bool value) {
    enabled.store(value, std::memory_order_relaxed);
    return changed(value ? "Keyframe smoothing on (smoothness " + number(smoothness()) + ")."
                         : std::string("Keyframe smoothing off: the game's own curve."));
}
std::string set_smoothness(float value) {
    smooth.store(std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f, std::memory_order_relaxed);
    return changed("Keyframe smoothness " + number(smoothness()) + (on() ? "." : " (smoothing is off)."));
}

std::string status() {
    const auto take = [](std::atomic<std::uint64_t>& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    std::uint64_t built{};
    std::string built_by;
    const auto add = [](std::string& list, const std::string& name, std::uint64_t count) {
        if (count) list += std::format("{}{}: {}", list.empty() ? "" : ", ", name, count);
    };
    for (std::size_t b = 0; b < behaviours; ++b) {
        for (std::size_t s = 0; s < subjects; ++s) {
            const auto built_here = take(counters.built[b][s]);
            built += built_here;
            add(built_by, behaviour_name(b) + "/" + subject_name(s), built_here);
        }
    }
    const auto seen = last_session.load(std::memory_order_relaxed);
    return std::format(
        "Keyframe smoothing {}: smoothness {}, free camera (Behaviour {}) only. Since the last status: framing "
        "segments built {} (by Behaviour/Subject: {}; smoothed {}, left as the game built them {}, other Behaviour {}, "
        "not matching {}){}.",
        on() ? "on" : "off", number(smoothness()), native::free_camera_behaviour,
        built, built_by.empty() ? "none" : built_by, take(counters.smoothed),
        take(counters.stock), take(counters.filtered), take(counters.mismatch),
        !framing_hooked.load() ? " (not hooked)"
        : seen ? std::format("; last built for session {}", static_cast<std::int32_t>(static_cast<std::uint32_t>(seen)))
               : std::string{});
}
} // namespace dingosdk::replay_keyframes
