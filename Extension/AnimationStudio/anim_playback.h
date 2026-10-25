#pragma once
#include "anim_runtime_rules.h"
#include "anim_core.h"
#include <cstdint>
#include <limits>
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
// A trick fitted to the game's air, never slower than it was recorded. Seconds into the recording's air for
// `in_air` seconds into the game's, when the game's air is expected to last `target` seconds (the recorded one
// lasted `recorded_air`). A shorter expected air plays it a little faster (at most 1.3 times: the expectation
// is often wrong both ways). A longer one is not slowed down to fit (a tester's varial kickflip: 0.71 s
// expected, 0.46 s real, and a trick slowed for it was still in the air when the board landed). It never
// reaches its landing while the game is still in the air, and it never stands still waiting for it either
// (a low-pop shuv-it on a bigger pop stood frozen for six frames): over its last `ease` seconds it slows,
// feet reaching for the ground, closer and closer to the landing however long the air runs.
inline float fitted_air_time(float in_air, float recorded_air, float target) noexcept {
    if (!std::isfinite(in_air)) return 0;
    in_air = std::max(0.0f, in_air);
    if (!(recorded_air > 1.0f / 30) || !std::isfinite(recorded_air)) return in_air;
    float t = in_air;
    if (target > 0.05f && std::isfinite(target) && target < recorded_air) t = in_air * std::min(recorded_air / target, 1.3f);
    const float land = recorded_air - 1.0f / 60, ease = std::min(0.06f, land * 0.3f), knee = land - ease;
    if (t > knee) t = knee + ease * (1 - std::exp(-(t - knee) / ease));
    return t;
}
// After the game's touchdown, `since` seconds ago: the recording from its own landing (`down`) on, at its own
// pace, so the landing bend happens on the ground. It was at `from` at touchdown; if that was short of its
// landing it catches up rather than jumping, over a time that grows with how far behind it was (80 to 200 ms),
// so it never runs at more than about 2.25 times its pace.
inline float landed_time(float since, float down, float from) noexcept {
    since = std::isfinite(since) ? std::max(0.0f, since) : 0.0f;
    const float behind = std::isfinite(from) ? std::max(0.0f, down - from) : 0.0f;
    const float window = std::clamp(behind * 0.8f, 0.08f, 0.2f);
    const float u = std::clamp(since / window, 0.0f, 1.0f);
    return down + since - behind * (1 - u);
}
struct PlaybackEpisode {
    MotionState previous{MotionState::unknown};
    PlaybackPhase phase{PlaybackPhase::idle};
    EntryKind entry{EntryKind::unknown};
    std::uint64_t id{};
    double takeoff{}, landing{-1}, release_at{}, selected_at{-1}, last_observed{-1};
    float release_weight{}, last_weight{};
    float last_time{-1};   // the recording's time last sampled
    float landed_from{-1}; // where the recording was when the game touched down (-1 while in the air)
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
            ++id; phase = PlaybackPhase::air; takeoff = now; landing = -1; selected_at = -1; landed_from = -1; last_time = -1;
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
    // `fit_air`: the trick is fitted to the air actually happening (fitted_air_time, landed_time) instead of
    // running at the seconds it was recorded at. The studio always plays with it on.
    PlaybackSample sample(double now, float up, float down, float duration, float speed, float predicted_air,
                          bool source_timing = false, bool fit_air = false) noexcept {
        PlaybackSample out;
        if (selected_at < 0 || up < 0 || duration <= 0 || up > duration || down > duration ||
            !std::isfinite(now) || !std::isfinite(up) || !std::isfinite(down) ||
            !std::isfinite(duration) || !std::isfinite(speed) || speed <= 0 || !std::isfinite(predicted_air)) return out;
        if (source_timing && (phase == PlaybackPhase::air || phase == PlaybackPhase::landing)) {
            // Recognition can arrive late or refine ollie into a flip. Anchor the
            // source clock at takeoff, never at selection, and keep source seconds
            // through reception instead of stretching air and discarding late keys.
            // Speed is a studio preview control. Slowing only this additive
            // layer while the native trick runs at 1x desynchronizes limbs and
            // board; source-bound gameplay keys always follow source seconds.
            // Fitted to the air (fitted_air_time, landed_time): takeoff on the game's takeoff, the flip at its
            // own pace, the recording's landing on the game's touchdown.
            float time = up + static_cast<float>(std::max(0.0, now - takeoff));
            if (fit_air && down > up) {
                if (phase == PlaybackPhase::air && contact.since) {
                    // The board is down and the game is making sure (ContactWindow): the landing starts at
                    // the touch, not frames later with a jump - the pose in the air with the board on the
                    // ground was it going into the ground.
                    if (landed_from < 0) landed_from = std::clamp(last_time >= 0 ? last_time : down, up, down);
                    time = landed_time(static_cast<float>(now - *contact.since), down, landed_from);
                } else if (phase == PlaybackPhase::air) {
                    time = up + fitted_air_time(static_cast<float>(now - takeoff), down - up, predicted_air);
                    if (last_time >= 0) time = std::max(time, last_time); // a touch that was a bounce never turns it back
                    landed_from = -1;
                } else {
                    if (landed_from < 0) landed_from = std::clamp(last_time >= 0 ? last_time : down, up, down);
                    time = landed_time(static_cast<float>(now - landing), down, landed_from);
                }
            }
            out.time = std::min(duration, time);
            clock.observe_air(now, out.time, 1);
            const auto enter = std::clamp(static_cast<float>(now - selected_at) / 0.08f, 0.0f, 1.0f);
            const auto leave = std::clamp((duration - out.time) / 0.08f, 0.0f, 1.0f);
            out.weight = enter * leave;
            out.finished = out.time >= duration;
        } else if (phase == PlaybackPhase::air) {
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
        if (out.time >= 0) last_time = out.time;
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
                                     std::uintptr_t selected, std::span<const Offset> desired, float seconds = 0.15f) {
        if (!std::isfinite(now)) { reset(); return current; }
        if (generation != gen || current.size() != desired.size() || now < observed) {
            generation = gen; current.assign(desired.size(), Offset{}); started = -1;
        }
        if (started < 0 || episode != attempt || source != selected) {
            from = current; started = now; episode = attempt; source = selected;
        }
        observed = now;
        // `seconds` (0.15 unless the Crossfade setting says): one trick's keys into the next, or out when a trick is
        // cut short (a new air, a bail), without a snap.
        float u = std::clamp(static_cast<float>(now - started) / std::max(seconds, 0.02f), 0.0f, 1.0f);
        u = u*u*(3 - 2*u);
        for (std::size_t i = 0; i < desired.size(); ++i) {
            current[i].rotation = u >= 1 ? desired[i].rotation : slerp(from[i].rotation, desired[i].rotation, u);
            for (std::size_t k = 0; k < 3; ++k)
                current[i].offset[k] = u >= 1 ? desired[i].offset[k] : from[i].offset[k] + (desired[i].offset[k] - from[i].offset[k]) * u;
        }
        return current;
    }
};
// ---- the ground -------------------------------------------------------------------------------------------------------
// The board falls at 20 m/s2 in the game, not the Earth's 9.81: measured 2026-10-25 from the board body's speed and height
// through flat ollies (the speed falls 6 m/s in 0.28 s; from the top it drops 0.58 m in 0.23 s). With 9.81 the time to the
// ground came out twice as long while the board rose, so every trick with a catch was taken for one off a drop on flat
// ground too: its own clock, and the keyed joints and the feet as made in the studio rather than on the game's pose.
inline constexpr float board_gravity = 20.0f;
// Seconds until the board is down: from how fast it goes up now (metres a second, falling below 0) and how high its lowest
// point is over the ground. Below 0: not known.
inline float time_to_ground(float up_speed, float height) noexcept {
    if (!std::isfinite(up_speed) || !std::isfinite(height)) return -1;
    const float h = std::max(0.0f, height);
    return (up_speed + std::sqrt(up_speed * up_speed + 2 * board_gravity * h)) / board_gravity;
}
// The lowest a foot's ankle is drawn: its height over the deck's mount, along the deck's up (on the deck the ankle stands
// 10.4 cm over it; the ground is 8.7 cm under it). `over`: within the deck's outline. Below -infinity: no limit.
// In the air, the deck upright under him: an ankle over the deck less than 9.5 cm over it is lifted to there, unless it is more
// than 8 cm into it (that is a foot past the board, not on it). Rolling on the ground: over the deck, never into it however
// deep the keys put it; off it, never under the ground (a leg keyed out over the edge took its foot into the ground, and a
// hips key 20 cm down took both feet through the deck, while it rolled away from a landing).
inline constexpr float ankle_on_deck = 0.095f, ankle_on_ground = 0.015f;
inline float lowest_ankle(bool rolling, bool over, float height) noexcept {
    if (rolling) return over ? ankle_on_deck : ankle_on_ground;
    return over && height >= -0.08f ? ankle_on_deck : -std::numeric_limits<float>::infinity();
}
// ---- off a drop: the catch slowed, never held still ------------------------------------------------------------------
// Off a drop, down stairs or onto a ledge the air is longer or shorter than the trick's own. From just before its catch the
// trick's time is its own (the game's pose is off the clip by then): it runs slower - or faster - so its landing comes on
// the game's touchdown, easing into the new pace and out of it again, never standing still and never jumping. `hold` (the
// trick's CATCH HOLD) says where the extra time goes: 1 around the catch (the knees up long, the legs reaching for the
// ground late), 0 spread evenly from the catch to the landing. (It held the catch frozen until the ground came: on a big
// set the skater stood still in the air for two seconds.)
struct AirStretch {
    static constexpr float slowest = 0.05f, fastest = 2.2f, ease = 0.1f; // trick seconds a second; seconds to change pace
    float time{-1}; // the trick's time while it is its own (-1: still the game's clock)
    float rate{1};
    bool on() const noexcept { return time >= 0; }
    void reset() noexcept { *this = {}; }
    // Its slow part: from a little before the catch to a share of the way on to the landing; after it, its own pace.
    static float slow_from(float takeoff, float caught) noexcept { return caught - std::min(0.1f, 0.4f * (caught - takeoff)); }
    static float slow_until(float caught, float land, float hold) noexcept {
        return std::min(land, caught + std::max(1.0f / 30, (land - caught) * (1 - std::clamp(hold, 0.0f, 1.0f))));
    }
    // One update. `clock`: the trick's time by the game's clock; `to_ground`: seconds until the board is down (below 0: not
    // known); `step`: seconds since the last update. Returns the trick's time.
    float update(float clock, float to_ground, float step, float takeoff, float caught, float land, float hold) noexcept {
        if (!(caught > takeoff) || takeoff < 0 || !(land > caught) || !std::isfinite(clock) || clock < 0) return on() ? time : clock;
        const bool known = std::isfinite(to_ground) && to_ground >= 0;
        if (!on()) {
            if (!known || clock < slow_from(takeoff, caught)) return clock;
            const float left = land - clock; // its own air still to come
            if (!(to_ground > left + 0.06f) && !(to_ground < left - 0.1f)) return clock; // in step with the game: its clock
            time = std::min(clock, land - 1.0f / 60);
            rate = 1;
        }
        if (!(step > 0) || !std::isfinite(step)) return time;
        float want = rate; // the ground not known for a moment: it keeps its pace
        if (known) {
            // Aimed at its last moment before the landing, on the touchdown (which brings the landing itself).
            const float end = land - 1.0f / 60, until = std::min(end, slow_until(caught, land, hold));
            const float tail = end - std::max(time, until); // after the slow part, at its own pace to the landing
            // The slow part ends with time to come back up to pace before the tail (none needed when there is no tail).
            const float ramp = tail > 1.0f / 60 ? 1.5f * ease : 0.0f;
            want = time < until ? (until - time) / std::max(to_ground - tail - ramp, 0.02f) : (end - time) / std::max(to_ground, 0.02f);
        }
        want = std::clamp(want, slowest, fastest);
        rate += (want - rate) * (1 - std::exp(-step / ease));
        time = std::min(time + rate * step, land - 1.0f / 60);
        return time;
    }
};
// The studio's DROP PREVIEW: the trick played as off a drop `metres` lower than its own landing, on the same AirStretch the
// game's touchdown drives in game, so its CATCH and CATCH HOLD can be set by eye.
struct DropPlay {
    float metres{}, takeoff{-1}, caught{-1}, land{-1}, duration{}, hold{default_catch_hold};
    bool on() const noexcept { return metres > 0.01f && takeoff >= 0 && caught > takeoff && land > caught && duration >= land; }
};
// The air off a drop `metres` lower: the clip's own takeoff (the speed that gives it `air`) falling that much further.
inline float drop_air(float air, float metres) noexcept {
    if (!(air > 0) || !(metres > 0) || !std::isfinite(air) || !std::isfinite(metres)) return air;
    const float g = board_gravity, up = g * air / 2;
    return (up + std::sqrt(up * up + 2 * g * metres)) / g;
}
// `real` seconds from the clip's start, off that drop -> the clip's time: its own up to its catch, slowed through it, its
// landing on the touchdown and its own pace from there.
inline float drop_clip_time(const DropPlay& p, float real) noexcept {
    if (!p.on() || !std::isfinite(real) || real <= p.takeoff) return real;
    const float air = drop_air(p.land - p.takeoff, p.metres), in_air = real - p.takeoff, until = std::min(in_air, air);
    AirStretch stretch;
    float time = p.takeoff;
    for (float t = 0; t < until - 1e-6f;) {
        const float step = std::min(1.0f / 60, until - t);
        t += step;
        time = stretch.update(p.takeoff + t, air - t, step, p.takeoff, p.caught, p.land, p.hold);
    }
    if (!stretch.on()) time = std::min(time, p.land - 1.0f / 60);
    if (in_air <= air) return time;
    return std::min(p.duration, landed_time(in_air - air, p.land, time));
}
// The moment off the drop that shows `clip_time` (the first: it never goes back).
inline float drop_real_time(const DropPlay& p, float clip_time) noexcept {
    if (!p.on() || !std::isfinite(clip_time) || clip_time <= p.takeoff) return clip_time;
    float low = p.takeoff, high = p.duration + drop_air(p.land - p.takeoff, p.metres) + 1;
    for (int i = 0; i < 40; ++i) {
        const float mid = (low + high) / 2;
        (drop_clip_time(p, mid) >= clip_time ? high : low) = mid;
    }
    return high;
}
} // namespace dingosdk::anim_studio
