#pragma once
#include "anim_runtime_rules.h"
#include "anim_core.h"
#include <cstdint>
#include <memory>

namespace dingosdk::anim_studio {
// These are observed physics states, not decoded native animation-graph phases.
enum class MotionState { unknown, riding, air, rail, offboard };
// Supported native physics selector values, shared by runtime and replay tests.
// 103 is the crouch/pop preparation observed between 100 and 200. It remains
// grounded; treating it as unavailable discards the entry before every normal pop.
inline MotionState motion_state(std::optional<std::uint32_t> state) noexcept {
    if (!state) return MotionState::unknown;
    if (*state == 300 || *state >= 500) return MotionState::offboard;
    if (*state >= 400 && *state < 500) return MotionState::rail;
    if (*state == 200 || *state == 201) return MotionState::air;
    if (*state == 100 || *state == 101 || *state == 103) return MotionState::riding;
    return MotionState::unknown;
}
enum class EntryKind { unknown, ground, rail };
enum class PlaybackPhase { idle, air, landing, rail_release };
inline const char* entry_name(EntryKind value) noexcept {
    return value == EntryKind::ground ? "ground" : value == EntryKind::rail ? "rail" : "unknown";
}
inline const char* phase_name(PlaybackPhase value) noexcept {
    return value == PlaybackPhase::air ? "air" : value == PlaybackPhase::landing ? "landing" :
           value == PlaybackPhase::rail_release ? "rail_release" : "idle";
}
struct PlaybackSample { float time{-1}, weight{}; bool finished{}; };
struct PlaybackEpisode {
    MotionState previous{MotionState::unknown};
    PlaybackPhase phase{PlaybackPhase::idle};
    EntryKind entry{EntryKind::unknown};
    std::uint64_t id{};
    double takeoff{}, landing{-1}, release_at{}, selected_at{-1}, last_observed{-1};
    float release_weight{}, last_weight{};
    ContactWindow contact;
    GameplayClipClock clock;
    void reset() noexcept { const auto next_id = id; *this = {}; id = next_id; }
    // True only when a new airborne episode begins. A rail exit cannot inherit the entry trick.
    bool observe(double now, MotionState motion) noexcept {
        if (!std::isfinite(now) || (last_observed >= 0 && now < last_observed)) { reset(); return false; }
        last_observed = now;
        if (motion == MotionState::unknown || motion == MotionState::offboard) {
            reset(); previous = motion; return false;
        }
        bool started = false;
        if (motion == MotionState::air && phase != PlaybackPhase::air) {
            const auto from = previous;
            ++id; phase = PlaybackPhase::air; takeoff = now; landing = -1; selected_at = -1;
            entry = from == MotionState::rail ? EntryKind::rail : from == MotionState::riding ? EntryKind::ground : EntryKind::unknown;
            clock.reset(); contact.since.reset(); last_weight = 0; started = true;
        } else if (motion == MotionState::rail && (phase == PlaybackPhase::air || phase == PlaybackPhase::landing)) {
            phase = selected_at >= 0 && clock.time >= 0 ? PlaybackPhase::rail_release : PlaybackPhase::idle;
            release_at = now; release_weight = last_weight; contact.since.reset();
        } else if (motion == MotionState::riding && phase == PlaybackPhase::air && contact.observe(now, true)) {
            phase = selected_at >= 0 ? PlaybackPhase::landing : PlaybackPhase::idle; landing = *contact.since;
        }
        if (motion == MotionState::air) contact.since.reset();
        previous = motion;
        return started;
    }
    bool selection_open(double now) const noexcept { return phase == PlaybackPhase::air && now >= takeoff && now - takeoff < 0.35; }
    void select(double now) noexcept { if (std::isfinite(now)) { selected_at = now; clock.reset(); } }
    PlaybackSample sample(double now, float up, float down, float duration, float speed, float predicted_air) noexcept {
        PlaybackSample out;
        if (selected_at < 0 || up < 0 || duration <= 0 || up > duration || down > duration ||
            !std::isfinite(now) || !std::isfinite(up) || !std::isfinite(down) ||
            !std::isfinite(duration) || !std::isfinite(speed) || speed <= 0 || !std::isfinite(predicted_air)) return out;
        if (phase == PlaybackPhase::air) {
            float rate = speed;
            if (predicted_air > 0 && down > up) rate *= (down - up) / predicted_air;
            out.time = clock.advance_air(now, up, rate, down > up ? down : duration);
            out.weight = std::clamp(static_cast<float>(now - selected_at) / 0.08f, 0.0f, 1.0f);
        } else if (phase == PlaybackPhase::landing && down >= 0) {
            const auto after = static_cast<float>(now - landing) * speed;
            const auto span = std::max(0.05f, std::min(0.6f, duration - down));
            const auto u = std::clamp(after / span, 0.0f, 1.0f);
            out.time = clock.land(now, speed, duration, down + after);
            out.weight = 1 - u*u*(3 - 2*u);
        } else if (phase == PlaybackPhase::rail_release) {
            // Entry is a release to the native constrained rail pose, never a fake airborne hold.
            const auto u = std::clamp(static_cast<float>(now - release_at) / 0.08f, 0.0f, 1.0f);
            out.time = clock.time; out.weight = release_weight * (1 - u*u*(3 - 2*u));
        }
        last_weight = out.weight;
        if (phase != PlaybackPhase::air && out.weight <= 0) { phase = PlaybackPhase::idle; out.finished = true; }
        return out;
    }
};
// Body/board share one edit schedule per presentation cohort. This token is not a native
// evaluation ID; native poses are read fresh at each publication and never replaced wholesale.
template<class Packet> struct PresentationLatch {
    std::shared_ptr<const Packet> pinned;
    std::uint64_t presentation{~std::uint64_t{0}}, epoch{~std::uint64_t{0}};
    std::shared_ptr<const Packet> get(std::uint64_t frame, std::uint64_t version, std::shared_ptr<const Packet> latest) {
        if (frame != presentation || version != epoch) { presentation = frame; epoch = version; pinned = std::move(latest); }
        return pinned;
    }
};
struct EditPublicationCache {
    std::uint64_t generation{~std::uint64_t{0}}, animation_update{};
    std::uintptr_t buffer{};
    std::vector<Joint> before, after;
    // Multiple publications of one native animation update must not accumulate our own edits.
    // A changed native value is always preserved; only an exact previous write is unwrapped.
    void restore(std::uint64_t gen, std::uint64_t update, std::uintptr_t address, std::span<Joint> pose) const {
        if (gen != generation || update != animation_update || address != buffer || pose.size() != after.size() || pose.size() != before.size()) return;
        for (std::size_t i = 0; i < pose.size(); ++i) {
            if (before[i].rotation != after[i].rotation && pose[i].rotation == after[i].rotation) pose[i].rotation = before[i].rotation;
            for (std::size_t k = 0; k < 3; ++k)
                if (before[i].position[k] != after[i].position[k] && pose[i].position[k] == after[i].position[k])
                    pose[i].position[k] = before[i].position[k];
        }
    }
    void commit(std::uint64_t gen, std::uint64_t update, std::uintptr_t address,
                std::vector<Joint> native, std::vector<Joint> edited) {
        generation = gen; animation_update = update; buffer = address;
        before = std::move(native); after = std::move(edited);
    }
};
// Smooth only edit deltas when selection changes. Absolute skeleton/board coordinates
// are never retained: the same delta can be composed over a moving fresh native pose.
struct EditTransition {
    std::uint64_t generation{~std::uint64_t{0}}, episode{};
    std::uintptr_t source{};
    double started{-1}, observed{-1};
    std::vector<Offset> from, current;
    void reset() { *this = {}; }
    std::span<const Offset> evaluate(double now, std::uint64_t gen, std::uint64_t attempt,
                                     std::uintptr_t selected, std::span<const Offset> desired) {
        if (!std::isfinite(now)) { reset(); return current; }
        if (generation != gen || current.size() != desired.size() || now < observed) {
            generation = gen; current.assign(desired.size(), Offset{}); started = -1;
        }
        if (started < 0 || episode != attempt || source != selected) {
            from = current; started = now; episode = attempt; source = selected;
        }
        observed = now;
        float u = std::clamp(static_cast<float>(now - started) / 0.08f, 0.0f, 1.0f);
        u = u*u*(3 - 2*u);
        for (std::size_t i = 0; i < desired.size(); ++i) {
            current[i].rotation = u >= 1 ? desired[i].rotation : slerp(from[i].rotation, desired[i].rotation, u);
            for (std::size_t k = 0; k < 3; ++k)
                current[i].offset[k] = u >= 1 ? desired[i].offset[k] : from[i].offset[k] + (desired[i].offset[k] - from[i].offset[k]) * u;
        }
        return current;
    }
};
} // namespace dingosdk::anim_studio
