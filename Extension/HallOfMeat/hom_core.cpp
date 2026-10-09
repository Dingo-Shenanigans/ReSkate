#include "hom_core.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dingosdk::hall_of_meat {
const std::array<BodyPart, body_parts> &body_parts_table() {
    // Copied from Skate 3's skatercollections database (the HoM scoring class's layout data): per
    // part, its four links {part, share} and its six damage-level slots {points, impact}. Parts:
    // 0 skull, 1 head joint, 2-5 and 6-9 the arms (hand, forearm, upper arm, shoulder), 10 chest,
    // 11-13 the spine (ribs, middle, lower), 14-17 and 18-21 the legs (toes, foot, calf, thigh),
    // 22 pelvis, 23 neck, 24 groin.
    static const std::array<BodyPart, body_parts> table{{
    {{{-1, 0.0f}, {-1, 0.0f}, {23, 0.0f}, {1, 0.5f}}, {{500, 7.0f}, {1000, 14.0f}, {0, 0.0f}, {0, 0.0f}, {5000, 21.0f}, {0, 0.0f}}}, // 0
    {{{-1, 0.0f}, {-1, 0.0f}, {0, 0.125f}, {10, 0.5f}}, {{0, 0.0f}, {0, 0.0f}, {0, 0.0f}, {0, 0.0f}, {0, 0.0f}, {0, 0.0f}}}, // 1
    {{{-1, 0.0f}, {-1, 0.0f}, {3, 0.25f}, {-1, 0.0f}}, {{100, 6.0f}, {250, 9.0f}, {500, 12.0f}, {750, 15.0f}, {0, 0.0f}, {1000, 18.0f}}}, // 2
    {{{-1, 0.0f}, {-1, 0.0f}, {4, 0.25f}, {2, 0.125f}}, {{100, 9.0f}, {250, 12.0f}, {500, 15.0f}, {0, 0.0f}, {1000, 21.0f}, {0, 0.0f}}}, // 3
    {{{-1, 0.0f}, {5, 0.25f}, {-1, 0.0f}, {3, 0.125f}}, {{100, 9.0f}, {250, 12.0f}, {500, 15.0f}, {750, 18.0f}, {1000, 21.0f}, {0, 0.0f}}}, // 4
    {{{4, 0.125f}, {10, 0.25f}, {-1, 0.0f}, {-1, 0.0f}}, {{100, 9.0f}, {250, 12.0f}, {500, 15.0f}, {750, 18.0f}, {1000, 21.0f}, {0, 0.0f}}}, // 5
    {{{-1, 0.0f}, {-1, 0.0f}, {7, 0.25f}, {-1, 0.0f}}, {{100, 6.0f}, {250, 9.0f}, {500, 12.0f}, {750, 15.0f}, {0, 0.0f}, {1000, 18.0f}}}, // 6
    {{{-1, 0.0f}, {-1, 0.0f}, {8, 0.25f}, {6, 0.125f}}, {{100, 9.0f}, {250, 12.0f}, {500, 15.0f}, {0, 0.0f}, {1000, 21.0f}, {0, 0.0f}}}, // 7
    {{{9, 0.25f}, {-1, 0.0f}, {-1, 0.0f}, {7, 0.125f}}, {{100, 9.0f}, {250, 12.0f}, {500, 15.0f}, {750, 18.0f}, {1000, 21.0f}, {0, 0.0f}}}, // 8
    {{{10, 0.25f}, {8, 0.125f}, {-1, 0.0f}, {-1, 0.0f}}, {{100, 9.0f}, {250, 12.0f}, {500, 15.0f}, {750, 18.0f}, {1000, 21.0f}, {0, 0.0f}}}, // 9
    {{{5, 0.25f}, {9, 0.25f}, {1, 0.125f}, {11, 0.5f}}, {{0, 0.0f}, {0, 0.0f}, {0, 0.0f}, {0, 0.0f}, {0, 0.0f}, {0, 0.0f}}}, // 10
    {{{-1, 0.0f}, {-1, 0.0f}, {10, 0.0f}, {12, 0.25f}}, {{250, 7.0f}, {500, 10.0f}, {0, 0.0f}, {0, 0.0f}, {1000, 15.0f}, {2000, 25.0f}}}, // 11
    {{{-1, 0.0f}, {-1, 0.0f}, {11, 0.25f}, {13, 0.25f}}, {{250, 7.0f}, {500, 10.0f}, {0, 0.0f}, {0, 0.0f}, {1000, 15.0f}, {2000, 25.0f}}}, // 12
    {{{-1, 0.0f}, {-1, 0.0f}, {12, 0.25f}, {22, 0.25f}}, {{250, 7.0f}, {500, 10.0f}, {0, 0.0f}, {0, 0.0f}, {1000, 15.0f}, {2000, 25.0f}}}, // 13
    {{{-1, 0.0f}, {15, 0.25f}, {-1, 0.0f}, {-1, 0.0f}}, {{100, 7.0f}, {250, 10.0f}, {0, 0.0f}, {500, 13.0f}, {750, 16.0f}, {1000, 19.0f}}}, // 14
    {{{14, 0.125f}, {-1, 0.0f}, {16, 0.25f}, {-1, 0.0f}}, {{100, 7.0f}, {250, 10.0f}, {0, 0.0f}, {500, 16.0f}, {750, 19.0f}, {1000, 22.0f}}}, // 15
    {{{-1, 0.0f}, {-1, 0.0f}, {17, 0.25f}, {15, 0.125f}}, {{100, 10.0f}, {250, 13.0f}, {500, 16.0f}, {750, 19.0f}, {1000, 22.0f}, {0, 0.0f}}}, // 16
    {{{-1, 0.0f}, {22, 0.25f}, {-1, 0.0f}, {16, 0.125f}}, {{100, 10.0f}, {250, 13.0f}, {500, 16.0f}, {750, 19.0f}, {1000, 22.0f}, {0, 0.0f}}}, // 17
    {{{19, 0.25f}, {-1, 0.0f}, {-1, 0.0f}, {-1, 0.0f}}, {{100, 7.0f}, {250, 10.0f}, {0, 0.0f}, {500, 13.0f}, {750, 16.0f}, {1000, 19.0f}}}, // 18
    {{{-1, 0.0f}, {18, 0.125f}, {20, 0.25f}, {-1, 0.0f}}, {{100, 7.0f}, {250, 10.0f}, {500, 13.0f}, {750, 16.0f}, {0, 0.0f}, {1000, 22.0f}}}, // 19
    {{{-1, 0.0f}, {-1, 0.0f}, {21, 0.25f}, {19, 0.125f}}, {{100, 10.0f}, {250, 13.0f}, {500, 16.0f}, {750, 19.0f}, {1000, 22.0f}, {0, 0.0f}}}, // 20
    {{{22, 0.25f}, {-1, 0.0f}, {-1, 0.0f}, {20, 0.125f}}, {{100, 10.0f}, {250, 16.0f}, {500, 16.0f}, {750, 19.0f}, {1000, 22.0f}, {0, 0.0f}}}, // 21
    {{{17, 0.25f}, {21, 0.25f}, {13, 0.25f}, {24, 0.0f}}, {{250, 7.0f}, {500, 10.0f}, {0, 0.0f}, {750, 16.0f}, {1000, 19.0f}, {2000, 25.0f}}}, // 22
    {{{-1, 0.0f}, {-1, 0.0f}, {-1, 0.0f}, {0, 1.0f}}, {{500, 6.0f}, {1000, 12.0f}, {0, 0.0f}, {0, 0.0f}, {5000, 20.0f}, {0, 0.0f}}}, // 23
    {{{-1, 0.0f}, {-1, 0.0f}, {22, 1.0f}, {-1, 0.0f}}, {{1000, 3.0f}, {2500, 5.0f}, {5000, 7.0f}, {0, 0.0f}, {10000, 14.0f}, {0, 0.0f}}}, // 24
    }};
    return table;
}

int level_count(std::size_t part) {
    int n = 0;
    for (const auto &l : body_parts_table()[part].levels) n += l.points > 0;
    return n;
}

const std::vector<Bone> &bones() {
    // The 19 bones of the original's X-ray skeleton (Skate 3 `dem_bones_hom`), in hom_rig.h order,
    // with the body part each shows and the sound material Skate 3 gives it (aud_material hom_*).
    static const std::vector<Bone> list{
        {"Skull", "head", 0},        {"Neck", "head", 23},         {"Rib Cage", "torso", 11},   {"Lower Spine", "torso", 13},
        {"Hips", "torso", 22},       {"Left Bicep", "arm", 4},     {"Right Bicep", "arm", 8},   {"Left Forearm", "arm", 3},
        {"Right Forearm", "arm", 7}, {"Left Hand", "arm", 2},      {"Right Hand", "arm", 6},    {"Left Thigh", "leg", 17},
        {"Right Thigh", "leg", 21},  {"Left Calf", "leg", 16},     {"Right Calf", "leg", 20},   {"Left Ankle", "foot", 15},
        {"Right Ankle", "foot", 19}, {"Left Toes", "foot", 14},    {"Right Toes", "foot", 18},
    };
    return list;
}

namespace {
std::uint64_t next(std::uint64_t &s) { // splitmix64
    s += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float unit(std::uint64_t &s) { return static_cast<float>(next(s) >> 40) / static_cast<float>(1 << 24); }
float dist(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

// Skate 3's homscoring.apt shows a metric row once its points pass the screen's threshold (speed,
// drop and bail time 1, air 5) or once the game flags the metric; rotation, which seldom pays, shows
// on the flag alone (the original reads "79°" with no points).
int metric_points(int metric, float value) {
    // Skate 3's score graphs (eight points each, from the same layout data). Checked against the
    // original's HUD: 7.10 s of bail read 7,750, 31.5 m of drop 5,740, 65.5 km/h 6,383.
    struct Graph {
        float x[8], y[8];
    };
    static const Graph graphs[5]{
        {{0, 719.9f, 720, 1080, 1800, 2520, 3240, 3500}, {0, 0, 750, 1250, 2500, 5000, 10000, 15000}}, // rotation, degrees
        {{0, 1.49999f, 1.5f, 2, 2.5f, 3, 3.1f, 3.4f}, {0, 0, 750, 1250, 2500, 5000, 10000, 10000}},   // air, seconds
        {{0, 6.99999f, 7, 10.5f, 17.5f, 30, 40, 45}, {0, 0, 750, 1250, 2500, 5000, 10000, 10000}},    // drop, metres
        {{0, 4.999f, 5, 5.25f, 5.5f, 6, 8, 9}, {0, 0, 750, 1250, 2500, 5000, 10000, 15000}},          // bail time, seconds
        {{0, 9.9999f, 10, 12.5f, 17.5f, 20, 45, 50}, {0, 0, 750, 2500, 5000, 10000, 10000, 10000}},   // speed, m/s
    };
    if (metric < 0 || metric > 4 || !(value > 0)) return 0;
    const auto &g = graphs[metric];
    if (value >= g.x[7]) return static_cast<int>(g.y[7]);
    for (int k = 1; k < 8; ++k)
        if (value < g.x[k]) return static_cast<int>(g.y[k - 1] + (g.y[k] - g.y[k - 1]) * (value - g.x[k - 1]) / (g.x[k] - g.x[k - 1]));
    return static_cast<int>(g.y[7]);
}

bool metric_visible(const Result &r, int metric) {
    switch (metric) {
    case 0: return r.rotation >= 1.0f;
    case 1: return r.air_points > 5;
    case 2: return r.drop_points > 1;
    case 3: return r.duration_points > 1;
    case 4: return r.speed_points > 1;
    }
    return false;
}

const char *title_for(int total, std::size_t n) {
    if (n >= 20) return "TOTAL ANNIHILATION";
    if (n >= 12) return "SKELETAL DISASTER";
    if (n >= 7) return "MASSIVE CARNAGE";
    if (n >= 3) return "SERIOUS MEAT";
    if (n >= 1) return "SNAP, CRACKLE";
    return total > 0 ? "JUST A TUMBLE" : "NO DAMAGE";
}

std::string Tracker::watch_line() const {
    float torso = std::max({bone_change_[2], bone_change_[3], bone_change_[4]}), any = 0;
    for (const float c : bone_change_) any = std::max(any, c);
    char text[200];
    std::snprintf(text, sizeof(text), "state %u%s, speed %.1f (peak %.1f), hips %+.1f m/s, lying %d, hit torso %.1f any %.1f, fell %.2fs ago",
                  previous_.physics_state, from_air_ ? " from the air" : "", body_speed_, recent_peak_speed_, hips_vertical_,
                  previous_.lying ? 1 : 0, torso, any, fast_fall_time_ < 0 ? -1.0 : previous_.time - fast_fall_time_);
    return text;
}

void Tracker::reset() {
    phase_ = Phase::idle;
    live_ = {};
    have_previous_ = false;
    slow_since_ = air_start_ = -1;
}

// Each bone's middle: how much speed it lost over the last ~0.13 s. A hit stops a bone, a swing only
// turns it. Kept every tick (not only during a bail) so a crash can be recognised by its impact.
void Tracker::track_bones(const Sample &previous, const Sample &now, double dt) {
    const auto length = [](const std::array<float, 3> &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    for (std::size_t k = 0; k + 1 < bone_times_.size(); ++k) bone_times_[k] = bone_times_[k + 1];
    bone_times_.back() = now.time;
    bone_samples_ = std::min<int>(bone_samples_ + 1, static_cast<int>(bone_times_.size()));
    for (std::size_t i = 0; i < 19; ++i) {
        std::array<float, 3> v{};
        for (std::size_t k = 0; k < 3; ++k) v[k] = static_cast<float>((now.bone_centres[i][k] - previous.bone_centres[i][k]) / dt);
        auto &h = bone_speeds_[i];
        for (std::size_t k = 0; k + 1 < h.size(); ++k) h[k] = h[k + 1];
        h.back() = length(v);
    }
    bone_change_ = {};
    bone_hit_ = {};
    if (bone_samples_ < 3) return;
    for (std::size_t i = 0; i < 19; ++i) {
        const auto &h = bone_speeds_[i];
        float before = 0;
        for (std::size_t k = h.size() - static_cast<std::size_t>(bone_samples_); k + 1 < h.size(); ++k)
            if (now.time - bone_times_[k] <= 0.13) before = std::max(before, h[k]);
        const float change = before - h.back();
        // Teleports and respawns move the whole body at once: not a hit.
        if (change > 0 && change <= 60.0f) bone_change_[i] = change;
    }
    // Skate 3 damages the parts that take the impact, not every bone of a body that stops: a bone
    // gets its whole loss only when it is down on the ground (within 0.3 m of the lowest bone),
    // otherwise only what it lost beyond the body's mean (a limb or the head striking something).
    {
        float lowest = now.bone_centres[0][1], mean = 0;
        for (std::size_t i = 0; i < 19; ++i) {
            lowest = std::min(lowest, now.bone_centres[i][1]);
            mean += bone_change_[i] / 19.0f;
        }
        for (std::size_t i = 0; i < 19; ++i)
            bone_hit_[i] = now.bone_centres[i][1] - lowest < 0.3f ? bone_change_[i] : std::max(0.0f, bone_change_[i] - mean);
    }
    float sum = 0;
    for (std::size_t i = 0; i < 19; ++i) sum += bone_speeds_[i].back();
    body_speed_ = sum / 19.0f;
    const float vertical = static_cast<float>((now.bone_centres[4][1] - previous.bone_centres[4][1]) / dt);
    hips_vertical_ += (vertical - hips_vertical_) * std::min(1.0f, static_cast<float>(dt) * 8.0f);
    if (vertical <= -3.0f && vertical > -60.0f) fast_fall_time_ = now.time;
    if (vertical <= -6.0f && vertical > -60.0f) hard_fall_time_ = now.time;
    // A respawn or teleport moves the body hundreds of metres in a tick: not a speed.
    if (body_speed_ < 80.0f) recent_peak_speed_ = std::max(body_speed_, recent_peak_speed_ - 12.0f * static_cast<float>(dt));
    {
        std::array<float, 3> v{};
        for (std::size_t k = 0; k < 3; ++k) v[k] = static_cast<float>((now.bone_centres[4][k] - previous.bone_centres[4][k]) / dt);
        const float blend = std::min(1.0f, static_cast<float>(dt) * 15.0f);
        for (std::size_t k = 0; k < 3; ++k) {
            const float a = static_cast<float>((v[k] - hips_velocity_[k]) / dt);
            if (std::isfinite(a) && std::abs(a) < 500.0f) hips_accel_[k] += (a - hips_accel_[k]) * blend;
        }
        hips_velocity_ = v;
        const float sideways = std::sqrt(hips_accel_[0] * hips_accel_[0] + hips_accel_[2] * hips_accel_[2]);
        free_fall_ = hips_accel_[1] < -6.5f && hips_accel_[1] > -13.0f && sideways < 3.0f;
    }
}

// A hit on a body part: it reaches every damage level whose impact the hit meets, and each part
// linked to it takes its share of the hit (Skate 3's propagation, one step).
void Tracker::hit_part(int part, float impact, double time) {
    const auto &table = body_parts_table();
    const auto apply = [&](int p, float x) {
        std::array<float, 6> needed{};
        int n = 0, reached = 0;
        for (const auto &l : table[p].levels)
            if (l.points > 0) needed[n++] = l.impact;
        std::sort(needed.begin(), needed.begin() + n);
        for (int k = 0; k < n; ++k)
            if (x >= needed[k]) reached = k + 1;
        if (reached <= live_.level[p]) return;
        live_.level[p] = reached;
        live_.events.push_back({p, reached, reached == n, x, time});
    };
    if (part < 0 || part >= static_cast<int>(body_parts) || impact <= 0) return;
    apply(part, impact);
    for (int p = 0; p < static_cast<int>(body_parts); ++p)
        for (const auto &link : table[p].links)
            if (link.part == part && link.weight > 0) apply(p, impact * link.weight);
    // The X-ray's view of it.
    const auto &list = bones();
    for (std::size_t i = 0; i < list.size() && i < 19; ++i) {
        const int n = level_count(static_cast<std::size_t>(list[i].part));
        const int l = live_.level[static_cast<std::size_t>(list[i].part)];
        live_.damage[i] = n ? static_cast<float>(l) / static_cast<float>(n) : 0.0f;
        if (n && l == n && !is_broken_[i]) {
            is_broken_[i] = true;
            live_.broken.push_back({i, impact, time});
        }
    }
}

// During a bail: each X-ray bone's own hit lands on its body part.
void Tracker::apply_bone_hits(const Sample &now) {
    if (now.time - start_.time < 0.15) return; // the wipeout's own first jolt
    const auto &list = bones();
    for (std::size_t i = 0; i < list.size() && i < 19; ++i) {
        const float change = bone_hit_[i];
        if (change <= 0) continue;
        live_.biggest_hit = std::max(live_.biggest_hit, change);
        hit_part(list[i].part, change, now.time);
    }
}

// Without the skeleton: a sudden loss of speed lands on a few random parts.
void Tracker::register_impact(float drop) {
    ++live_.impacts;
    live_.biggest_hit = std::max(live_.biggest_hit, drop);
    const auto &list = bones();
    const int strikes = std::clamp(static_cast<int>(drop / 3.0f), 1, 6);
    for (int i = 0; i < strikes; ++i) {
        const auto index = std::min(list.size() - 1, static_cast<std::size_t>(unit(rng_) * static_cast<float>(list.size())));
        hit_part(list[index].part, drop * (0.6f + 0.8f * unit(rng_)), previous_.time);
    }
}

void Tracker::score(Result &r) const {
    // Every damaged part pays the points of the highest level it reached.
    r.bone_points = 0;
    const auto &table = body_parts_table();
    for (std::size_t p = 0; p < body_parts; ++p) {
        if (r.level[p] <= 0) continue;
        std::array<DamageLevel, 6> sorted{};
        std::copy(std::begin(table[p].levels), std::end(table[p].levels), sorted.begin());
        std::sort(sorted.begin(), sorted.end(), [](const DamageLevel &a, const DamageLevel &b) {
            return (a.points > 0) != (b.points > 0) ? a.points > 0 : a.impact < b.impact;
        });
        r.bone_points += sorted[static_cast<std::size_t>(r.level[p] - 1)].points;
    }
    r.rotation_points = metric_points(0, r.rotation);
    r.air_points = metric_points(1, r.air_time);
    r.drop_points = metric_points(2, r.drop);
    r.duration_points = metric_points(3, r.duration);
    r.speed_points = metric_points(4, r.peak_speed);
    r.total = r.bone_points + r.air_points + r.drop_points + r.duration_points + r.speed_points + r.rotation_points;
    r.title = title_for(r.total, r.broken.size());
}

bool Tracker::update(const Sample &s) {
    bool finished = false;
    if (phase_ == Phase::finished) phase_ = Phase::idle;
    {
        const double step = s.time - previous_.time;
        if (have_previous_ && s.bones_valid && previous_.bones_valid && step > 0 && step < 0.25) {
            track_bones(previous_, s, step);
        } else {
            bone_samples_ = 0;
            bone_change_ = {};
            bone_hit_ = {};
        }
    }
    if (phase_ == Phase::idle) {
        // A bail starts in one of two ways:
        //  - the game's wipeout physics state, at once;
        //  - an off-board crash: the body hits something hard (its chest or hips stop suddenly) and is
        //    then off its feet for a moment. Many slams, and every crash taken on foot, never enter the
        //    wipeout state. Coming off the board for a hippy jump, a plant or a dive does none of it:
        //    there is no hit, or the skater is back on their feet at once.
        const bool off_board = s.physics_state == config_.wipeout_state || s.physics_state == 504;
        if (s.physics_state == 504 && have_previous_ && previous_.physics_state >= 200 && previous_.physics_state < 300) from_air_ = true;
        if (!off_board) from_air_ = false;
        // Leaving the board (or the feet): where a crash would start from.
        const bool was_off_board = have_previous_ && (previous_.physics_state == config_.wipeout_state || previous_.physics_state == 504);
        // Standing or walking about off the board is not the start of anything; falling upright is.
        if (!off_board || (s.bones_valid && s.upright && std::abs(hips_vertical_) < 1.5f)) {
            have_offboard_start_ = false;
        } else if (!was_off_board || !have_offboard_start_) {
            offboard_start_ = s;
            have_offboard_start_ = true;
            pre_air_ = 0;
        } else if (free_fall_ && have_previous_) {
            pre_air_ += static_cast<float>(s.time - previous_.time);
        }
        if (s.bones_valid && s.upright) {
            if (upright_idle_since_ < 0) upright_idle_since_ = s.time;
            if (s.time - upright_idle_since_ >= 0.3) armed_ = true;
        } else {
            upright_idle_since_ = -1;
        }
        if (s.bones_valid && s.lying && off_board) {
            if (down_since_ < 0) {
                down_since_ = s.time;
                down_start_ = s;
            }
        } else {
            down_since_ = -1;
        }
        {
            float torso = std::max({bone_change_[2], bone_change_[3], bone_change_[4]}), any = 0;
            for (const float c : bone_change_) any = std::max(any, c);
            if (off_board && (torso >= config_.hit_torso || any >= config_.hit_any)) hit_time_ = s.time;
            // A body that falls hard and smashes into something is a crash at that instant, as Skate 3
            // starts its Hall of Meat on the bail: no waiting for it to lie flat.
            slam_now_ = off_board && s.bones_valid && !s.upright && hard_fall_time_ >= 0 && s.time - hard_fall_time_ <= 0.3 &&
                        (torso >= 6.0f || any >= 12.0f);
            for (std::size_t i = 0; i < 19; ++i)
                if (bone_hit_[i] > 0 && (s.time - recent_hit_time_[i] > 3.0 || bone_hit_[i] >= recent_hit_[i])) {
                    recent_hit_[i] = bone_hit_[i];
                    recent_hit_time_[i] = s.time;
                }
        }
        const bool wipeout = s.physics_state == config_.wipeout_state && (!have_previous_ || previous_.physics_state != config_.wipeout_state);
        // On the ground: a spread-eagle glide or a dive is down and can jolt, but it keeps sinking.
        const bool grounded = std::abs(hips_vertical_) < 1.5f;
        // A crash stops the body; a glide or a dive sails on at speed whatever it brushes.
        const bool stopping = body_speed_ < 5.0f || body_speed_ < 0.65f * recent_peak_speed_;
        // Off the board since the air: the body must have come down onto something.
        const bool came_down = !from_air_ || (fast_fall_time_ >= 0 && hit_time_ >= 0 && hit_time_ - fast_fall_time_ <= 0.6);
        const bool thrown = armed_ && grounded && stopping && came_down && down_since_ >= 0 && s.time - down_since_ >= config_.down_time &&
                            hit_time_ >= 0 && s.time - hit_time_ <= 0.8 && hit_time_ >= down_since_ - 0.4;
        const bool slammed = armed_ && slam_now_ && !thrown;
        if (wipeout) trigger_ = "wipeout state";
        else if (thrown) trigger_ = "hit and down";
        else if (slammed) trigger_ = "slam";
        if (wipeout || thrown || slammed) {
            armed_ = false;
            hit_time_ = -1;
            down_since_ = -1;
            phase_ = Phase::bailing;
            live_ = {};
            live_.serial = ++serial_;
            rng_ = 0xC0FFEEull * live_.serial + 17;
            is_broken_.assign(bones().size(), false);
            went_down_ = !s.upright;
            upright_since_ = -1;
            start_ = thrown ? down_start_ : s;
            // The bail started where the skater left the board or their feet, if that was just now.
            if ((thrown || slammed) && have_offboard_start_ && s.time - offboard_start_.time < 4.0) {
                start_ = offboard_start_;
                live_.air_time = pre_air_;
            }
            turn_ = 0;
            slow_since_ = -1;
            air_start_ = -1;
            live_.peak_speed = s.speed;
            frozen_ = false;
            still_since_ = -1;
            // The impact that showed this was a crash came before the bail was recognised: count it.
            if (thrown || slammed) {
                const auto &list = bones();
                for (std::size_t i = 0; i < list.size() && i < 19; ++i) {
                    if (s.time - recent_hit_time_[i] > 3.0 || recent_hit_[i] <= 0) continue;
                    live_.biggest_hit = std::max(live_.biggest_hit, recent_hit_[i]);
                    hit_part(list[i].part, recent_hit_[i], recent_hit_time_[i]);
                }
            }
            recent_hit_ = {};
        }
    } else if (have_previous_) {
        const double dt = s.time - previous_.time;
        const double elapsed = s.time - start_.time;
        {
            const float moving = s.bones_valid ? body_speed_ : s.speed;
            if (moving < config_.still_speed && elapsed > 0.5) {
                if (still_since_ < 0) still_since_ = s.time;
                if (s.time - still_since_ >= config_.still_time) frozen_ = true;
            } else {
                // Once the body has stopped the bail's numbers are final, as in Skate 3: getting up
                // (or being dragged by the ragdoll) does not count on.
                still_since_ = -1;
            }
        }
        if (!frozen_ && dt > 0 && dt < 0.25) {
            const float drop = previous_.speed - s.speed;
            if (s.bones_valid && previous_.bones_valid) {
                if (drop >= config_.impact_threshold) ++live_.impacts;
                apply_bone_hits(s);
            } else if (drop >= config_.impact_threshold) {
                register_impact(drop);
            }
            // In the air: the board says so while riding; off it, the hips are rising or falling freely.
            float hips_vertical = 0;
            if (s.bones_valid && previous_.bones_valid) hips_vertical = static_cast<float>((s.bone_centres[4][1] - previous_.bone_centres[4][1]) / dt);
            (void)hips_vertical;
            // In the air: the board says so while riding; off it, the body is in free fall (sliding
            // down a ramp is fast and downward too, but it is not air).
            const bool flying = s.airborne || (s.bones_valid && free_fall_);
            if (flying) {
                if (air_start_ < 0) air_start_ = previous_.time;
            } else if (air_start_ >= 0) {
                live_.air_time += static_cast<float>(s.time - air_start_);
                air_start_ = -1;
            }
        }
        if (!frozen_) {
            live_.distance = dist(s.position, start_.position);
            live_.peak_speed = std::max(live_.peak_speed, s.speed);
            live_.peak_height = std::max(live_.peak_height, s.position[1] - start_.position[1]);
            live_.drop = std::max(live_.drop, start_.position[1] - s.position[1]);
            float turn = s.heading - previous_.heading;
            while (turn > 180.0f) turn -= 360.0f;
            while (turn < -180.0f) turn += 360.0f;
            // Only while the body is really moving: a still body's heading jitters.
            if (dt > 0 && dt < 0.25 && (s.bones_valid ? body_speed_ : s.speed) > 1.0f) turn_ += turn;
            // Net turn: a tumbling ragdoll's heading twitches back and forth, which must not add up.
            live_.rotation = std::max(live_.rotation, std::abs(turn_));
            live_.duration = static_cast<float>(elapsed);
        } else if (air_start_ >= 0) {
            live_.air_time += static_cast<float>(s.time - air_start_);
            air_start_ = -1;
        }
        if (s.speed < config_.settle_speed) {
            if (slow_since_ < 0) slow_since_ = s.time;
        } else {
            slow_since_ = -1;
        }
        // Lying still is not the end while the skeleton is read: the X-ray stays until the skater is up.
        const bool settled = !s.bones_valid && slow_since_ >= 0 && s.time - slow_since_ >= config_.settle_time;
        // With the skeleton: over once the skater is back on their feet. Without it: out of the
        // wipeout state and slow.
        if (s.bones_valid && !s.upright) went_down_ = true;
        if (s.bones_valid && s.upright) {
            if (upright_since_ < 0) upright_since_ = s.time;
        } else {
            upright_since_ = -1;
        }
        // Back on the board ends it at once.
        const bool riding = s.physics_state >= 100 && s.physics_state < 300 && elapsed > 0.3;
        const bool recovered = riding || (s.bones_valid ? (went_down_ && upright_since_ >= 0 && s.time - upright_since_ >= 0.4)
                                                       : (s.physics_state != config_.wipeout_state && elapsed > 0.5 && s.speed < 2.0f));
        if (settled || recovered || elapsed > config_.max_duration) {
            if (air_start_ >= 0) live_.air_time += static_cast<float>(s.time - air_start_);
            air_start_ = -1;
            if (elapsed >= config_.min_duration) {
                score(live_);
                last_ = live_;
                phase_ = Phase::finished;
                finished = true;
            } else {
                phase_ = Phase::idle;
            }
        }
    }
    if (phase_ == Phase::bailing) {
        score(live_);
        for (int k = 0; k < 5; ++k)
            if (live_.shown_at[k] < 0 && metric_visible(live_, k)) live_.shown_at[k] = s.time;
    }
    previous_ = s;
    have_previous_ = true;
    return finished;
}
} // namespace dingosdk::hall_of_meat
