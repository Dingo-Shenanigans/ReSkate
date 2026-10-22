#include "filmer_rig.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace dingosdk::filmer {
namespace {
constexpr float pi = std::numbers::pi_v<float>;
constexpr float degrees = pi / 180.0f;

float length(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float flat_length(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
Vec3 add(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(const Vec3 &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 lerp(const Vec3 &a, const Vec3 &b, float t) { return add(a, scale(sub(b, a), t)); }
bool finite(const Vec3 &v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
// The share of the way an eased value moves in `dt` towards its target, for a time constant `tau`.
float ease(float dt, float tau) { return tau <= 0 ? 1.0f : 1.0f - std::exp(-dt / tau); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
// A second-order spring towards a moving target: no lag behind a target moving at constant speed.
void spring(float &x, float &v, float target, float target_rate, float omega, float damping, float dt) {
    // Substeps keep a stiff spring stable through a long frame.
    const int steps = std::clamp(static_cast<int>(std::ceil(dt * omega / 0.25f)), 1, 16);
    const float h = dt / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        const float a = omega * omega * (target - x) + 2.0f * damping * omega * (target_rate - v);
        v += a * h;
        x += v * h;
    }
}
// The same, for an angle: the target is reached the short way round.
void angle_spring(float &x, float &v, float target, float omega, float damping, float dt, float target_rate = 0.0f) {
    const float goal = x + wrap_angle(target - x);
    spring(x, v, goal, target_rate, omega, damping, dt);
    x = wrap_angle(x);
}
float yaw_of(float x, float z) { return std::atan2(x, z); }
} // namespace

float wrap_angle(float radians) noexcept {
    if (!std::isfinite(radians)) return 0;
    radians = std::fmod(radians + pi, 2.0f * pi);
    if (radians < 0) radians += 2.0f * pi;
    return radians - pi;
}

bool valid_camera(const Matrix &m) noexcept {
    for (float value : m)
        if (!std::isfinite(value) || std::abs(value) > 1000000.0f) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        const float l = std::sqrt(m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2]);
        if (std::abs(l - 1.0f) > 0.02f) return false;
        for (std::size_t other = row + 1; other < 3; ++other) {
            const float dot = m[row * 4] * m[other * 4] + m[row * 4 + 1] * m[other * 4 + 1] + m[row * 4 + 2] * m[other * 4 + 2];
            if (std::abs(dot) > 0.02f) return false;
        }
    }
    return true;
}

float Rig::Wobble::sample(double t) const noexcept {
    float total = 0;
    for (std::size_t i = 0; i < 3; ++i)
        total += std::sin(static_cast<float>(std::fmod(t * frequency[i], 1.0)) * 2.0f * pi + phase[i]) * (i == 0 ? 0.6f : i == 1 ? 0.3f : 0.15f);
    return total; // about -1..1
}

Rig::Rig() : random_(0x5658u) {
    // Each wobble sums three sines at frequencies that never line up, so the motion never repeats.
    // Rows: the slow sway of a body on a board, the hands, and the board's rattle over the ground.
    const std::array<float, 9> base{0.23f, 0.31f, 0.27f, 1.1f, 1.37f, 0.9f, 9.0f, 11.3f, 7.7f};
    std::uniform_real_distribution<float> phase(0.0f, 2.0f * pi), spread(0.85f, 1.2f);
    for (std::size_t i = 0; i < wobble_.size(); ++i)
        for (std::size_t k = 0; k < 3; ++k) {
            wobble_[i].phase[k] = phase(random_);
            wobble_[i].frequency[k] = base[i] * spread(random_) * (k == 0 ? 1.0f : k == 1 ? 1.618f : 2.414f);
        }
    reset();
}

void Rig::reset() noexcept {
    subject_ = {};
    self_ = {};
    trail_.clear();
    history_.clear();
    aim_ = {};
    operator_have_ = false;
    eye_have_ = eye_y_have_ = false;
    operator_velocity_ = {};
    operator_height_rate_ = 0;
    lean_ = 0;
    angle_ = angle_target_ = 22.0f * degrees;
    angle_rate_ = 0;
    angle_until_ = 0;
    offset_yaw_ = offset_pitch_ = 0;
    pushing_ = false;
    planted_ = edge_ = holding_ = false;
    flat_air_ = false;
    unseen_since_ = -1.0;
    plan_angle_ = angle_;
    plan_risk_ = 0.0f;
    plan_scale_ = 1.0f;
    clear_since_ = -1.0;
    plan_next_ = plan_keep_ = 0.0;
    fresh_ = true;
    halt_ = Halt::none;
    halt_speed_ = 0.0f;
    halt_stopped_ = halt_zoom_rolled_ = drifting_ = drift_over_ = false;
    ollie_until_ = -1.0;
    ollie_pop_at_ = -100.0;
    ollie_next_ = 0.0;
    ollie_planned_ = false;
    bail_over_since_ = -1.0;
    auto_zoom_ = 1.0f;
    zoom_kind_ = 0;
    auto_zooming_ = false;
    speed_slow_ = 0.0f;
    pushed_ = was_on_foot_ = on_foot_ = false;
    caution_ = false;
    caution_next_ = 0.0;
    grinding_ = false;
    grind_weight_ = board_weight_ = 0.0f;
    grind_over_since_ = -1.0;
    post_ = Post::none;
    raised_was_ = ledge_side_ = false;
    wall_raise_ = wall_raise_target_ = wall_raise_rate_ = 0.0f;
    wall_next_ = 0.0;
    hidden_since_ = seen_low_since_ = -1.0;
    peeking_ = false;
    skater_accel_ = last_speed_ = 0.0f;
    lift_ = lift_rate_ = air_lift_ = air_lift_rate_ = 0.0f;
    focus_have_ = false;
    floor_rate_ = 0.0f;
    rest_have_ = false;
    still_since_ = rest_bad_since_ = -1.0;
    rest_next_check_ = 0.0;
    rolling_stop_ = stop_due_ = framed_ = pan_active_ = false;
    stop_lift_ = stop_lift_rate_ = 0.0f;
}

void Rig::set_planted(bool planted) noexcept {
    if (planted_ && !planted && operator_have_) {
        // Riding again: from far off the filmer cuts to a new spot behind you instead of a long chase.
        const float away = flat_length(sub(subject_.position, operator_));
        if (away > 12.0f) operator_have_ = false;
    }
    planted_ = planted;
    edge_ = false;
    // Stair stop: still rolling, they brake to a halt (short of a drop ahead) instead of stopping dead.
    rolling_stop_ = planted && operator_have_ && flat_length(operator_velocity_) > 0.3f;
    stop_due_ = rolling_stop_;
    if (!planted) framed_ = pan_active_ = composing_ = false;
}

void Rig::begin_tick(double now) noexcept {
    now_ = now;
    asked_last_tick_ = asked_this_tick_;
    asked_this_tick_ = 0;
    probe_budget_ = 12;
    ray_budget_ = 12;
    if (probes_.size() > 4096) probes_.clear();
}

std::optional<float> Rig::ground_at(float x, float z, float near_y, bool always) const {
    if (!cast_ || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(near_y)) return std::nullopt;
    const auto cell = [](float v, float size) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(v / size)) & 0x1fffff); };
    const std::uint64_t key = cell(x, 0.25f) | cell(z, 0.25f) << 21 | cell(near_y, 0.5f) << 42;
    const auto found = probes_.find(key);
    // The ground under the lens is asked fresh each tick, at its own spot: a cell's answer is from
    // somewhere else in the cell, up to a hand's height off on a bank.
    const bool fresh = !always && found != probes_.end() && now_ - found->second.time < 2.0;
    if (!fresh && (probe_budget_ > 0 || always)) {
        if (probe_budget_ > 0 && !always) --probe_budget_;
        Vec3 hit{};
        Probe probe{false, 0.0f, now_};
        ++asked_this_tick_;
        if (cast_(Vec3{x, near_y + 0.9f, z}, Vec3{x, near_y - 6.0f, z}, hit) && std::isfinite(hit[1])) probe = {true, hit[1], now_};
        probes_[key] = probe;
        ++ground_asked_;
        ground_answers_ += ((probe.hit ? 1.0f : 0.0f) - ground_answers_) * 0.03f;
        if (!world_answers()) return near_y;
        return probe.hit ? std::optional<float>(probe.y) : std::nullopt;
    }
    if (!world_answers()) return near_y; // the game is not answering: level, the trail decides
    if (found != probes_.end()) return found->second.hit ? std::optional<float>(found->second.y) : std::nullopt;
    return near_y; // out of questions this tick and never asked here: assume it is level
}

bool Rig::blocked(const Vec3 &from, const Vec3 &to, Vec3 *hit, bool always) const {
    if (!cast_ || (ray_budget_ == 0 && !always)) return false;
    if (ray_budget_ > 0 && !always) --ray_budget_; // (one that must be asked does not use up the others' share)
    Vec3 found{};
    ++asked_this_tick_;
    if (!cast_(from, to, found)) return false;
    if (hit) *hit = found;
    return true;
}

// In the way at the knee (or wherever `above` metres over the ground at each end): a hit that is only
// the ground itself (the ray cut across the brow of a hill, the lip of a bank) does not count; a face
// standing up from it (a wall, a ledge, a trunk) does.
bool Rig::obstructed(const Vec3 &from, const Vec3 &to, float above, bool always) const {
    Vec3 hit{};
    if (!blocked(from, to, &hit, always)) return false;
    const float base = std::max(from[1], to[1]) - above;
    const auto g = ground_at(hit[0], hit[2], hit[1] + 0.3f, always);
    return !(g && std::abs(*g - hit[1]) < 0.12f && *g <= base + 0.2f);
}

const Rig::TrailPoint *Rig::nearest_trail(float x, float z, float within) const {
    const TrailPoint *best{};
    float best_distance = within * within;
    for (const auto &point : trail_) {
        const float dx = point.at[0] - x, dz = point.at[2] - z, d = dx * dx + dz * dz;
        if (d < best_distance) {
            best_distance = d;
            best = &point;
        }
    }
    return best;
}

// Whether a filmer on a board could roll from where they are to `to` (with ground at `to_ground`):
// not up onto anything a board will not roll up, and, with stop_at_drops, not off a drop or over a
// stretch you flew over (a set, a gap): there they stop at the edge and film.
bool Rig::step_allowed(const Vec3 &, float to_ground, bool stop_at_drops) const {
    const float rise = to_ground - operator_ground_;
    // Up onto what you ollied onto (a ledge, a manual pad): an ollie up, never higher than that.
    if (rise > 0.3f && !(join_ok_ && to_ground <= join_to_ + 0.15f && rise <= join_height + 0.15f)) return false;
    if (!stop_at_drops) return true;
    return rise >= -0.85f; // a curb, a planter's edge: rolled off; a set or a big drop: not
}

// The vertical half of the view (radians): the fisheye's frame edge sees half the game camera's FOV.
float Rig::half_view(const RigSettings &s) const { return render_fov(s.lens) / zoom_ * 0.5f * degrees; }

// The pitch that frames a skater standing at `root` (their feet) from `eye`: the board at a fixed
// place below the centre (tilt), raised if their head would be cut, but never so far the board leaves.
float Rig::frame_pitch(const Vec3 &eye, const Vec3 &root, bool, float, const RigSettings &s) const {
    const float horizontal = std::max(flat_length(sub(root, eye)), 0.3f);
    const float feet = std::atan2(root[1] + 0.05f - eye[1], horizontal);
    const float head = std::atan2(root[1] + 1.7f - eye[1], horizontal);
    const float view = half_view(s);
    float pitch = feet + mix(0.15f, 0.75f, std::clamp(s.tilt, 0.0f, 1.0f)) * view;
    pitch = std::max(pitch, head - 0.88f * view);
    return std::min(pitch, feet + 0.82f * view);
}

void Rig::slap(double now) noexcept {
    slap_at_ = now;
    std::uniform_real_distribution<float> side(-1.0f, 1.0f);
    slap_yaw_ = side(random_) > 0 ? 1.0f : -1.0f;
    slap_pitch_ = -0.6f - 0.4f * std::abs(side(random_));
}

float Rig::angle_degrees() const noexcept { return angle_ / degrees; }

// The zoom eases to where it is wanted, as a servo zoom does; riding again (or the fisheye on), back
// out to the long lens's widest.
void Rig::ease_zoom(float dt, const RigSettings &s) noexcept {
    if (s.lens != Lens::long_lens || !stationary()) zoom_target_ = 1.0f;
    // A zoom of the filmer's own (a slam, a stop) goes in, and later back out, at one steady moderate
    // pace (in steps of the picture's size, so it looks the same pace at any zoom), easing only as it
    // arrives. Yours (the long lens held still) is a quick servo.
    const float want = std::max(zoom_target_, auto_zoom_);
    if (auto_zoom_ > 1.001f || auto_zooming_) {
        const float from = std::log(std::max(zoom_, 1.0f)), to = std::log(std::max(want, 1.0f));
        const float diff = to - from;
        // A thumb on the zoom rocker: the pace builds as it is pressed in (not at full speed from the
        // first frame), wavers a touch with the hand, and eases off as the framing arrives.
        const float waver = 1.0f + 0.15f * wobble_[6].sample(now_ * 0.8);
        const float want_rate = std::copysign(std::min(zoom_pace_, std::abs(diff) * 2.5f + 0.04f) * waver, diff);
        zoom_rate_ += (want_rate - zoom_rate_) * ease(dt, std::abs(want_rate) > std::abs(zoom_rate_) ? 0.35f : 0.12f);
        float step = zoom_rate_ * dt;
        if (step * diff < 0.0f) step = 0.0f;              // (never back the wrong way)
        if (std::abs(step) > std::abs(diff)) step = diff; // (never past it)
        zoom_ = std::exp(from + step);
        auto_zooming_ = auto_zoom_ > 1.001f || std::abs(zoom_ - want) > 0.002f;
        zoom_moving_ = std::abs(zoom_ - want) > 0.01f;
    } else {
        zoom_ += (want - zoom_) * ease(dt, 0.15f);
        zoom_moving_ = false;
        zoom_rate_ = 0.0f;
    }
    if (std::abs(zoom_ - want) < 0.002f) zoom_ = want;
}

bool Rig::stationary() const noexcept {
    if (filming_) return self_.have && flat_length(self_.velocity) < 0.5f;
    return operator_have_ && ((planted_ && !rolling_stop_) || (halt_ != Halt::none && halt_stopped_));
}

void Rig::look(float right, float down) noexcept {
    // (You filming: the mouse aims whether you stand, walk or ride; WASD moves you as ever.)
    if ((!stationary() && !filming_) || !std::isfinite(right) || !std::isfinite(down)) return;
    if (filming_) {
        // As the right stick does: the framing's offset (the assist keeps framing them past it).
        offset_yaw_ = wrap_angle(offset_yaw_ - right);
        offset_pitch_ = std::clamp(offset_pitch_ - down, -1.2f, 1.2f);
        stick_idle_since_ = now_;
    } else if (aim_.have) {
        aim_.yaw = wrap_angle(aim_.yaw - right);
        aim_.pitch = std::clamp(aim_.pitch - down, -1.35f, 1.35f);
        aim_.yaw_rate = aim_.pitch_rate = 0.0f;
        pan_active_ = composing_ = false; // your framing now, not the stair stop's
    }
}

// Stair stop: where to brake to. At a firm stop from their speed (5 m/s^2, a few metres at most), or
// short of a drop the ground ahead shows (the first step, a ledge's end), whichever comes first.
void Rig::compute_rolling_stop(float floor) noexcept {
    const float speed = flat_length(operator_velocity_);
    if (speed < 0.3f) { rolling_stop_ = false; return; }
    stop_dir_ = Vec3{operator_velocity_[0] / speed, 0.0f, operator_velocity_[2] / speed};
    float reach = std::clamp(speed * speed / (2.0f * 5.0f), 0.3f, 4.0f);
    if (cast_) {
        probe_budget_ += 16;
        for (float d = 0.4f; d <= reach + 1.2f; d += 0.4f) {
            const Vec3 at = add(operator_, scale(stop_dir_, d));
            const auto g = ground_at(at[0], at[2], floor + 0.5f);
            if (!g || *g < floor - 0.35f) {
                reach = std::min(reach, std::max(0.15f, d - 0.6f));
                break;
            }
        }
    }
    stop_point_ = add(operator_, scale(stop_dir_, reach));
    roll_speed_ = speed;
}

// Stair stop: the pitch that takes in the set (or the rail, the ledge) ahead: down to a point past the
// first drop the ground shows that way, a little above its bottom; with no drop to see, a little down.
// Always at least a touch down from where it was, never steeper than about 57 degrees.
float Rig::stair_pan_pitch(const Vec3 &eye, float yaw, float pitch, float floor) const {
    const Vec3 way{std::sin(yaw), 0.0f, std::cos(yaw)};
    float target = pitch - 0.14f;
    if (cast_) {
        probe_budget_ += 20;
        for (float d = 0.6f; d <= 8.0f; d += 0.4f) {
            const Vec3 at = add(eye, scale(way, d));
            const auto g = ground_at(at[0], at[2], floor + 0.5f);
            if (!g || *g < floor - 0.35f) {
                const float low = g ? *g : floor - 1.5f;
                const float out = d + 1.5f;
                target = std::atan2(low + 0.3f - eye[1], out);
                break;
            }
        }
    }
    return std::min(std::max(target, -1.0f), pitch - 0.06f);
}

void Rig::zoom_by(float factor, const RigSettings &settings) noexcept {
    if (settings.lens != Lens::long_lens || !stationary() || !(factor > 0.0f) || !std::isfinite(factor)) return;
    zoom_target_ = std::clamp(zoom_target_ * factor, 1.0f, max_zoom);
}

Rig::Cruise Rig::cruise(float gap) const noexcept {
    if (!filming_ || !framing_ || !self_.have || !subject_.have) return {};
    // How far they are ahead of you along the way they ride (behind you: negative).
    const Vec3 heading{subject_.heading_x, 0.0f, subject_.heading_z};
    const Vec3 rel{subject_.position[0] - self_.position[0], 0.0f, subject_.position[2] - self_.position[2]};
    const float ahead = rel[0] * heading[0] + rel[2] * heading[2];
    const float their_speed = flat_length(subject_.velocity), my_speed = flat_length(self_.velocity);
    if (their_speed < 1.0f) return {}; // they have stopped: nothing to keep up with
    // Their speed, and a pull on the gap: closing at up to 4 m/s, falling back at up to 3.
    const float want = std::clamp(their_speed + std::clamp((ahead - gap) * 0.8f, -3.0f, 4.0f), 0.0f, 14.0f);
    return {true, want, want < my_speed - 0.5f && ahead < gap, ahead};
}

// How far the skater's root (where the game says they are) is above the ground under their board.
// The game's root can sit at the hips: everything that compares the trail (roots) with the ground the
// game reports (the level, the ledges, the gaps, the framing of the feet) goes through this.
void Rig::learn_root(float dt, const Vec3 &root) {
    if (!cast_ || !world_answers()) return;
    const auto under = ground_at(root[0], root[2], root[1]);
    if (!under) return;
    const float d = root[1] - *under;
    if (d < -0.3f || d > 1.6f) return;
    if (!root_known_) {
        root_samples_.push_back(d);
        if (root_samples_.size() >= 15) {
            std::vector<float> sorted = root_samples_;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
            root_above_ground_ = sorted[sorted.size() / 2];
            root_known_ = true;
            root_samples_.clear();
        }
    } else if (std::abs(d - root_above_ground_) < 0.15f) {
        root_above_ground_ = mix(root_above_ground_, d, ease(dt, 3.0f));
    }
}

// The least the lens is ever above the ground: closer, the game's near clipping plane cuts into the
// ground at the corners of the wide lens and the picture shows under the floor.
constexpr float lens_clearance = 0.12f;

// Follows a skater from tick to tick: speed, the way they travel, whether they are in the air and
// the ground under them. A jump of more than 8 m is a teleport or a respawn: everything restarts.
void Rig::track(double now, float dt, const Subject &subject, Tracked &t, bool own_trail, bool learn) {
    Vec3 p = subject.position;
    // (More than 4 m in one tick is never riding: back to a marker, a teleport to another player.)
    if (!t.have || length(sub(p, t.position)) > std::max(4.0f, 30.0f * dt) || now - t.last_seen > 1.0) {
        if (own_trail) fresh_ = true;
        t = {};
        t.have = true;
        t.position = p;
        t.samples.push_back({now, p});
        const float fl = std::sqrt(subject.facing[0] * subject.facing[0] + subject.facing[2] * subject.facing[2]);
        if (fl > 0.01f) { t.heading_x = subject.facing[0] / fl; t.heading_z = subject.facing[2] / fl; }
        t.ground = p[1];
        t.last_seen = now;
        operator_have_ = false;
        if (own_trail) {
            trail_.clear();
            history_.clear();
            aim_.have = false;
        }
        return;
    }
    t.last_seen = now;
    // A new sample from the game, or the same one again (its physics has not stepped since)?
    if (t.samples.empty() || length(sub(p, t.samples.back().second)) > 1e-5f) {
        if (!t.samples.empty()) {
            const double gap = now - t.samples.back().first;
            if (gap > 0.002 && gap < 0.1) t.interval = mix(static_cast<float>(t.interval), static_cast<float>(gap), 0.1f);
        }
        t.samples.push_back({now, p});
        while (t.samples.size() > 8) t.samples.pop_front();
    }
    // Where the game draws them now: between its last two samples, as its picture does.
    {
        const double when = now - t.interval;
        const auto &first = t.samples.front();
        if (when <= first.first) p = first.second;
        else {
            p = t.samples.back().second;
            for (std::size_t i = t.samples.size(); i-- > 1;) {
                const auto &a = t.samples[i - 1], &b = t.samples[i];
                if (a.first <= when && when <= b.first) {
                    const double span = b.first - a.first;
                    p = lerp(a.second, b.second, span > 1e-6 ? static_cast<float>((when - a.first) / span) : 1.0f);
                    break;
                }
            }
        }
    }
    const float step = std::max(dt, 0.001f);
    const Vec3 raw = scale(sub(p, t.position), 1.0f / step);
    t.velocity = lerp(t.velocity, raw, ease(dt, 0.07f));
    t.position = p;
    const float speed = flat_length(t.velocity);
    // The heading follows the travel a little behind, so the filmer swings round smoothly; slowly
    // towards the way the skater faces when they stop.
    // (Turned round the way the angle goes, so a reversal (fakie, turning round on the spot) swings
    // the heading round instead of leaving it stuck pointing the old way.)
    {
        float want_x = 0.0f, want_z = 0.0f, tau = 0.0f;
        if (speed > 0.8f) {
            want_x = t.velocity[0] / speed;
            want_z = t.velocity[2] / speed;
            // (Riding along, closely: the filmer turns as you turn, not a beat after.)
            tau = speed > 3.0f ? 0.1f : 0.3f;
        } else if (const float fl = std::sqrt(subject.facing[0] * subject.facing[0] + subject.facing[2] * subject.facing[2]); fl > 0.01f) {
            want_x = subject.facing[0] / fl;
            want_z = subject.facing[2] / fl;
            tau = 1.5f;
        }
        if (tau > 0.0f) {
            const float from = std::atan2(t.heading_x, t.heading_z);
            const float to = std::atan2(want_x, want_z);
            const float turned = from + wrap_angle(to - from) * ease(dt, tau);
            t.heading_x = std::sin(turned);
            t.heading_z = std::cos(turned);
        }
    }
    // In the air: told for your own skater; for others, rising fast or well above the last ground.
    bool air = false;
    if (subject.airborne) air = *subject.airborne;
    else air = (t.airborne && p[1] > t.ground + 0.15f) || t.velocity[1] > 2.2f || p[1] > t.ground + 0.45f;
    if (subject.bailing) air = false;
    if (air && !t.airborne) {
        t.air_since = now;
        if (own_trail) {
            // A flatground trick, for the lunge: popped off level ground (not a ramp or a bank: the last
            // metre of trail was flat, and the pop not too steep), towards level ground.
            bool flat = t.velocity[1] < 4.0f;
            float seen = 0.0f;
            Vec3 previous{p[0], t.ground, p[2]};
            for (auto it = trail_.rbegin(); it != trail_.rend() && seen < 1.2f; ++it) {
                if (std::abs(it->at[1] - t.ground) > 0.12f) flat = false;
                seen += flat_length(sub(it->at, previous));
                previous = it->at;
            }
            if (flat && cast_) {
                const float speed_now = flat_length(t.velocity);
                if (speed_now > 0.5f)
                    for (const float ahead : {1.0f, 2.0f}) {
                        const auto g = ground_at(p[0] + t.velocity[0] / speed_now * ahead, p[2] + t.velocity[2] / speed_now * ahead, t.ground);
                        if (!g || std::abs(*g - world(t.ground)) > 0.15f) flat = false;
                    }
            }
            flat_air_ = flat;
            takeoff_height_ = t.ground;
        }
    }
    // Not flatground after all: a big air, or going down a set.
    if (air && own_trail && (p[1] > takeoff_height_ + 1.0f || p[1] < takeoff_height_ - 0.3f)) flat_air_ = false;
    // Nobody is in the air for four seconds: the ground was guessed wrong (a ramp, a lift).
    if (air && now - t.air_since > 4.0) air = false;
    const bool landed = t.airborne && !air;
    t.airborne = air;
    // On the ground, the ground is where the skater is: a step (onto a ledge, off one) is a step, not a
    // slope, so it is not smoothed over; the root's bob is.
    if (!air) t.ground = landed || std::abs(p[1] - t.ground) > 0.2f ? p[1] : mix(t.ground, p[1], ease(dt, 0.15f));
    else if (p[1] < t.ground) t.ground = p[1]; // dropped below where they left: a drop-in or a stair set
    if (learn && !air) learn_root(dt, p);
    // The trail: where the skater has been, at their feet. That ground is known to be clear, so the
    // filmer rides it.
    if (own_trail) {
        const Vec3 foot{p[0], t.ground, p[2]};
        if (trail_.empty() || flat_length(sub(foot, trail_.back().at)) > 0.12f) {
            trail_.push_back({foot, t.airborne, t.ground, next_trail_id_++});
            while (trail_.size() > 600) trail_.pop_front();
        }
        if (t.airborne) landed_since_ = now;
    }
}

// The point `arc` metres back along the trail from the skater; `fallback` when it is shorter.
Vec3 Rig::trail_point(float arc, Vec3 fallback) const {
    if (trail_.size() < 2) return fallback;
    float left = arc;
    Vec3 previous = subject_.position;
    previous[1] = subject_.ground;
    for (auto it = trail_.rbegin(); it != trail_.rend(); ++it) {
        const Vec3 &at = it->at;
        const float piece = flat_length(sub(at, previous));
        if (piece >= left && piece > 0.0001f) return lerp(previous, at, left / piece);
        left -= piece;
        previous = at;
    }
    // Not that much trail yet: carry on straight back from its end.
    const Vec3 end = trail_.front().at;
    Vec3 back{-subject_.heading_x, 0, -subject_.heading_z};
    return add(end, scale(back, left));
}

std::optional<std::size_t> Rig::trail_index_near(float x, float z, float within) const {
    std::optional<std::size_t> best;
    float best_distance = within * within;
    for (std::size_t i = 0; i < trail_.size(); ++i) {
        const float dx = trail_[i].at[0] - x, dz = trail_[i].at[2] - z, d = dx * dx + dz * dz;
        if (d < best_distance) { best_distance = d; best = i; }
    }
    return best;
}

// The trail point `arc` metres back from the skater.
std::size_t Rig::trail_index_back(float arc) const {
    if (trail_.empty()) return 0;
    float left = arc;
    Vec3 previous{subject_.position[0], subject_.ground, subject_.position[2]};
    for (std::size_t i = trail_.size(); i-- > 0;) {
        left -= flat_length(sub(trail_[i].at, previous));
        previous = trail_[i].at;
        if (left <= 0.0f) return i;
    }
    return 0;
}

// Something you can ride on, under trail point `k` (a ledge, a manual pad, a planter's top), not a
// rail: the same top a little either side of your line.
bool Rig::wide_at(std::size_t k) const {
    if (!cast_ || k >= trail_.size()) return false;
    const Vec3 &here = trail_[k].at;
    const Vec3 &prev = trail_[k > 0 ? k - 1 : std::min(k + 1, trail_.size() - 1)].at;
    Vec3 along = sub(here, prev);
    const float l = flat_length(along);
    if (l < 0.01f) return false;
    const Vec3 across{-along[2] / l * 0.35f, 0.0f, along[0] / l * 0.35f};
    const float top = world(here[1]);
    for (const float side_sign : {1.0f, -1.0f}) {
        // (Asked whatever the budget: an answer a couple of seconds old is used again, so this is cheap.)
        ++probe_budget_;
        const auto g = ground_at(here[0] + across[0] * side_sign, here[2] + across[2] * side_sign, top + 0.3f);
        if (!g || std::abs(*g - top) > 0.15f) return false;
    }
    return true;
}

// Is the ground under a jump from `a` to `b` (trail points) a ramp or a bank a filmer would roll down
// after you (a smooth slope, not steeper than about 45 degrees), rather than a set, a drop or a gap?
// (In skate. you can leave the ground for a moment rolling over the lip of a bank or down a ramp.)
// Asked once per jump.
bool Rig::rideable_slope(std::size_t before, std::size_t landing) const {
    if (!cast_ || before >= trail_.size() || landing >= trail_.size()) return false;
    const std::uint64_t key = trail_[before].id;
    if (const auto found = slope_cache_.find(key); found != slope_cache_.end()) return found->second;
    const Vec3 &a = trail_[before].at, &b = trail_[landing].at;
    const float span = flat_length(sub(b, a));
    const float top = world(a[1]), bottom = world(b[1]);
    bool smooth = span > 0.3f && (top - bottom) / span < 1.0f;
    float previous_off = 0.0f;
    float wiggle = 0.0f;
    probe_budget_ += 9;
    for (int i = 1; smooth && i <= 8; ++i) {
        const float u = static_cast<float>(i) / 9.0f;
        const float line = top + (bottom - top) * u;
        const auto g = ground_at(a[0] + (b[0] - a[0]) * u, a[2] + (b[2] - a[2]) * u, line + 0.6f);
        if (!g) { smooth = false; break; }
        const float off = *g - line;
        // Well off the straight line from top to bottom: a drop, a set's lip, a gap.
        if (std::abs(off) > 0.3f) smooth = false;
        if (i > 1) wiggle += std::abs(off - previous_off);
        previous_off = off;
    }
    // Steps (a set of stairs) zig-zag about the line; a ramp follows it.
    if (smooth && wiggle / 7.0f > 0.06f) smooth = false;
    if (slope_cache_.size() > 256) slope_cache_.clear();
    slope_cache_[key] = smooth;
    return smooth;
}

// From trail point `from` (where the filmer is, at `level`) towards `to`. The filmer follows the
// ground you rolled on up and down banks and slopes, step by step. Where you went up onto something
// (a ledge, a rail) they stay down at their level and ride alongside. Where you came down from their
// level all at once (a set, a drop, off the end of a ledge they were not on) they stop at the top;
// and where you jumped a gap at the same level (the ground under the jump missing or far below) too.
// A jump you are still in the air for: they wait at the takeoff.
Rig::Walk Rig::walk(std::size_t from, std::size_t to, float level, bool stop_at_drops) const {
    Walk result{from, level, false, false};
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::size_t air_start = none;
    for (std::size_t k = from + 1; k <= to && k < trail_.size(); ++k) {
        const auto &point = trail_[k];
        if (point.air) {
            if (air_start == none) air_start = k;
            continue;
        }
        const float ground = point.at[1];
        const bool jumped = air_start != none;
        const std::size_t before = jumped ? (air_start > 0 ? air_start - 1 : 0) : k - 1;
        // Off it with an ollie (or a flip, or just off the end of it, in the air) and down further than
        // a filmer would huck after you: they stop at the top. Rolled down it (a ramp, a bank, however
        // fast and far): they roll down after you.
        // (Down a ramp or a bank you left the ground on for a moment: rolled down after you.)
        if (jumped && stop_at_drops && result.level - ground > huck_ && !rideable_slope(before, k)) {
            result.edge = true;
            result.reach = before;
            return result;
        }
        if (ground < result.level - 0.35f) {
            result.level = ground; // down after you: a curb, a ramp, a small set hucked
        } else if (jumped && std::abs(ground - result.level) <= 0.35f && cast_) {
            // Over a jump at the same level: is there ground under it (an ollie), or nothing (a gap)?
            const Vec3 &a = trail_[before].at;
            const float span = flat_length(sub(point.at, a));
            if (span > 1.2f) {
                for (const float u : {0.33f, 0.5f, 0.67f}) {
                    const auto g = ground_at(a[0] + (point.at[0] - a[0]) * u, a[2] + (point.at[2] - a[2]) * u, result.level);
                    if (!g || *g < world(a[1]) - huck_) { // well below where you took off: a gap
                        // (One they can ollie after you, landing level: they do.)
                        if (stop_at_drops && span > 5.0f) { result.edge = true; result.reach = before; return result; }
                        break;
                    }
                }
            }
            result.level = ground;
        } else if (std::abs(ground - result.level) <= 0.35f) {
            result.level = ground; // rolling along, up and down banks and slopes
        } else if (jumped && ground - result.level <= join_height && wide_at(k)) {
            result.level = ground; // ollied up onto a ledge or a manual pad: they ollie up after you
        }
        // (Higher up otherwise, after a jump: a rail, or too high. The filmer stays at their level.)
        air_start = none;
        result.reach = k;
    }
    // Still in the air at the end: if there is ground under the jump so far, not far below where you
    // took off (an ollie on flat, on a slope, up onto something), they ride on; over a gap or off a
    // drop too high to huck (or with no way to tell) they ease off to wait at the takeoff until you land.
    if (air_start != none) {
        bool level_ground = static_cast<bool>(cast_);
        const float takeoff = world(trail_[air_start > 0 ? air_start - 1 : 0].at[1]);
        for (std::size_t k = air_start; level_ground && k <= to && k < trail_.size(); k += 3) {
            const auto g = ground_at(trail_[k].at[0], trail_[k].at[2], trail_[k].at[1]);
            if (!g || *g < takeoff - huck_) level_ground = false;
        }
        // Over a ramp going down (smooth under you so far): they roll on after you.
        if (!level_ground && cast_) {
            const std::size_t last = std::min(to, trail_.size() - 1);
            if (last > air_start && rideable_slope(air_start > 0 ? air_start - 1 : 0, last)) level_ground = true;
        }
        if (!level_ground) {
            result.hold = true;
            result.reach = air_start > 0 ? air_start - 1 : 0;
        } else {
            result.reach = std::min(to, trail_.size() - 1);
        }
    }
    return result;
}

// Auto: a filmer picks angles. Mostly a little off to one side behind, now and then wide, rarely out
// in front; never for less than a few seconds.
void Rig::choose_angle(double now, float speed, std::uint8_t angles) {
    if (now < angle_until_) return;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float draw = unit(random_);
    const float side = angle_target_ >= 0 ? 1.0f : -1.0f;
    float keep_side = unit(random_) < 0.65f ? side : -side;
    if (side_lock_ != 0.0f) keep_side = side_lock_;
    // Among the angles chosen (all, if none): behind most, then out to the side, now and then ahead
    // (only riding fast enough to stay in front; slower, ahead falls back to behind if it can).
    if ((angles & 7u) == 0) angles = 7;
    float weight_behind = (angles & angle_behind) ? 0.5f : 0.0f, weight_side = (angles & angle_side) ? 0.35f : 0.0f,
          weight_ahead = (angles & angle_ahead) && (speed > 3.5f || angles == angle_ahead) ? 0.15f : 0.0f;
    if (weight_behind + weight_side + weight_ahead <= 0.0f) weight_behind = 1.0f;
    const float roll = draw * (weight_behind + weight_side + weight_ahead);
    float pick;
    double hold;
    if (roll < weight_behind) { pick = (26.0f + 18.0f * unit(random_)) * keep_side; hold = 5.0 + 6.0 * unit(random_); }
    else if (roll < weight_behind + weight_side) { pick = (55.0f + 25.0f * unit(random_)) * keep_side; hold = 3.5 + 4.0 * unit(random_); }
    else { pick = (160.0f + 15.0f * unit(random_)) * keep_side; hold = 3.0 + 2.5 * unit(random_); }
    angle_target_ = pick * degrees;
    angle_until_ = now + hold;
}

// Where in the skater the filmer points: low on the body with the fisheye (feet and board in frame),
// a little ahead of where they are going; higher and further ahead through an air.
// Where the filmer is looking: the skater's feet, a little ahead of where they are going (further ahead
// through an air). The height of the frame comes from frame_pitch.
Vec3 Rig::aim_point(const Tracked &t, const RigSettings &) const {
    const float lead = t.airborne ? 0.16f : 0.12f;
    Vec3 point = t.position;
    point[1] -= root_above_ground_; // the board, not the root
    point[0] += t.velocity[0] * lead;
    point[2] += t.velocity[2] * lead;
    if (t.airborne) point[1] += std::clamp(t.velocity[1] * 0.08f, -0.4f, 0.4f);
    return point;
}

// Where the aim point was `delay` seconds ago: a person sees the move, then turns.
Vec3 Rig::delayed_point(double now, float delay) const {
    if (history_.empty()) return {};
    const double when = now - delay;
    for (std::size_t i = history_.size(); i-- > 0;) {
        if (history_[i].time <= when) {
            if (i + 1 < history_.size()) {
                const auto &a = history_[i], &b = history_[i + 1];
                const double span = b.time - a.time;
                const float t = span > 1e-6 ? static_cast<float>((when - a.time) / span) : 0.0f;
                return lerp(a.point, b.point, std::clamp(t, 0.0f, 1.0f));
            }
            return history_[i].point;
        }
    }
    return history_.front().point;
}

void Rig::steer_aim(float dt, float want_yaw, float want_pitch, float omega, float damping, float yaw_rate, float pitch_rate) {
    if (!aim_.have) {
        aim_ = {want_yaw, want_pitch, 0, 0, true};
        return;
    }
    angle_spring(aim_.yaw, aim_.yaw_rate, want_yaw, omega, damping, dt, yaw_rate);
    spring(aim_.pitch, aim_.pitch_rate, std::clamp(want_pitch, -1.35f, 1.35f), pitch_rate, omega, damping, dt);
    aim_.pitch = std::clamp(aim_.pitch, -1.4f, 1.4f);
}

// The camera from its position and angles. Right, up and back are built as the game's own follow
// camera builds them (Extension/Multiplayer/Hud/follow_camera.cpp), then turned about the view
// by `roll` (positive drops the right side).
Matrix Rig::build(const Vec3 &eye, float yaw, float pitch, float roll) const {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const Vec3 forward{std::sin(yaw) * cp, sp, std::cos(yaw) * cp};
    const Vec3 back = scale(forward, -1.0f);
    Vec3 right{back[2], 0, -back[0]};
    const float fl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (fl > 0.0001f) { right[0] /= fl; right[2] /= fl; } else right = {1, 0, 0};
    Vec3 up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0]};
    const float cr = std::cos(roll), sr = std::sin(roll);
    const Vec3 rolled_right = sub(scale(right, cr), scale(up, sr));
    const Vec3 rolled_up = add(scale(up, cr), scale(right, sr));
    return {rolled_right[0], rolled_right[1], rolled_right[2], 0, rolled_up[0], rolled_up[1], rolled_up[2], 0,
            back[0], back[1], back[2], 0, eye[0], eye[1], eye[2], 1};
}

// Handheld: the body swaying on the board, the hands, the board rattling over the ground faster the
// faster it rolls, and a hand knocking the lens.
void Rig::shake(double now, float speed, float human, float &yaw, float &pitch, float &roll, Vec3 &eye) const {
    const float h = std::clamp(human, 0.0f, 1.0f);
    const float rattle = std::clamp(speed / 8.0f, 0.0f, 1.4f) * (0.25f + 0.75f * h);
    yaw += (wobble_[0].sample(now) * 1.3f * h + wobble_[3].sample(now) * 0.45f * h + wobble_[6].sample(now) * 0.22f * rattle) * degrees;
    pitch += (wobble_[1].sample(now) * 0.9f * h + wobble_[4].sample(now) * 0.4f * h + wobble_[7].sample(now) * 0.3f * rattle) * degrees;
    roll += (wobble_[2].sample(now) * 1.6f * h + wobble_[5].sample(now) * 0.5f * h + wobble_[8].sample(now) * 0.25f * rattle) * degrees;
    eye[1] += wobble_[7].sample(now * 0.5) * 0.008f * rattle + wobble_[1].sample(now) * 0.02f * h;
    // The smack: the palm hits the glass 0.1 s after the slap starts and knocks the camera back hard,
    // it shakes while the hand is held there, and recovers as the hand comes off.
    // The pop of an ollie: a jolt up through the board (the tail snapped), then the catch.
    const double popped = now - ollie_pop_at_;
    if (popped >= 0 && popped < 0.5) {
        const float t = static_cast<float>(popped);
        const float kick = (1.0f - std::exp(-t * 80.0f)) * std::exp(-t * 12.0f);
        pitch -= kick * 3.5f * degrees;
        roll += std::sin(t * 40.0f) * std::exp(-t * 14.0f) * 1.2f * degrees;
        eye[1] += kick * 0.05f;
        const double landed = now - ollie_until_;
        if (landed >= 0 && landed < 0.3) eye[1] -= static_cast<float>(std::exp(-landed * 20.0) * std::sin(landed * 30.0)) * 0.04f;
    }
    const double since = now - slap_at_ - slap_contact;
    if (since >= 0 && since < 0.6) {
        const float t = static_cast<float>(since);
        const float kick = (1.0f - std::exp(-t * 60.0f)) * std::exp(-t * 7.0f);
        const float buzz = std::sin(t * 70.0f) * std::exp(-t * 10.0f);
        yaw += slap_yaw_ * (kick * 5.0f + buzz * 0.8f) * degrees;
        pitch += slap_pitch_ * (kick * 6.0f + buzz * 0.6f) * degrees;
        roll += slap_yaw_ * kick * 4.0f * degrees;
        eye[1] -= kick * 0.04f;
    }
}

// The way planner. A filmer riding behind someone sees what is coming: the bench on their side, the
// tree on the corner, the wall the line runs along, the drop off the side of the path. A few times a
// second the spots the filmer could take round the skater (the side it keeps to, a quarter round,
// straight behind, the other side, and each closer in or further out) are scored where the skater is
// now and where they will be over the next second and a bit, and on the way the filmer would ride
// between them, a body's width wide:
//   no ground level with the skater's there (a drop beside the line, a gap, something raised),
//   a wall between the spot and the skater (no footy, and the lens behind it),
//   anything in the way of riding there (a trunk, a post, a corner).
// The side it keeps to costs least only while it is clear: with something coming up there, the
// clearest side and distance win, and that choice is held a few seconds once past it (a filmer
// carving back over, not snapping back). With nowhere clear at all (wedged between walls) they wait
// where they are for you to come past (`trapped`). Never in front of you.
// One spot round you scored (see plan): `a` the angle, `scale_by` the distance's scale.
float Rig::plan_score(float a, float scale_by) {
    const auto &t = subject_;
    const auto &p = plan_job_;
    const Vec3 heading{t.heading_x, 0.0f, t.heading_z};
    const Vec3 side{t.heading_z, 0.0f, -t.heading_x};
    float cost = std::abs(a - p.want) / degrees * 0.012f + std::abs(scale_by - 1.0f) * 0.8f;
    // The other side from the one they are on: a ride across behind you, so only for a reason.
    // (Just up on something, the camera raised with it: they stay on their side, settled.)
    if (std::abs(plan_angle_) > 5.0f * degrees && a * plan_angle_ < 0.0f) cost += now_ < settled_until_ ? 2.5f : 0.3f;
    // Told which side by you (the side buttons): never the other.
    if (side_hard_ != 0.0f && a * side_hard_ < -1.0f * degrees) return 1e6f;
    const float dist = p.d * scale_by;
    const float level = operator_floor_;
    Vec3 last{operator_[0], level, operator_[2]};
    const std::array<float, 4> ahead{0.0f, 0.5f, 1.0f, 1.6f};
    for (std::size_t i = 0; i < p.samples; ++i) {
        const float weight = i == 0 ? 1.0f : i == 1 ? 0.85f : i == 2 ? 0.7f : 0.55f;
        const float along = p.flat_speed * ahead[i];
        const Vec3 at{t.position[0] + p.way[0] * along, t.position[1], t.position[2] + p.way[2] * along};
        const Vec3 line = add(at, scale(heading, -dist * std::cos(a)));
        const Vec3 spot = add(line, scale(side, dist * std::sin(a)));
        float ground = p.reference[i];
        if (p.grinding) {
            // Beside a rail or a ledge you grind (down a hubba, along a flat bar): any ground below
            // you there that a filmer can stand on, down the stairs beside it too; not up on what
            // you grind, and not off a real drop.
            const float feet = p.feet[i];
            const auto g = ground_at(spot[0], spot[2], feet + 1.2f);
            if (std::abs(std::sin(a)) * dist < 0.9f) cost += 2.0f * weight;     // on your line: on what you grind
            else if (!g) cost += 2.5f * weight;
            else if (*g > feet + 1.0f) cost += 2.0f * weight;                // well up above you
            else if (feet - *g > 2.6f) cost += 3.0f * weight;                // off a real drop
            else {
                ground = *g;
                if (*g > feet - 0.1f) cost += 0.6f * weight;                  // up level with you: the low side is better
            }
        } else {
            // Judged against the ground on your line level with the spot (on a hill the spots behind
            // you are higher, and that is no ledge).
            float ref = p.reference[i];
            if (!p.up && std::abs(std::sin(a)) > 0.05f)
                if (const auto gl = ground_at(line[0], line[2], p.reference[i] + 1.5f)) ref = *gl;
            if (!p.up && std::abs(std::sin(a)) <= 0.05f) ref = ground_at(line[0], line[2], p.reference[i] + 1.5f).value_or(p.reference[i]);
            const auto g = ground_at(spot[0], spot[2], ref + 0.5f);
            ground = ref;
            if (!g) cost += 2.5f * weight;                    // nothing under it: off an edge, a gap
            else if (*g < ref - 0.6f) cost += 3.0f * weight;  // a drop beside the line
            else if (*g > ref + 0.3f) cost += 2.0f * weight;  // up on something (a planter, a ledge)
            else ground = *g;
        }
        const Vec3 eye{spot[0], ground + p.height + 0.1f, spot[2]};
        const Vec3 centre{at[0], (p.up || p.grinding ? world(at[1]) : p.reference[i]) + 0.9f, at[2]};
        if (obstructed(centre, eye, 0.25f, true)) cost += 2.5f * weight; // a wall between: no footy
        // Elbow room: a wall, a bin or a post right beside the spot (within 0.7 m either side) is
        // a spot to keep off, before they are brushing past it.
        if (i <= 2) {
            for (const float side_sign : {1.0f, -1.0f}) {
                const Vec3 from{spot[0], ground + 0.3f, spot[2]};
                const Vec3 to{spot[0] + side[0] * 0.7f * side_sign, ground + 0.3f, spot[2] + side[2] * 0.7f * side_sign};
                if (obstructed(from, to, 0.3f, true)) cost += 0.8f * weight;
            }
        }
        // The way there, a body wide (a trunk, a post, a corner): either shoulder, low (a planter, a kerb-high box).
        const Vec3 move{spot[0] - last[0], 0.0f, spot[2] - last[2]};
        const float length_moved = flat_length(move);
        if (length_moved > 0.05f) {
            const Vec3 across{-move[2] / length_moved * 0.3f, 0.0f, move[0] / length_moved * 0.3f};
            for (const float shoulder : {1.0f, -1.0f}) {
                const Vec3 from{last[0] + across[0] * shoulder, last[1] + 0.25f, last[2] + across[2] * shoulder};
                const Vec3 to{spot[0] + across[0] * shoulder, ground + 0.25f, spot[2] + across[2] * shoulder};
                if (obstructed(from, to, 0.25f, true)) cost += 0.9f * weight;
            }
        }
        last = Vec3{spot[0], ground, spot[2]};
    }
    return cost;
}

// The way planner (see above). Its questions to the world are spread over a few ticks (one spot a
// tick), never asked all in one: on a slow machine a burst of them in one frame is a hitch.
float Rig::plan(double now, float want, float d, float height, float speed, bool &trapped) {
    trapped = halt_ == Halt::wait && clear_since_ < 0.0;
    if (!cast_ || !world_answers() || !operator_have_) {
        plan_angle_ = want;
        plan_scale_ = 1.0f;
        plan_risk_ = 0.0f;
        plan_job_.stage = 0;
        return want;
    }
    auto &p = plan_job_;
    const auto &t = subject_;
    if (p.stage == 0) {
        if (now < plan_next_) return plan_angle_;
        plan_next_ = now + (grinding_ ? 0.05 : 0.15);
        // A new look: what is wanted, and the ground where you will be over the next second and a
        // half (a wall or a bin coming up on their side is seen in time to move over).
        p = PlanJob{};
        p.stage = 1;
        p.want = want;
        p.d = d;
        p.height = height;
        p.flat_speed = std::min(speed, 12.0f);
        const Vec3 heading{t.heading_x, 0.0f, t.heading_z};
        p.way = p.flat_speed > 0.5f ? Vec3{t.velocity[0] / std::max(speed, 0.01f), 0.0f, t.velocity[2] / std::max(speed, 0.01f)} : heading;
        p.samples = p.flat_speed > 0.5f ? 4 : 1;
        // Where the skater is up on something (a ledge, a rail), the spots are judged against the
        // filmer's own level, or, grinding, against the ground below you there; otherwise against
        // the ground under the skater (riding down a bank or a ramp, the spots beside them are lower
        // too, and that is no drop).
        p.grinding = grinding_;
        p.up = !p.grinding && up_beside_ > 0.3f && t.ground > level_ + 0.3f && !joining_;
        const std::array<float, 4> ahead{0.0f, 0.5f, 1.0f, 1.6f};
        probe_budget_ += 8;
        for (std::size_t i = 0; i < p.samples; ++i) {
            const float along = p.flat_speed * ahead[i];
            const float x = t.position[0] + p.way[0] * along, z = t.position[2] + p.way[2] * along;
            p.reference[i] = operator_floor_;
            p.feet[i] = world(t.ground) + t.velocity[1] * ahead[i];
            if (!p.up && !p.grinding)
                if (const auto g = ground_at(x, z, world(t.ground) + 0.5f + std::clamp(t.velocity[1], -6.0f, 6.0f) * ahead[i])) p.reference[i] = *g; // (where you will be, up or down a hill)
        }
        const float sign = want > 0.0f ? 1.0f : want < 0.0f ? -1.0f : (plan_angle_ >= 0.0f ? 1.0f : -1.0f);
        const float quarter = std::min(std::abs(want) * 0.6f, 22.0f * degrees) * sign;
        const float candidates[] = {want, quarter, 0.0f, -quarter, -want, plan_angle_};
        // (Grinding: beside you, this side or that; quick to decide, there is not long.)
        const float grind_candidates[] = {want, -want};
        for (const float a : p.grinding ? std::span<const float>(grind_candidates) : std::span<const float>(candidates)) {
            bool repeat = false;
            for (std::size_t k = 0; k < p.count; ++k) if (std::abs(p.angles[k] - a) < 2.0f * degrees) repeat = true;
            if (!repeat) { p.angles[p.count] = a; p.scales[p.count] = 1.0f; ++p.count; }
        }
        p.next = 0;
        p.want_cost = 0.0f;
        p.current_cost = 1e9f;
        return plan_angle_;
    }
    // One spot this tick.
    probe_budget_ += 12;
    for (int n = 0; n < 1 && p.next < p.count; ++n, ++p.next) {
        const float a = p.angles[p.next], scale_by = p.scales[p.next];
        const float cost = plan_score(a, scale_by) + (p.next >= p.side_from ? 0.2f : 0.0f);
        if (p.next == 0) p.want_cost = cost;
        if (std::abs(a - plan_angle_) < 2.0f * degrees && std::abs(plan_scale_ - scale_by) < 0.05f) p.current_cost = cost;
        if (cost < p.best.cost) { if (scale_by == 1.0f) p.second = p.best; p.best = {a, scale_by, cost}; }
        else if (cost < p.second.cost && scale_by == 1.0f) p.second = {a, scale_by, cost};
    }
    if (p.next < p.count) return plan_angle_;
    // Then, nothing quite clear at the usual distance: closer in, or further out, at the best two
    // angles. Then, nothing behind you any good (a wall at your back, the top of a ramp): out to
    // your side, the side with the room.
    if (p.stage == 1) {
        p.stage = 2;
        if (p.best.cost > 0.6f && !p.grinding) {
            for (const auto &option : {p.best, p.second}) {
                if (option.cost >= 1e8f) continue;
                for (const float scale_by : {0.7f, 1.4f}) { p.angles[p.count] = option.angle; p.scales[p.count] = scale_by; ++p.count; }
            }
            if (p.next < p.count) return plan_angle_;
        }
    }
    if (p.stage == 2) {
        p.stage = 3;
        if (p.best.cost > 1.0f && !p.grinding) {
            p.side_from = p.count;
            for (const float a : {80.0f * degrees, -80.0f * degrees}) { p.angles[p.count] = a; p.scales[p.count] = 1.0f; ++p.count; }
            return plan_angle_;
        }
    }
    // Decided. Kept while it is clear (no dithering); a choice made to get round something is held
    // a few seconds, so they carve back over after it, not snap back.
    p.stage = 0;
    const auto best = p.best;
    const bool keep = now < plan_keep_ && p.current_cost < 0.6f && !(p.grinding && std::abs(plan_angle_ - p.want) > 20.0f * degrees);
    if (!keep) {
        const bool evasive = p.want_cost - best.cost > 0.5f;
        if (std::abs(best.angle - plan_angle_) > 1.0f * degrees || std::abs(best.scale_by - plan_scale_) > 0.05f)
            plan_keep_ = now + (evasive ? 2.5 : 0.8);
        plan_angle_ = best.angle;
        plan_scale_ = best.scale_by;
    }
    plan_risk_ = std::max(0.0f, p.want_cost - std::min(best.cost, p.current_cost));
    // Wedged: every spot is bad. Room again once something is clear for a moment.
    if (best.cost > 4.5f) clear_since_ = -1.0;
    else if (clear_since_ < 0.0 && best.cost < 1.5f) clear_since_ = now;
    trapped = best.cost > 4.5f || (halt_ == Halt::wait && (clear_since_ < 0.0 || now - clear_since_ < 0.4));
    plan_trapped_ = best.cost > 4.5f;
    return plan_angle_;
}

// You standing still: a spot near you, behind (left or right of your back, straight behind when
// those are no good), with level ground, a clear view of you and a clear way there. The nearest of
// those to where the filmer is: they roll over and stop, they do not circle round you.
// Whether you were in the air over (or right by) a spot: your trail there left the ground.
// (Going the same way over it as they would: over it along your line, not across a ledge you grind.)
bool Rig::ollied_over(const Vec3 &at, const Vec3 &way, float up_to) const {
    for (std::size_t k = 1; k < trail_.size(); ++k) {
        const auto &point = trail_[k];
        if (!point.air) continue;
        const float dx = point.at[0] - at[0], dz = point.at[2] - at[2];
        if (dx * dx + dz * dz >= 1.5f * 1.5f) continue;
        const Vec3 along = sub(point.at, trail_[k - 1].at);
        const float l = flat_length(along);
        if (l <= 1e-3f || (along[0] * way[0] + along[2] * way[2]) / l <= 0.6f) continue;
        // Over it and down again about where you took off (not up onto it: a ledge you went on to grind).
        std::size_t before = k;
        while (before > 0 && trail_[before].air) --before;
        std::size_t after = k;
        while (after < trail_.size() && trail_[after].air) ++after;
        if (after >= trail_.size()) return false; // still in the air: not yet known
        const float rise = trail_[after].at[1] - trail_[before].at[1];
        if (rise > -0.3f && rise < std::max(0.3f, up_to)) return true;
    }
    return false;
}

bool Rig::rest_spot_ok(const Vec3 &spot, float, float height) const {
    const auto &t = subject_;
    const float level = operator_have_ ? operator_floor_ : world(t.ground);
    const auto g = ground_at(spot[0], spot[2], level + 0.5f);
    if (!g || *g < level - 0.85f || *g > level + 0.3f) return false;
    const Vec3 centre{t.position[0], world(t.position[1]) + 0.9f, t.position[2]};
    return !blocked(centre, Vec3{spot[0], *g + height + 0.1f, spot[2]});
}

bool Rig::choose_rest(float d, float height) {
    const auto &t = subject_;
    const Vec3 heading{t.heading_x, 0.0f, t.heading_z};
    const Vec3 side{t.heading_z, 0.0f, -t.heading_x};
    const float current = operator_have_ ? ((operator_[0] - t.position[0]) * side[0] + (operator_[2] - t.position[2]) * side[2] >= 0.0f ? 1.0f : -1.0f)
                                         : (side_lock_ != 0.0f ? side_lock_ : 1.0f);
    const float first = side_lock_ != 0.0f ? side_lock_ : current;
    const float near = std::clamp(d, 1.2f, 8.0f);
    struct Option { float angle, distance; };
    const Option options[] = {{30.0f * first, near}, {50.0f * first, near}, {30.0f * -first, near}, {50.0f * -first, near},
                              {0.0f, near}, {30.0f * first, near * 0.7f}, {30.0f * -first, near * 0.7f}, {0.0f, near * 0.7f},
                              {85.0f * first, near}, {85.0f * -first, near}, {85.0f * first, near * 0.7f}, {85.0f * -first, near * 0.7f}};
    probe_budget_ += 18;
    ray_budget_ += 40;
    // The room either side of you (to a wall, a rail, a drop's edge is not seen: just what is in the
    // way), for when it has to be your side: the side with more of it.
    float room[2] = {4.0f, 4.0f};
    if (cast_) {
        const float level = operator_have_ ? operator_floor_ : world(t.ground);
        for (int k = 0; k < 2; ++k) {
            const float sgn = k == 0 ? 1.0f : -1.0f;
            const Vec3 from{t.position[0], world(t.ground) + 0.9f, t.position[2]};
            const Vec3 to = add(from, scale(side, 4.0f * sgn));
            Vec3 hit{};
            if (blocked(from, to, &hit)) room[k] = flat_length(sub(hit, from));
            // Nothing to stand on out there either: no room.
            for (float out = 1.0f; out <= room[k]; out += 1.0f) {
                const auto g = ground_at(t.position[0] + side[0] * out * sgn, t.position[2] + side[2] * out * sgn, level + 0.5f);
                if (!g || *g < level - 0.85f) { room[k] = out - 0.5f; break; }
            }
        }
    }
    float best_score = 1e9f;
    bool found = false;
    for (const auto &o : options) {
        const float a = o.angle * degrees;
        const Vec3 spot = add(Vec3{t.position[0], 0.0f, t.position[2]}, add(scale(heading, -o.distance * std::cos(a)), scale(side, o.distance * std::sin(a))));
        if (cast_ && !rest_spot_ok(spot, d, height)) continue;
        // The nearest good one to where the filmer is (and on the side they are on).
        float score = operator_have_ ? flat_length(sub(spot, operator_)) : 0.0f;
        // On the side they keep to (yours to choose: never the other), else the side they are on.
        if (side_hard_ != 0.0f && o.angle * side_hard_ < 0.0f) continue;
        if (o.angle * (side_lock_ != 0.0f ? side_lock_ : current) < 0.0f) score += side_lock_ != 0.0f ? 4.0f : 1.5f;
        if (o.angle == 0.0f && side_lock_ != 0.0f) score += 2.0f; // straight behind only if their side is no good
        // Out to your side only when behind you is no good, on the side with the room.
        if (std::abs(o.angle) > 60.0f) score += 3.0f + std::max(0.0f, 4.0f - room[o.angle > 0.0f ? 0 : 1]) * 0.6f;
        if (cast_ && operator_have_) {
            const Vec3 from{operator_[0], operator_floor_ + 0.45f, operator_[2]};
            if (blocked(from, Vec3{spot[0], operator_floor_ + 0.45f, spot[2]})) score += 4.0f;
        }
        if (score < best_score) {
            best_score = score;
            rest_spot_ = spot;
            found = true;
        }
    }
    if (!found) {
        // Nothing clear round you (a tight spot): beside you on the side with more room, close.
        const float sgn = side_hard_ != 0.0f ? side_hard_ : (room[0] >= room[1] ? 1.0f : -1.0f);
        const float out = std::clamp(room[sgn > 0.0f ? 0 : 1] - 0.5f, 0.8f, near * 0.7f);
        rest_spot_ = add(Vec3{t.position[0], 0.0f, t.position[2]}, add(scale(side, out * sgn), scale(heading, -0.3f)));
    }
    rest_from_ = t.position;
    rest_have_ = true;
    rest_bad_since_ = -1.0;
    return found;
}

// Stopping by themselves: rolling on along the way they were riding and braking gently to a halt
// (about 2.5 m/s/s, harder only if there is not the room), short of anything ahead that drops away.
void Rig::begin_halt(Halt why, double now, float room) {
    if (halt_ == why) {
        // Already stopping: a drop seen closer than where they meant to stop moves the stop point in.
        const float remaining = (halt_point_[0] - operator_[0]) * halt_dir_[0] + (halt_point_[2] - operator_[2]) * halt_dir_[2];
        if (room < remaining) halt_point_ = add(operator_, scale(halt_dir_, std::max(room, 0.0f)));
        return;
    }
    const bool rolling = halt_ != Halt::none;
    halt_ = why;
    halt_since_ = now;
    halt_zoom_rolled_ = false;
    halt_for_grind_ = halt_grind_zoom_rolled_ = false;
    drifting_ = false;
    drift_done_ = 0.0f;
    bail_over_since_ = -1.0;
    if (rolling) return; // already braking: carry on to the same stop
    const float v = flat_length(operator_velocity_);
    halt_speed_ = v;
    halt_stopped_ = v < 0.05f;
    halt_dir_ = v > 0.05f ? Vec3{operator_velocity_[0] / v, 0.0f, operator_velocity_[2] / v} : Vec3{subject_.heading_x, 0.0f, subject_.heading_z};
    // You slammed: they carve off to the side as they slow, never on towards you (lying there): only
    // the way that does not close on you is kept, at most of their speed, braking gently to a halt.
    if (why == Halt::slam && v > 0.05f) {
        Vec3 to_you{subject_.position[0] - operator_[0], 0.0f, subject_.position[2] - operator_[2]};
        const float far = flat_length(to_you);
        if (far > 0.1f) {
            to_you = scale(to_you, 1.0f / far);
            const float closing = halt_dir_[0] * to_you[0] + halt_dir_[2] * to_you[2];
            if (closing > 0.0f) {
                const Vec3 aside{halt_dir_[0] - to_you[0] * closing, 0.0f, halt_dir_[2] - to_you[2] * closing};
                const float al = flat_length(aside);
                // (Head on: off to their own side of you.)
                const Vec3 side_way = al > 0.15f ? scale(aside, 1.0f / al) : Vec3{-to_you[2], 0.0f, to_you[0]};
                halt_dir_ = side_way;
                halt_speed_ = v * std::max(al, 0.6f);
            }
        }
        room = std::max(room, halt_speed_ * halt_speed_ / (2.0f * 2.5f));
    }
    // Never rolling on past you (you slammed, you stopped in front of them): short of you, by a body.
    {
        const float to_you = (subject_.position[0] - operator_[0]) * halt_dir_[0] + (subject_.position[2] - operator_[2]) * halt_dir_[2];
        const float across = std::abs((subject_.position[0] - operator_[0]) * halt_dir_[2] - (subject_.position[2] - operator_[2]) * halt_dir_[0]);
        if (to_you > 0.0f && across < 2.0f) room = std::min(room, std::max(0.0f, to_you - 1.4f));
    }
    // (Short of room, they may use some of the margin rather than stop dead.)
    const float vs = halt_speed_;
    float reach = std::max(std::min(vs * vs / (2.0f * 2.5f), std::max(room, 0.0f)), std::min(vs * vs / (2.0f * 6.0f), std::max(room + 0.4f, 0.0f)));
    if (cast_ && reach > 0.2f) {
        probe_budget_ += 12;
        for (float step = 0.4f; step <= reach + 0.8f; step += 0.4f) {
            const Vec3 at = add(operator_, scale(halt_dir_, step));
            const auto g = ground_at(at[0], at[2], operator_floor_ + 0.5f);
            if (!g || *g < operator_floor_ - 0.5f) {
                reach = std::min(reach, std::max(0.1f, step - 0.6f));
                break;
            }
        }
    }
    halt_point_ = add(operator_, scale(halt_dir_, reach));
}

void Rig::resume_behind() noexcept {
    // A cut: the next tick places them straight in behind you.
    operator_have_ = false;
    halt_ = Halt::none;
    post_ = Post::none;
    planted_ = edge_ = holding_ = false;
    caution_ = false;
    auto_zoom_ = zoom_ = 1.0f; // a cut: the new shot starts wide
    zoom_kind_ = 0;
    auto_zooming_ = false;
}

std::string Rig::doing() const {
    if (!operator_have_) return "placing the filmer";
    if (planted_) return "stopped by you (left stick to ride on)";
    switch (halt_) {
    case Halt::drop: return "stopped at the top: too high to huck (push, step off or left stick to carry on)";
    case Halt::lag: return "too far behind: stopped (push, step off or left stick to carry on)";
    case Halt::slam: return "you slammed: stopped, filming you";
    case Halt::wait: return "no room to ride: waiting for you to come past";
    case Halt::none: break;
    }
    if (now_ < ollie_until_) return "ollying over it after you";
    if (ollie_planned_) return "lining up to ollie over it after you";
    if (post_ == Post::rail_end) return "racing to the end of the ledge for a low angle";
    if (post_ == Post::ledge) return "behind the ledge, camera up over it";
    if (joining_) return operator_floor_ < join_to_ - 0.2f ? "ollying up onto the ledge after you" : "up on the ledge with you";
    if (grinding_) return "beside you through the grind";
    if (rest_have_) return "you are still: waiting behind you";
    if (caution_) return "easing off: a drop ahead";
    if (wall_raise_ > 0.2f) return "camera raised to see you over something";
    return "riding with you";
}

Frame Rig::follow(double now, float dt, const Subject &subject, const RigSettings &s) {
    if (!subject.valid || !finite(subject.position)) return {};
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    filming_ = framing_ = false;
    slam_zoom_ = s.slam_zoom;
    stop_zoom_ = s.stop_zoom;
    huck_ = drop_height_for(s.drop_strictness);
    drop_airtime_ = drop_airtime_for(s.drop_strictness);
    ff_have_ = false;
    track(now, dt, subject, subject_, true, true);
    ease_zoom(dt, s);
    const auto &t = subject_;
    const float speed = flat_length(t.velocity);
    const float human = std::clamp(s.human, 0.0f, 1.0f), reaction = std::clamp(s.reaction, 0.0f, 1.0f);
    const bool fish = s.lens == Lens::fisheye;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    float distance = std::clamp(s.distance, 0.6f, 6.0f) * (fish ? 1.0f : 3.2f);
    // Never right on top of you: the least gap between the filmer and you while you skate.
    const float min_gap = fish ? std::clamp(distance * 0.8f, 1.0f, 1.4f) : 2.5f;
    // You pushing (your speed rising on the flat), or stepping off the board: what sends a filmer who
    // stopped by themselves on again.
    {
        const bool foot = subject.on_foot.value_or(false);
        pushed_ = !t.airborne && !subject.bailing && t.velocity[1] > -0.4f && speed > 1.2f && speed - speed_slow_ > 0.6f;
        if (foot && !was_on_foot_) pushed_ = true;
        was_on_foot_ = foot;
        on_foot_ = foot;
        speed_slow_ += (speed - speed_slow_) * ease(dt, 0.6f);
        if (dt > 0.0f) skater_accel_ += ((speed - last_speed_) / dt - skater_accel_) * ease(dt, 0.15f);
        last_speed_ = speed;
        if (speed < speed_slow_) speed_slow_ = speed; // slowing down never builds up a "push"
    }
    // The lunge: with the fisheye, on a flatground trick the filmer pushes in closer and lower under
    // it, and eases back out as you land and ride away.
    const bool lunging = s.lunge && fish && t.airborne && flat_air_ && !planted_ && !edge_ && !holding_ && halt_ == Halt::none;
    spring(lunge_, lunge_rate_, lunging ? 1.0f : 0.0f, 0.0f, lunging ? 6.0f : 2.5f, 1.0f, dt);
    lunge_ = std::clamp(lunge_, 0.0f, 1.0f);
    // Pull back a little at speed and in the air, so a trick stays in the frame (unless lunging in).
    distance += std::min(speed * 0.04f, 0.5f);
    if (t.airborne) distance += (std::clamp(t.position[1] - t.ground, 0.0f, 3.0f) * 0.35f + 0.3f) * (1.0f - lunge_);
    distance = std::max(distance * mix(1.0f, 0.65f, lunge_), std::min(distance, min_gap));
    // The side they keep to.
    side_lock_ = s.side == Side::left ? 1.0f : s.side == Side::right ? -1.0f : 0.0f;
    side_hard_ = s.side_hard && !grinding_ ? side_lock_ : 0.0f; // (on a grind, the best side of it)
    if (side_lock_ != last_side_lock_) {
        // Sent to the other side: they start over at once (a new spot, if you are standing).
        rest_have_ = false;
        plan_next_ = plan_keep_ = 0.0;
        if (side_lock_ != 0.0f) angle_target_ = std::max(std::abs(angle_target_), 26.0f * degrees) * side_lock_;
        last_side_lock_ = side_lock_;
        side_switch_until_ = now + 2.5;
    }
    // Stopped by themselves, and what ends it.
    if (operator_have_ && halt_ != Halt::none) {
        const float away = flat_length(sub(t.position, operator_));
        const bool long_unseen = unseen_since_ >= 0 && now - unseen_since_ > 8.0;
        // The shot let go (panned down to the ground): the next clip, in behind you again.
        if (drift_over_) {
            drift_over_ = false;
            drifting_ = false;
            resume_behind();
        }
        switch (halt_) {
        case Halt::drop:
        case Halt::lag:
            // You push or step off the board (or click the left stick): straight back in behind you.
            // Long gone (out of sight, or a long way off): the same, as the next clip.
            // (Still close by: they just ride on after you, a push at a time.)
            if ((pushed_ && now - halt_since_ > 0.5) || away > 60.0f || long_unseen) {
                if (away < std::max(6.0f, distance * 2.5f) && !long_unseen) halt_ = Halt::none;
                else resume_behind();
            } else if (halt_ == Halt::lag && away < std::max(12.0f, distance * 5.0f) * 0.5f) {
                halt_ = Halt::none; // you came back
            }
            break;
        case Halt::slam:
            // Back up: carry on (from far off, they wait for you to push).
            if (subject.bailing) bail_over_since_ = -1.0;
            else if (bail_over_since_ < 0.0) bail_over_since_ = now;
            else if (now - bail_over_since_ > 0.8) {
                halt_ = Halt::none;
                if (away > std::max(12.0f, distance * 5.0f)) begin_halt(Halt::lag, now, 0.0f);
            }
            break;
        case Halt::wait:
        case Halt::none:
            break;
        }
    }
    // Lost sight of you for a while when riding along, and a good way off (behind a building, a wall):
    // cut. (Out of sight close by, they push on round it after you.)
    if (!planted_ && halt_ == Halt::none && !edge_ && !holding_ && post_ == Post::none && unseen_since_ >= 0 && now - unseen_since_ > 2.5 &&
        operator_have_ && flat_length(sub(t.position, operator_)) > std::max(10.0f, distance * 4.0f))
        operator_have_ = false;
    // Placed afresh (back to a marker, a teleport to someone, a cut): straight in behind you, a little
    // to the side the default cameras sit (the side chosen, or the one they were on), never round in
    // front and never riding round from where they were.
    if (fresh_ || !operator_have_) {
        const float keep_side = side_lock_ != 0.0f ? side_lock_ : (angle_target_ >= 0.0f ? 1.0f : -1.0f);
        angle_ = angle_target_ = plan_angle_ = 26.0f * degrees * keep_side;
        angle_rate_ = 0.0f;
        angle_until_ = now + 3.0;
        plan_next_ = plan_keep_ = 0.0;
        plan_scale_ = 1.0f;
        rest_have_ = false;
        still_since_ = -1.0;
        operator_have_ = false;
        halt_ = Halt::none;
        post_ = Post::none;
        caution_ = false;
        drifting_ = false;
        auto_zoom_ = zoom_ = 1.0f; // a cut: the new shot starts wide
        zoom_kind_ = 0;
        auto_zooming_ = false;
        wall_raise_target_ = 0.0f;
        hidden_since_ = -1.0;
        peeking_ = false;
        fresh_ = false;
    }
    const float sign = side_lock_ != 0.0f ? side_lock_ : (angle_target_ >= 0 ? 1.0f : -1.0f);
    // How sharply you are turning (radians a second), eased.
    {
        const float yaw_now = yaw_of(t.heading_x, t.heading_z);
        if (dt > 0.0f && speed > 1.0f) turn_rate_ += (wrap_angle(yaw_now - last_heading_yaw_) / dt - turn_rate_) * ease(dt, 0.15f);
        else turn_rate_ *= 0.9f;
        last_heading_yaw_ = yaw_now;
    }
    // The angle round the skater: behind, or out to the side, never in front (a filmer behind you on
    // their board, not riding backwards ahead of you).
    switch (s.style) {
    case Style::automatic: {
        // (Mid-turn is no time to swap sides, a ride across behind you as you carve: after it.)
        if (std::abs(turn_rate_) > 0.25f && now >= angle_until_ - 0.05) angle_until_ = now + 0.5;
        // A carve held for a moment: to the inside of it (the short way round, close to you), as a
        // filmer does, instead of out round the outside of it falling back.
        if (std::abs(turn_rate_) > 0.35f) {
            if (turning_since_ < 0.0) turning_since_ = now;
        } else if (std::abs(turn_rate_) < 0.2f) {
            turning_since_ = -1.0;
        }
        if (turning_since_ >= 0.0 && now - turning_since_ > 0.35 && side_lock_ == 0.0f && !grinding_ && speed > 2.0f) {
            const float inside = turn_rate_ > 0.0f ? 1.0f : -1.0f;
            if (angle_target_ * inside < 0.0f) {
                angle_target_ = std::abs(angle_target_) * inside;
                angle_until_ = std::max(angle_until_, now + 2.0);
            }
        }
        const std::uint8_t allowed = static_cast<std::uint8_t>(s.angles & (angle_behind | angle_side));
        choose_angle(now, speed, allowed ? allowed : angle_behind);
        if (side_lock_ != 0.0f) angle_target_ = std::abs(angle_target_) * side_lock_;
        break;
    }
    case Style::behind: angle_target_ = 28.0f * degrees * sign; break; // back and a little out, trailing you
    case Style::side:
    case Style::ahead: angle_target_ = 70.0f * degrees * sign; break;
    }
    angle_target_ = std::clamp(angle_target_, -75.0f * degrees, 75.0f * degrees);
    // Up on something (a ledge, a rail, a planter you manual along): the filmer stays down at their
    // level and, picking one at the start, either rides out a little wider with the camera up a bit
    // (to see the top of what you grind), pushes well out for the landing and the roll away, or, on a
    // slow grind with time to get there, races to the far end for a low angle as you come off it.
    {
        // How far you are up on something: above the ground beside you (either side, the lower), not
        // above the filmer (who is lower than you all the way up a hill without you being up on
        // anything). Without the game's answers, above the filmer's level.
        if (cast_ && world_answers() && now >= up_next_) {
            up_next_ = now + 0.15;
            probe_budget_ += 2;
            const Vec3 across{t.heading_z * 0.7f, 0.0f, -t.heading_x * 0.7f};
            const float feet = world(t.ground);
            float lowest = feet;
            for (const float side_sign : {1.0f, -1.0f}) {
                const auto g = ground_at(t.position[0] + across[0] * side_sign, t.position[2] + across[2] * side_sign, feet + 0.3f);
                lowest = std::min(lowest, g ? *g : feet - 1.0f);
            }
            up_beside_ = feet - lowest;
        } else if (!cast_ || !world_answers()) {
            up_beside_ = t.ground - level_;
        }
        const float up = up_beside_;
        // (Held for a moment first: a landing's first tick can still read high.)
        const bool up_there = operator_have_ && !t.airborne && up > 0.3f && up < 3.0f && speed > 0.5f && !joining_;
        if (!up_there) grind_seen_since_ = -1.0;
        else if (grind_seen_since_ < 0.0) grind_seen_since_ = now;
        // (The game's own word for it, your skater locked into a grind or a slide, at once; else told
        // from you being up on something narrow.)
        const bool grinding = (subject.grinding && operator_have_) || (up_there && (grinding_ || now - grind_seen_since_ > 0.08));
        if (grinding && !grinding_) {
            grind_since_ = now;
            // Beside you through it, picked at the start: level with you, a touch back or a touch
            // ahead (no rushing off to the end of it, no hanging back out wide).
            grind_style_ = 0;
            grind_angle_ = 84.0f + unit(random_) * 12.0f; // (square to you: your board across the frame)
            // (1 in 25: in behind you on your line instead, as if they were doing the trick too.)
            grind_trick_ = unit(random_) < 0.04f;
            if (grind_trick_) grind_angle_ = 0.0f;
            // On the side of it they are on (the way across to the other side is over what you grind).
            // (Which side of the rail they are on is where they are now from you, up on it: not the
            // side they rode on before you hopped across onto it.)
            grind_side_ = side_hard_ != 0.0f ? side_hard_ : (angle_ >= 0.0f ? 1.0f : -1.0f);
            grind_side_due_ = true;
            // (A new grind, not the same one picked up again after a flicker.)
            if (grind_over_since_ < 0.0 || now - grind_over_since_ > 1.5) grind_down_checked_ = grind_follow_down_ = false;
        }
        // Whether they stop where it starts and film you along it from there, or go along beside you:
        // a long downrail (a handrail, a hubba going down) 6 times in 10; anything short (under about
        // 4 m: a short rail, a box, a bump of a ledge) 3 in 10; a long flat one, never. How long it is
        // goes by what you are on: looked along the way you grind for where it ends (its top, going
        // on at your slope, gives way).
        if (grinding && !grind_down_checked_ && now - grind_since_ > 0.12 && s.stop_at_downrails) {
            grind_down_checked_ = true;
            const bool downhill = t.velocity[1] < -0.8f;
            float length = -1.0f; // not known (a thin rail the ground probes go past)
            if (cast_ && world_answers() && speed > 0.5f) {
                probe_budget_ += 30;
                const Vec3 way{t.velocity[0] / speed, 0.0f, t.velocity[2] / speed};
                const float slope = t.velocity[1] / speed, feet = world(t.position[1]);
                float along = 0.4f;
                for (; along <= 12.0f; along += 0.4f) {
                    const float expected = feet + slope * along;
                    const auto g = ground_at(t.position[0] + way[0] * along, t.position[2] + way[2] * along, expected + 0.3f, true);
                    if (!g || std::abs(*g - expected) > 0.25f) break;
                }
                if (along > 0.8f) length = along + std::max(0.0f, speed * static_cast<float>(now - grind_since_));
            }
            const bool short_one = length >= 0.0f && length < 4.0f;
            // What it goes down over: stairs (a run of steps, under it or beside it on their side), or
            // a slope (a bank, a ramp). Only a long one over stairs is one they may stop at the top of:
            // over a slope they always ride down it beside you.
            bool over_stairs = false;
            if (downhill && cast_ && world_answers() && speed > 0.5f) {
                probe_budget_ += 44;
                const Vec3 way{t.velocity[0] / speed, 0.0f, t.velocity[2] / speed};
                const Vec3 across{way[2], 0.0f, -way[0]};
                const float their_side = (operator_[0] - t.position[0]) * across[0] + (operator_[2] - t.position[2]) * across[2] >= 0.0f ? 1.0f : -1.0f;
                for (const float out : {0.0f, 1.4f * their_side}) {
                    const Vec3 from = add(Vec3{t.position[0], 0.0f, t.position[2]}, scale(across, out));
                    // (Asked fresh at each spot: the remembered answers are a quarter metre apart, which
                    // would make any slope look like steps.)
                    auto last = ground_at(from[0], from[2], world(t.position[1]) - 0.15f, true);
                    int risers = 0, treads = 0;
                    for (float along = 0.15f; last && along <= 3.0f; along += 0.15f) {
                        const auto g = ground_at(from[0] + way[0] * along, from[2] + way[2] * along, *last + 0.3f, true);
                        if (!g) break;
                        const float drop = *last - *g;
                        if (drop > 0.07f && drop < 0.4f) ++risers;
                        else if (std::abs(drop) < 0.025f) ++treads;
                        last = g;
                    }
                    if (risers >= 3 && treads >= risers) over_stairs = true;
                }
            }
            const float chance = short_one ? 0.3f : (downhill && over_stairs) ? 0.6f : 0.0f;
            if (unit(random_) < chance) {
                // (Rolled to a stop over a body length or two: no sudden stop.)
                if (halt_ == Halt::none) {
                    begin_halt(Halt::drop, now, std::max(1.5f, flat_length(operator_velocity_) * flat_length(operator_velocity_) / 6.0f));
                    halt_for_grind_ = true;
                }
            } else {
                grind_follow_down_ = true;
                caution_ = false;
            }
        }
        // A quarter second in, the way along it is known (you hopped across onto it, so the way you
        // were going is not it): their side of it is where they are now.
        if (grinding && grind_side_due_ && now - grind_since_ > 0.25 && side_hard_ == 0.0f) {
            grind_side_due_ = false;
            const Vec3 across_now{t.heading_z, 0.0f, -t.heading_x};
            const float out = (operator_[0] - t.position[0]) * across_now[0] + (operator_[2] - t.position[2]) * across_now[2];
            if (std::abs(out) > 0.02f) grind_side_ = out > 0.0f ? 1.0f : -1.0f; // (the nearer way round its end)
        }
        if (!grinding && grind_follow_down_ && grind_over_since_ >= 0.0 && now - grind_over_since_ > 1.5) grind_follow_down_ = false;
        if (grinding) grind_over_since_ = -1.0;
        else if (grinding_ && grind_over_since_ < 0.0) grind_over_since_ = now;
        grinding_ = grinding;
        const bool grind_on = grinding_ || (grind_over_since_ >= 0.0 && now - grind_over_since_ < 1.2);
        // (Locked in, as the game says: up beside you at once.)
        grind_weight_ += ((grind_on ? 1.0f : 0.0f) - grind_weight_) * ease(dt, grind_on ? (subject.grinding ? 0.12f : 0.3f) : 1.5f);
        // Through a grind the board in the middle of the frame; after it (and after any trick), slowly
        // back to framing your body.
        board_weight_ += ((grinding_ ? 1.0f : 0.0f) - board_weight_) * ease(dt, grinding_ ? 0.4f : 2.0f);
        // The end of the ledge: there, they wait, low, until you have come off it and rolled away.
        if (post_ == Post::rail_end && !grinding_ && !t.airborne && grind_over_since_ >= 0.0 && now - grind_over_since_ > 1.2) post_ = Post::none;
        if (post_ == Post::rail_end && now - post_since_ > 12.0) post_ = Post::none;
    }
    // A filmer never holds exactly the same spot.
    const float wander_angle = wobble_[0].sample(now * 0.31) * 9.0f * degrees * human;
    const float wander_distance = wobble_[1].sample(now * 0.27) * 0.35f * human +
        // Variety: a slow drift closer and further through a line (a filmer moving in and out).
        distance * 0.45f * std::clamp(s.distance_variety, 0.0f, 1.0f) * wobble_[4].sample(now * 0.06);
    // Through a grind: a little out from it, beside you.
    const float grind_out = grind_weight_ * 0.1f;
    // How far in front of your shoulders they may be: beside you through a grind (up to a body ahead),
    // otherwise never.
    const float lead = grind_weight_ * 1.6f;
    const float base_d = std::max(min_gap, (distance + wander_distance) * (1.0f + grind_out));
    const Vec3 heading{t.heading_x, 0, t.heading_z};
    const Vec3 side{t.heading_z, 0, -t.heading_x};
    // Variety: lower and higher through a line, slowly (a filmer crouching, then standing a little).
    float height = std::max(0.1f, (std::clamp(s.height, 0.08f, 2.5f) +
        0.35f * std::clamp(s.height_variety, 0.0f, 1.0f) * wobble_[5].sample(now * 0.05)) * mix(1.0f, 0.75f, lunge_));
    if (post_ == Post::rail_end) height = std::max(0.1f, height * 0.6f); // the low angle at the end
    // The way planner: the side (or straight behind), and how close, that is clear where you are
    // heading. Moving round to a new angle takes the time a filmer needs to ride there: a carve, a
    // little quicker when getting out of the way of something.
    bool trapped = false;
    const float grind_sign = grind_side_ != 0.0f ? grind_side_ : sign;
    // (Grinding: straight out to your side at once, square to you, never a drift round from behind.)
    const float planned = plan(now, grinding_ ? grind_sign * grind_angle_ * degrees
                                   : grind_weight_ > 0.01f ? grind_sign * mix(std::abs(angle_target_), grind_angle_ * degrees, grind_weight_) : angle_target_,
                               base_d, height, speed, trapped);
    {
        const bool crossing = planned * angle_ < 0.0f; // over to your other side (the left stick, or round something)
        // (Into a grind: up beside you without dawdling.)
        const float grinding_now = grinding_ ? 1.0f : grind_weight_;
        // (Sent over by you, the side buttons or the stick's click: quicker still.)
        const bool sent = crossing && now < side_switch_until_;
        const float most = std::max((sent ? 110.0f : crossing ? 60.0f : plan_risk_ > 0.5f ? 30.0f : 15.0f), 90.0f * grinding_now) * degrees;
        spring(angle_, angle_rate_, planned, 0.0f, std::max(sent ? 3.4f : crossing ? 2.2f : plan_risk_ > 0.5f ? 1.6f : 0.8f, 3.5f * grinding_now), 1.0f, dt);
        angle_rate_ = std::clamp(angle_rate_, -most, most);
    }
    plan_scale_ = std::clamp(plan_scale_, 0.7f, 1.4f);
    const float d = std::max(min_gap, base_d * plan_scale_);
    const float widest = mix(85.0f, 108.0f, grind_weight_) * degrees;
    const float angle = std::clamp(angle_ + wander_angle * (plan_risk_ > 0.5f ? 0.3f : 1.0f) * (1.0f - 0.6f * grind_weight_), -widest, widest);
    // Where on the trail the filmer is, and the level they ride at.
    std::optional<std::size_t> at_index;
    if (operator_have_) {
        at_index = trail_index_near(operator_[0], operator_[2], 4.0f);
        if (at_index && *at_index > last_index_) {
            // Ridden on along the trail since the last tick: follow its slopes to here.
            level_ = walk(last_index_, *at_index, level_, false).level;
        }
        if (at_index) last_index_ = *at_index;
        // And the ground the filmer is actually on, where the game answers: the level never drifts
        // from it (riding up a long gentle slope, it would, and everything ahead would look raised).
        if (cast_ && world_answers() && root_known_ && !edge_ && !holding_)
            level_ += (operator_floor_ + root_above_ground_ - level_) * ease(dt, 0.3f);
    }
    Vec3 desired;
    float ground = level_;
    bool edge = false;
    holding_ = false;
    Vec3 base{};
    std::size_t base_index = static_cast<std::size_t>(-1);
    float offset = 0.0f;
    bool raised_ahead = false;
    std::size_t rise_index = 0;
    if (trail_.size() >= 2) {
        // Behind and beside: on the trail the skater left, out to the side; never past a drop too high
        // to huck after an ollie, a gap, or a jump you are still in the air for.
        const float arc = std::max(0.25f, d * std::cos(angle));
        std::size_t index = trail_index_back(arc);
        if (operator_have_ && at_index && *at_index < index) {
            const auto w = walk(*at_index, index, level_, s.stop_at_drops && !grind_follow_down_);
            ground = w.level;
            edge = w.edge;
            holding_ = w.hold;
            if (w.edge || w.hold) {
                index = w.reach;
                // Which way is over the edge: along the trail from its last point.
                const auto &here = trail_[index].at;
                const auto &next = trail_[std::min(index + 1, trail_.size() - 1)].at;
                const Vec3 along_trail = sub(next, here);
                const float l = flat_length(along_trail);
                edge_dir_ = l > 0.01f ? Vec3{along_trail[0] / l, 0, along_trail[2] / l} : heading;
                edge_point_ = here;
            }
        } else if (!operator_have_) {
            ground = trail_[index].at[1];
        }
        base = trail_[index].at;
        base_index = index;
        offset = d * std::sin(angle);
        // You turned right round (a 180, a kickturn): your line behind you doubles back on itself, so
        // the point on it a filmer's distance back is beside you or ahead, not behind. They come round
        // after you, straight in behind you the way you face now, not waiting for your line to unwind.
        {
            const Vec3 back{base[0] - t.position[0], 0.0f, base[2] - t.position[2]};
            const float bl = flat_length(back);
            if (bl > 0.2f && speed > 0.5f) {
                const float behind_dot = -(back[0] * heading[0] + back[2] * heading[2]) / bl;
                const float w = std::clamp((0.7f - behind_dot) / 0.5f, 0.0f, 1.0f);
                if (w > 0.0f) {
                    const float arc_now = std::max(0.25f, d * std::cos(angle));
                    const Vec3 straight{t.position[0] - heading[0] * arc_now, base[1], t.position[2] - heading[2] * arc_now};
                    base = lerp(base, straight, w);
                    base_index = static_cast<std::size_t>(-1);
                }
            }
        }
        // Level with you or a touch ahead (beside you through a grind): on along your way from there.
        if (d * std::cos(angle) < 0.25f) base = add(base, scale(heading, 0.25f - d * std::cos(angle)));
        // Something raised on your line ahead of the filmer (the planter, the ledge you ollied onto):
        // seen well before they reach it (four metres and more at speed), so they pick a side, or stop
        // at its foot and raise the camera, before they are nosed up against it.
        const float look_ahead = 4.0f + std::min(flat_length(operator_velocity_), 8.0f) * 0.4f;
        const std::size_t ahead = trail_index_back(std::max(0.25f, arc - look_ahead));
        const std::size_t from = at_index ? std::min(*at_index, index) : index;
        // Ollied up onto something low and wide (a ledge, a manual pad): they ollie up after you and
        // ride it (a little lift of the camera as they go up), then down off the end after you, or stop
        // at the end if it is too high to huck off.
        {
            float lvl = ground;
            bool jumped = false;
            const bool was_joining = joining_;
            joining_ = false;
            // (A pop just behind where they are counts: the air before the first point looked at.)
            for (std::size_t k = from > 12 ? from - 12 : 0; k < from; ++k) {
                if (trail_[k].air) jumped = true;
                else if (trail_[k].at[1] <= ground + 0.3f) jumped = false;
            }
            for (std::size_t i = from; i < trail_.size(); ++i) {
                if (trail_[i].air) { jumped = true; continue; }
                const float y = trail_[i].at[1];
                if (y > lvl + 0.3f) {
                    if (jumped && y - lvl <= join_height && wide_at(i)) {
                        if (!joining_) { join_point_ = trail_[i].at; join_rise_ = y - lvl; }
                        joining_ = true;
                        lvl = y;
                    } else if (i <= std::max(ahead, index)) {
                        raised_ahead = true;
                        rise_index = i;
                        break;
                    }
                } else {
                    lvl = y; // rolling along: up and down slopes and banks, and down off things
                }
                jumped = false;
            }
            // Once on their way up, they carry on up (until they are up there, or a few seconds on).
            if (joining_ && !was_joining) join_since_ = now;
            if (!joining_ && was_joining && operator_floor_ < join_to_ - 0.2f && now - join_since_ < 3.0 && raised_ahead) {
                joining_ = true;
                raised_ahead = false;
            }
        }
    } else {
        base = sub(Vec3{t.position[0], t.ground, t.position[2]}, scale(heading, d * std::cos(angle)));
        offset = d * std::sin(angle);
        if (!operator_have_) ground = t.ground;
    }
    // Their way along your line already goes up onto it (they are on their way up, not yet there).
    if (!joining_ && operator_have_ && at_index && world(ground) > operator_floor_ + 0.3f && world(ground) - operator_floor_ <= join_height + 0.15f) {
        for (std::size_t i = *at_index; i < trail_.size(); ++i)
            if (!trail_[i].air && world(trail_[i].at[1]) > operator_floor_ + 0.3f) {
                join_point_ = trail_[i].at;
                joining_ = true;
                break;
            }
    }
    join_ok_ = joining_ && operator_have_;
    join_to_ = world(join_point_[1]);
    // Out to the side only where the ground is the filmer's level: not over a gap or off the end of a
    // ledge. Where you are up on something (a ledge, a rail, a planter) they keep at least a metre off
    // your line, beside it, never on it. The share of the offset used eases, so the spot never jumps.
    const bool raised = raised_ahead || base[1] > ground + 0.3f;
    const float lean = offset == 0.0f ? sign : (offset > 0.0f ? 1.0f : -1.0f);
    // Just seen: ride round beside it, or (now and then, a low one, or when there is no room beside
    // it) stop at its foot with the camera raised over it, as if following you up onto it.
    bool side_found = false;
    float side_wanted = std::max(std::abs(offset), 1.0f) * lean;
    if (raised && cast_) {
        for (const float flip : {1.0f, -1.0f}) {
            for (const float out : {std::max(std::abs(offset), 1.0f), 1.6f, 2.2f, 2.8f, 3.5f}) {
                // Level there and half a metre further in towards your line: room to ride by.
                const Vec3 spot = add(base, scale(side, out * lean * flip));
                const Vec3 inner = add(base, scale(side, (out - 0.5f) * lean * flip));
                const auto g = ground_at(spot[0], spot[2], ground);
                const auto gi = ground_at(inner[0], inner[2], ground);
                // (Going down with you, a rail down a bank: lower ground beside it is fine, so long as
                // it is ground to ride on, not a drop: level across from it.)
                const float below_ok = t.velocity[1] < -0.5f ? 3.0f : 0.3f;
                if (g && gi && *g < world(ground) + 0.3f && *g > world(ground) - below_ok && std::abs(*g - *gi) < 0.3f &&
                    *gi < world(ground) + 0.3f) {
                    side_wanted = out * lean * flip;
                    side_found = true;
                    break;
                }
            }
            if (side_found) break;
        }
    }
    if (raised && !raised_was_ && operator_have_ && post_ == Post::none && rise_index > 0) {
        const float rise = trail_[rise_index].at[1] - ground;
        // (They ride beside it and film the whole of whatever you do on it; only with no room beside it
        // at all do they wait at its foot with the camera up over it.)
        ledge_side_ = side_found;
        if (!ledge_side_ && rise < 1.3f) {
            // The foot of it: back along the trail from where you popped onto it (before the air), out
            // of the way of the face.
            std::size_t pop = rise_index;
            while (pop > 0 && (trail_[pop - 1].air || trail_[pop - 1].at[1] > ground + 0.3f)) --pop;
            if (pop > 0) --pop;
            const Vec3 face = trail_[pop].at;
            const Vec3 before_face = trail_[pop > 0 ? pop - 1 : 0].at;
            Vec3 back_dir = sub(before_face, face);
            const float bl = flat_length(back_dir);
            back_dir = bl > 0.01f ? scale(Vec3{back_dir[0], 0.0f, back_dir[2]}, 1.0f / bl) : scale(heading, -1.0f);
            post_ = Post::ledge;
            post_spot_ = add(Vec3{face[0], 0.0f, face[2]}, scale(back_dir, 0.9f));
            // The face itself (a little past where you popped), which they never get closer than 0.7 m to.
            ledge_face_ = trail_[rise_index].at;
            ledge_back_ = back_dir;
            post_since_ = now;
            ledge_lift_ = std::clamp(rise + 0.25f, 0.3f, 1.4f);
        }
    }
    // You landed up on something (not one they follow you onto) with them behind you, looking at
    // your back: the camera goes up at once to see over its lip, before it can hide you.
    if (raised && !raised_was_ && operator_have_ && !joining_ && rise_index > 0 && cast_) {
        const float along = (operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2];
        if (along < 0.0f && std::abs(angle_) < 55.0f * degrees) {
            const float rise = trail_[rise_index].at[1] - ground;
            const float want_up = std::min(raise_ceiling, rise + 0.45f) - height;
            if (want_up > wall_raise_target_) wall_raise_target_ = want_up;
            if (hidden_since_ < 0.0) hidden_since_ = now;
            seen_low_since_ = -1.0;
        }
    }
    raised_was_ = raised;
    if (post_ == Post::ledge) {
        // Back down off it (or a long way along it): they carry on, beside it if you are still up there.
        if (!raised && !t.airborne) post_ = Post::none;
        else if (flat_length(sub(t.position, operator_)) > 4.5f) { post_ = Post::none; ledge_side_ = true; }
    }
    if (joining_ && operator_floor_ < join_to_ - 0.2f) {
        // Ollying up after you: in behind you, on your line, so they land on it, not off its side.
        raised_offset_ += (0.0f - raised_offset_) * ease(dt, 0.15f);
        side_keep_ = 0.0f;
        desired = Vec3{base[0], ground, base[2]};
        if (std::abs(raised_offset_) > 0.02f) desired = add(desired, scale(side, raised_offset_));
    } else if (raised) {
        raised_offset_ += (side_wanted - raised_offset_) * ease(dt, 0.25f);
        side_keep_ = 1.0f;
        desired = add(Vec3{base[0], ground, base[2]}, scale(side, raised_offset_));
    } else {
        // Out to the side only where the ground is the filmer's level (not over a gap or off the end of
        // a ledge): otherwise closer in. The share of the offset used eases, so the spot never jumps.
        float keep_target = 1.0f;
        if (cast_ && std::abs(offset) > 0.05f) {
            keep_target = 0.0f;
            for (const float keep : {1.0f, 0.5f}) {
                const Vec3 spot = add(base, scale(side, offset * keep));
                const auto g = ground_at(spot[0], spot[2], ground);
                if (g && std::abs(*g - world(ground)) < 0.3f) { keep_target = keep; break; }
            }
        }
        side_keep_ += (keep_target - side_keep_) * ease(dt, 0.5f);
        // Through a turn they ride your line's own curve, close in on it: out from it square to the
        // way your line went there (not the way you face now, which throws them wide), and closer to
        // it the sharper you are turning, so they carve the same turn you do.
        Vec3 out_dir = side;
        if (base_index < trail_.size() && trail_.size() >= 3) {
            const std::size_t i0 = base_index > 0 ? base_index - 1 : 0, i1 = std::min(base_index + 1, trail_.size() - 1);
            const Vec3 tangent = sub(trail_[i1].at, trail_[i0].at);
            const float tl = flat_length(tangent);
            if (tl > 0.05f) out_dir = Vec3{tangent[2] / tl, 0.0f, -tangent[0] / tl};
        }
        const float tighten = std::clamp((std::abs(turn_rate_) - 0.15f) * 1.2f, 0.0f, 0.65f);
        raised_offset_ = offset * side_keep_ * (1.0f - tighten);
        desired = add(Vec3{base[0], ground, base[2]}, scale(out_dir, raised_offset_));
    }
    if (post_ != Post::none) desired = Vec3{post_spot_[0], ground, post_spot_[2]};
    // Through a grind or a slide (the game says you are locked in): straight out to your side, square to
    // you, so your board goes across the frame, on the side they were on when you locked in; whatever
    // else would have them (behind you on your line, behind what you grind, waiting at its foot). The
    // other side only if theirs has no ground to ride or a wall between. (Not on the 1 in 25 they
    // follow you along it instead.)
    if (grinding_ && !grind_trick_ && operator_have_ && !planted_ && halt_ == Halt::none && grind_side_ != 0.0f) {
        Vec3 way{t.velocity[0], 0.0f, t.velocity[2]};
        const float wl = flat_length(way);
        way = wl > 0.5f ? scale(way, 1.0f / wl) : heading;
        const Vec3 across{way[2], 0.0f, -way[0]};
        const float feet = world(t.position[1]);
        const float out = std::max(min_gap, distance);
        const float ahead_of = -out * std::cos(grind_angle_ * degrees); // (a touch back or ahead)
        const Vec3 centre_now{t.position[0], feet + 0.6f, t.position[2]};
        for (const float which : {grind_side_, -grind_side_}) {
            const Vec3 spot = add(Vec3{t.position[0], 0.0f, t.position[2]}, add(scale(across, out * which), scale(way, ahead_of)));
            const auto g = cast_ && world_answers() ? ground_at(spot[0], spot[2], feet + 0.6f) : std::optional<float>(operator_floor_);
            if (!g || *g > feet + 0.3f || *g < feet - 3.0f) continue;
            if (cast_ && obstructed(centre_now, Vec3{spot[0], *g + height + 0.1f, spot[2]}, 0.25f, true)) continue;
            desired = Vec3{spot[0], ground, spot[2]};
            post_ = Post::none;
            if (which != grind_side_) grind_side_ = which;
            break;
        }
    }
    // You standing still: the filmer settles on a spot behind you (left or right of your back, straight
    // behind when those are no good) and stays there, still, so you can still see ahead of you. Only if
    // it goes bad (you turned round and it is in front of you now, or a wall is between) after a moment
    // they move to another.
    const bool still = speed < 0.5f && !t.airborne;
    if (!still) {
        still_since_ = -1.0;
        rest_have_ = false;
    } else if (still_since_ < 0) {
        still_since_ = now;
    }
    if (still && now - still_since_ > 0.4 && operator_have_ && !planted_ && !edge && !holding_ && halt_ == Halt::none && post_ == Post::none) {
        if (!rest_have_ || flat_length(sub(t.position, rest_from_)) > 1.0f) {
            choose_rest(std::max(d, min_gap + 0.2f), height);
        } else if (now >= rest_next_check_) {
            rest_next_check_ = now + 0.5;
            const Vec3 rel = sub(rest_spot_, t.position);
            const bool in_front = rel[0] * heading[0] + rel[2] * heading[2] > 0.2f * d;
            const bool hidden = cast_ && !rest_spot_ok(rest_spot_, d, height);
            if (!in_front && !hidden) rest_bad_since_ = -1.0;
            else if (rest_bad_since_ < 0) rest_bad_since_ = now;
            else if (now - rest_bad_since_ > 1.2) choose_rest(std::max(d, min_gap + 0.2f), height);
        }
        desired = Vec3{rest_spot_[0], ground, rest_spot_[2]};
    }
    const Vec3 centre{t.position[0], world(t.position[1]) + 0.9f, t.position[2]};
    // Never behind a wall: where the spot is out of sight of the skater, it comes in towards them.
    if (cast_ && !edge && !holding_ && halt_ == Halt::none && post_ == Post::none) {
        const Vec3 spot{desired[0], ground + height + 0.1f, desired[2]};
        Vec3 hit{};
        if (blocked(centre, spot, &hit)) {
            const Vec3 to = sub(spot, centre);
            const float full = flat_length(to);
            if (full > 0.01f) {
                const float keep = std::max(min_gap, flat_length(sub(hit, centre)) - 0.4f);
                desired = add(Vec3{centre[0], ground, centre[2]}, scale(Vec3{to[0], 0, to[2]}, keep / full));
            }
        }
    }
    // Gone closer to look over what hides you.
    if (peeking_) desired = Vec3{peek_spot_[0], ground, peek_spot_[2]};
    // Never in front of you: riding along, the spot is at least half a metre behind your shoulders.
    if (!still && speed > 1.0f && post_ != Post::rail_end && !peeking_) {
        const float along = (desired[0] - t.position[0]) * heading[0] + (desired[2] - t.position[2]) * heading[2];
        if (along > lead - 0.5f) {
            desired[0] -= heading[0] * (along + 0.5f - lead);
            desired[2] -= heading[2] * (along + 0.5f - lead);
        }
    }
    // And never too close to you.
    {
        const Vec3 rel{desired[0] - t.position[0], 0.0f, desired[2] - t.position[2]};
        const float gap = flat_length(rel);
        if (gap < min_gap) {
            const Vec3 out = gap > 0.05f ? scale(rel, 1.0f / gap) : scale(heading, -1.0f);
            desired[0] = t.position[0] + out[0] * min_gap;
            desired[2] = t.position[2] + out[2] * min_gap;
        }
    }
    if (edge && !edge_) edge_since_ = now;
    edge_ = edge;
    // A new spot: far off the one wanted (a teleport, a respawn), or after a cut. Never while stopped.
    if (!operator_have_ || (!planted_ && !edge_ && !holding_ && halt_ == Halt::none && post_ == Post::none &&
                            flat_length(sub(operator_, desired)) > 15.0f)) {
        // Somewhere with ground about your level under it and you in sight from it: the spot wanted, or
        // failing that another round you (never placed down off an edge, into water or under the map).
        float placed_ground = cast_ ? ground_at(desired[0], desired[2], ground).value_or(world(ground)) : ground;
        if (cast_ && world_answers()) {
            const float feet = world(t.position[1]);
            const Vec3 you_at{t.position[0], feet + 0.9f, t.position[2]};
            const auto good = [&](const Vec3 &spot, float &g_out) {
                const auto g = ground_at(spot[0], spot[2], feet + 0.8f, true);
                if (!g || *g < feet - 1.2f || *g > feet + 0.6f) return false;
                if (blocked(Vec3{spot[0], *g + 0.6f, spot[2]}, you_at, nullptr, true)) return false;
                g_out = *g;
                return true;
            };
            float g = 0.0f;
            if (!good(desired, g)) {
                const Vec3 heading_now{t.heading_x, 0.0f, t.heading_z}, side_now{t.heading_z, 0.0f, -t.heading_x};
                const float dd = std::max(min_gap, d);
                bool found = false;
                for (const float a : {26.0f, -26.0f, 0.0f, 60.0f, -60.0f, 90.0f, -90.0f}) {
                    for (const float k : {1.0f, 0.6f}) {
                        const float r = a * degrees;
                        const Vec3 spot = add(Vec3{t.position[0], 0.0f, t.position[2]}, add(scale(heading_now, -dd * k * std::cos(r)), scale(side_now, dd * k * std::sin(r))));
                        if (good(spot, g)) { desired = Vec3{spot[0], desired[1], spot[2]}; found = true; break; }
                    }
                    if (found) break;
                }
                // Nowhere: right by you, at your level.
                if (!found) { desired = Vec3{t.position[0] - t.heading_x * min_gap, desired[1], t.position[2] - t.heading_z * min_gap}; g = feet; }
            }
            placed_ground = g;
        }
        operator_ = desired;
        operator_velocity_ = t.velocity;
        operator_velocity_[1] = 0;
        level_ = ground;
        operator_ground_ = placed_ground;
        operator_floor_ = operator_ground_;
        floor_rate_ = 0.0f;
        operator_height_ = operator_ground_ + height;
        operator_height_rate_ = 0;
        last_desired_ = desired;
        last_index_ = trail_index_near(desired[0], desired[2], 4.0f).value_or(trail_.empty() ? 0 : trail_.size() - 1);
        operator_have_ = true;
        edge_ = holding_ = false;
        unseen_since_ = -1.0;
        lens_path_.clear();
        eye_have_ = eye_y_have_ = false;
        placed_at_ = now;
    }
    // Stopping by themselves.
    if (!planted_) {
        // Over a drop too high to huck: braking to a stop short of the edge.
        if (edge_) {
            const float to_edge = (edge_point_[0] - operator_[0]) * edge_dir_[0] + (edge_point_[2] - operator_[2]) * edge_dir_[2];
            begin_halt(Halt::drop, now, to_edge - 0.5f);
        }
        // In the air over a drop too high to huck, told from the jump itself, not the landing: from a
        // moment into the air (a real pop, not a bump), the ground under you is looked at as you go;
        // once it is more than a huck below where you took off (a set, a big drop, a gap), or you have
        // been falling well past an ollie's airtime below your takeoff, they start braking for the top
        // at once, before they get anywhere near the first step. Over a ramp or a bank (the ground
        // just under you), it is no drop.
        if (t.airborne && s.stop_at_drops && !grind_follow_down_ && halt_ == Halt::none && post_ == Post::none && cast_ && world_answers() && now >= drop_next_) {
            drop_next_ = now + 0.08;
            const double airtime = now - t.air_since;
            // Off something with the ground well below: they ease off at once, a moment into the air,
            // before it is known whether they stop (they let go again if you land level).
            if (!caution_ && airtime > 0.12 && airtime <= drop_airtime_) {
                probe_budget_ += 1;
                const float takeoff = world(takeoff_height_);
                const auto under = ground_at(t.position[0], t.position[2], world(t.position[1]) + 0.3f);
                std::size_t k = trail_.size();
                while (k > 0 && trail_[k - 1].air) --k;
                if ((!under || takeoff - *under > huck_) && k > 0) {
                    caution_ = true;
                    caution_air_ = false; // (told by the next look, once you have been up long enough)
                    caution_since_ = now;
                    caution_point_ = trail_[k - 1].at;
                    caution_top_ = takeoff;
                }
            }
            if (airtime > drop_airtime_) {
                probe_budget_ += 1;
                const float takeoff = world(takeoff_height_), feet = world(t.position[1]);
                const auto under = ground_at(t.position[0], t.position[2], feet + 0.3f);
                const bool over_surface = under && feet - *under < 0.6f && takeoff - *under < huck_;
                const bool deep = !under || takeoff - *under > huck_;
                // Coming down, below where you took off (off a ramp over a gap up onto something
                // higher, you never are: they follow you).
                const bool falling = feet < takeoff - 0.3f && t.velocity[1] < -0.5f;
                const bool long_fall = airtime > drop_airtime_ + 0.5 && takeoff - feet > huck_ * 0.6f;
                // Where you will come down (your flight carried on, under gravity): onto something no
                // further below your takeoff than they would huck (over a gap, up onto a higher
                // surface), it is no drop: they follow you.
                bool lands_ok = false;
                if (!over_surface && falling && (deep || long_fall)) {
                    probe_budget_ += 12;
                    for (float ahead = 0.1f; ahead <= 1.2f; ahead += 0.1f) {
                        const float x = t.position[0] + t.velocity[0] * ahead, z = t.position[2] + t.velocity[2] * ahead;
                        const float y = feet + t.velocity[1] * ahead - 4.9f * ahead * ahead;
                        const auto g = ground_at(x, z, y + 0.6f);
                        if (g && *g >= y - 0.1f) {
                            lands_ok = *g >= takeoff - 0.5f; // (level with your takeoff, or up)
                            break;
                        }
                    }
                }
                if (!over_surface && falling && (deep || long_fall) && !lands_ok) {
                    // Where you took off: the last point of your line before this jump.
                    std::size_t k = trail_.size();
                    while (k > 0 && trail_[k - 1].air) --k;
                    if (k > 0) {
                        const Vec3 &lip = trail_[k - 1].at;
                        edge_point_ = lip;
                        edge_dir_ = heading;
                        const float to_lip = (lip[0] - operator_[0]) * heading[0] + (lip[2] - operator_[2]) * heading[2];
                        begin_halt(Halt::drop, now, to_lip - 0.5f);
                        edge_ = true;
                    }
                }
            }
        }
        // You slammed: stop where they are (rolling to it), still filming you.
        if (subject.bailing && halt_ != Halt::slam) begin_halt(Halt::slam, now, 6.0f);
        // Too far behind for good footy, and not closing the gap (for a second): ease off and roll to
        // a stop.
        {
            const Vec3 rel{t.position[0] - operator_[0], 0.0f, t.position[2] - operator_[2]};
            const float away = flat_length(rel);
            const float closing = away > 0.01f ? ((operator_velocity_[0] - t.velocity[0]) * rel[0] + (operator_velocity_[2] - t.velocity[2]) * rel[2]) / away : 0.0f;
            // (Only when they cannot keep up: held up by something, or you are faster than a filmer
            // could ever push. Riding as fast as they can and getting closer, they keep at it.)
            const bool held_up = flat_length(operator_velocity_) < std::max(1.5f, speed * 0.6f);
            // (And only with you out of sight: in view, they keep pushing after you.)
            const bool out_of_sight = unseen_since_ >= 0 && now - unseen_since_ > 0.5;
            if (halt_ == Halt::none && post_ == Post::none && !peeking_ && away > std::max(15.0f, distance * 6.0f) && closing < 0.3f && held_up && out_of_sight) {
                if (losing_since_ < 0.0) losing_since_ = now;
            } else {
                losing_since_ = -1.0;
            }
            if (losing_since_ >= 0.0 && now - losing_since_ > 1.0) begin_halt(Halt::lag, now, 8.0f);
        }
        // Nowhere clear to ride beside you (an alley, between walls): in behind you on your own line,
        // which you just rode, so is clear; they carry on filming, never stop for it.
        if (trapped && halt_ == Halt::none && post_ == Post::none && !still && !grinding_ && grind_weight_ < 0.3f) trail_follow_until_ = now + 0.5; // (not along what you grind)
        if (halt_ == Halt::wait) halt_ = Halt::none;
        // Waiting and you rode past them (or off away from them): on again, behind you.
        if (halt_ == Halt::wait && speed > 1.0f) {
            const float along = (operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2];
            if (along < -min_gap && now - halt_since_ > 0.8) halt_ = Halt::none;
        }
        if (halt_ == Halt::wait && now - halt_since_ > 1.5) halt_ = Halt::none; // look again, riding
    }
    if (halt_ == Halt::none) drifting_ = false;
    // A drop ahead of you that would be too high to huck: a filmer sees it coming and eases off, so
    // that if you ollie off it they can stop at the top without slamming on the brakes. (You roll down
    // it instead, a ramp or a bank: they carry on after you.)
    if (cast_ && world_answers() && operator_have_ && now >= caution_next_ && s.stop_at_drops) {
        caution_next_ = now + 0.2;
        if (caution_) {
            const float passed = (t.position[0] - caution_point_[0]) * heading[0] + (t.position[2] - caution_point_[2]) * heading[2];
            // (A real jump, not the moment the wheels leave a ramp's lip or a bank's top.)
            if (t.airborne && now - t.air_since > 0.25) caution_air_ = true;
            const bool landed = !t.airborne;
            const float at_now = world(t.ground);
            // Clear: rolled down it without leaving the ground (a ramp, a bank), landed level or higher
            // after a jump (a gap cleared, an ollie on the flat), rode on past without going off it,
            // stopped, or they are stopping for it anyway. (Landed down below after a jump: kept, until
            // they have stopped at the top: it is a drop, and they ease off for it all the way.)
            if ((!caution_air_ && at_now < caution_top_ - 0.3f && (landed || now - t.air_since < 0.25)) ||
                (landed && caution_air_ && at_now >= caution_top_ - 0.5f) ||
                (landed && !caution_air_ && passed > 3.0f) || speed < 0.5f || halt_ != Halt::none || now - caution_since_ > 5.0)
                caution_ = false;
        }
        if (!caution_ && !t.airborne && speed > 2.0f && !grind_follow_down_ && !grinding_) {
            probe_budget_ += 10;
            const Vec3 way{t.velocity[0] / speed, 0.0f, t.velocity[2] / speed};
            // A lip: the ground falls away steeper than a ramp (or is not there), and a huck's height
            // down within a couple of metres of it. (A hill or a bank goes down gently: no lip.)
            const float top = world(t.ground);
            float last = top;
            // (Looked for far enough ahead, at your speed, for them to ease off over a second or more.)
            const float horizon = std::clamp(speed * 1.4f, 6.0f, 14.0f);
            probe_budget_ += 12;
            for (float along = 0.6f; along <= horizon; along += 0.6f) {
                const auto g = ground_at(t.position[0] + way[0] * along, t.position[2] + way[2] * along, last + 0.3f);
                const bool lip = !g || last - *g > 0.45f; // steeper than about 37 degrees, or nothing
                if (lip) {
                    float lowest = g ? *g : top - 10.0f;
                    for (float more = 0.6f; g && more <= 2.4f && lowest > top - huck_; more += 0.6f) {
                        const auto further = ground_at(t.position[0] + way[0] * (along + more), t.position[2] + way[2] * (along + more), last + 0.3f);
                        lowest = further ? std::min(lowest, *further) : top - 10.0f;
                    }
                    if (lowest < top - huck_) {
                        caution_ = true;
                        caution_air_ = false;
                        caution_since_ = now;
                        caution_point_ = Vec3{t.position[0] + way[0] * (along - 0.6f), 0.0f, t.position[2] + way[2] * (along - 0.6f)};
                        caution_top_ = top;
                    }
                    break;
                }
                last = *g;
            }
        }
    }
    // Ollying over what you ollied over: a bubble round them, looked into the way they ride a few
    // times a second. Something low coming up (a bin, a hurdle, a kerb-high box: knee-high or less),
    // with the ground carrying on past it, at a spot on your line where you were in the air: they
    // ride at it, pop near where you did, and land past it (no steering round it).
    {
        const float riding = flat_length(operator_velocity_);
        const bool can = cast_ && world_answers() && operator_have_ && halt_ == Halt::none && post_ == Post::none &&
                         !planted_ && !edge_ && !holding_ && !on_foot_ && riding > 1.5f &&
                         !grinding_ && grind_weight_ < 0.3f && up_beside_ < 0.3f;
        if (!can) ollie_planned_ = false;
        if (can && now >= ollie_until_ && !ollie_planned_ && now >= ollie_next_) {
            ollie_next_ = now + 0.1;
            probe_budget_ += 4;
            ray_budget_ += 2;
            const Vec3 dir = scale(Vec3{operator_velocity_[0], 0.0f, operator_velocity_[2]}, 1.0f / riding);
            const float reach = std::clamp(riding * 0.6f, 1.2f, 3.0f);
            const Vec3 from{operator_[0], operator_floor_ + 0.15f, operator_[2]};
            Vec3 hit{};
            if (blocked(from, add(from, scale(dir, reach)), &hit)) {
                // How high it is (its top, just past its face), and the ground past it.
                const Vec3 on_it = add(hit, scale(dir, 0.15f));
                const auto top = ground_at(on_it[0], on_it[2], operator_floor_ + 1.0f);
                const float h = top ? *top - operator_floor_ : 99.0f;
                float far_side = -99.0f;
                for (float past = 0.6f; past <= 2.4f; past += 0.6f) {
                    const Vec3 at = add(hit, scale(dir, past));
                    const auto g = ground_at(at[0], at[2], operator_floor_ + 1.0f);
                    if (g && std::abs(*g - operator_floor_) < 0.25f) { far_side = *g; break; }
                }
                if (h > 0.08f && h <= 0.75f && far_side > -98.0f && ollied_over(hit, dir, 0.3f) &&
                    !blocked(Vec3{from[0], operator_floor_ + h + 0.25f, from[2]}, add(Vec3{hit[0], operator_floor_ + h + 0.25f, hit[2]}, scale(dir, 0.6f)))) {
                    ollie_planned_ = true;
                    ollie_gap_ = false;
                    ollie_at_ = hit;
                    ollie_h_ = h;
                    ollie_base_ = operator_floor_;
                    ollie_dir_ = dir;
                }
            } else if (s.stop_at_drops) {
                // A gap you ollied (the ground gone, or far below, then ground again level with them or
                // up to knee-high above): they ollie it after you, from its lip.
                float lip = -1.0f;
                for (float along = 0.4f; along <= reach + 0.4f; along += 0.4f) {
                    const auto g = ground_at(operator_[0] + dir[0] * along, operator_[2] + dir[2] * along, operator_floor_ + 0.5f);
                    if (!g || *g < operator_floor_ - 0.85f) { lip = along - 0.2f; break; }
                }
                if (lip > 0.0f) {
                    probe_budget_ += 12;
                    for (float along = lip + 0.6f; along <= lip + 5.0f; along += 0.4f) {
                        const Vec3 at = add(operator_, scale(dir, along));
                        const auto g = ground_at(at[0], at[2], operator_floor_ + join_height + 0.3f);
                        if (g && *g >= operator_floor_ - 0.35f) {
                            const Vec3 edge_at = add(operator_, scale(dir, lip));
                            const Vec3 middle = add(operator_, scale(dir, (lip + along) * 0.5f));
                            if (*g <= operator_floor_ + join_height && ollied_over(middle, dir, join_height + 0.1f)) {
                                ollie_planned_ = true;
                                ollie_gap_ = true;
                                ollie_at_ = Vec3{edge_at[0], operator_floor_, edge_at[2]};
                                ollie_h_ = std::max(0.0f, *g - operator_floor_) + 0.2f;
                                ollie_land_ = *g;
                                ollie_span_ = along - lip;
                                ollie_base_ = operator_floor_;
                                ollie_dir_ = dir;
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (ollie_planned_) {
            const float to_it = (ollie_at_[0] - operator_[0]) * ollie_dir_[0] + (ollie_at_[2] - operator_[2]) * ollie_dir_[2];
            const float speed_on = std::max(riding, 1.5f);
            // Popped a board length or so before it, quicker the faster they go.
            if (to_it < (ollie_gap_ ? 0.25f : 0.35f + speed_on * 0.12f)) {
                ollie_planned_ = false;
                ollie_pop_at_ = now;
                if (ollie_gap_) {
                    // Over the gap: at least a pace that clears it.
                    const float pace = std::max(speed_on, 3.0f);
                    if (riding < pace) operator_velocity_ = scale(ollie_dir_, pace);
                    ollie_until_ = now + std::clamp((std::max(to_it, 0.0f) + ollie_span_ + 0.3f) / pace, 0.35f, 1.6f);
                } else {
                    // In the air until past it (its width, a little more), never less than a small pop.
                    float width = 0.3f;
                    for (float past = 0.3f; past <= 2.4f; past += 0.3f) {
                        const Vec3 at = add(ollie_at_, scale(ollie_dir_, past));
                        const auto g = ground_at(at[0], at[2], ollie_base_ + 1.0f);
                        if (g && *g < ollie_base_ + 0.06f) { width = past; break; }
                        width = past;
                    }
                    ollie_until_ = now + std::clamp((std::max(to_it, 0.0f) + width + 0.4f) / speed_on, 0.4f, 0.9f);
                }
            } else if (to_it < -0.5f) {
                ollie_planned_ = false; // gone past it some other way
            }
        }
    }
    const bool ollying = now < ollie_until_ || ollie_planned_;
    // Steering round what is in the way (a pole, a bench, a bin, a low wall): a few times a second
    // they look ahead the way they are riding, and if it is not clear, take the nearest way round that
    // is (towards their spot first), keeping to that side until they are past it (no dithering).
    // (Ollying up after you onto a ledge: on your line, which you just rode; no steering off it.)
    const bool popping_up = joining_ && operator_floor_ < join_to_ - 0.2f;
    if (cast_ && !planted_ && !edge_ && !holding_ && halt_ == Halt::none && !popping_up && !ollying) {
        const Vec3 to{desired[0] - operator_[0], 0.0f, desired[2] - operator_[2]};
        const float dist = flat_length(to);
        const float riding = flat_length(operator_velocity_);
        // The way to their spot (a turn off it is the way round; turned off the way they happen to be
        // riding, turns would pile up on turns), or, right by it, the way they ride.
        Vec3 way{};
        if (dist > 0.8f) way = scale(to, 1.0f / dist);
        else if (riding > 0.8f) way = scale(Vec3{operator_velocity_[0], 0.0f, operator_velocity_[2]}, 1.0f / riding);
        else if (speed > 0.8f) way = Vec3{t.heading_x, 0.0f, t.heading_z};
        else if (dist > 0.4f) way = scale(to, 1.0f / dist);
        if (flat_length(way) > 0.5f) {
            const auto rotated = [](const Vec3 &v, float a) {
                return Vec3{v[0] * std::cos(a) - v[2] * std::sin(a), 0.0f, v[0] * std::sin(a) + v[2] * std::cos(a)};
            };
            if (now >= steer_next_) {
                steer_next_ = now + 0.1;
                // (Far enough ahead to get round it in time at their pace, as a carve: most of a second.)
                // (Beside what you grind, the way along it close by: a short look, not across it.)
                const float look = grind_weight_ > 0.1f || grinding_ ? std::clamp(1.0f + std::max(riding, speed) * 0.3f, 1.5f, 3.2f)
                                                                    : std::clamp(1.5f + std::max(riding, speed) * 0.55f, 2.0f, 6.0f);
                probe_budget_ += 30; // the look round has its own questions
                ray_budget_ += 24;
                // 0 clear, 1 in the way (or no ground a board would roll on), 2 clear down the middle
                // but too tight for a body (a doorway, a gap between two things).
                const auto check = [&](const Vec3 &heading_way) -> int {
                    // Ground at a third, two thirds and the end: each step up no more than a kerb, each
                    // step down no steeper than a ramp (a slope is many small steps; a bench or a
                    // planter is one big one, a ledge's end a sudden drop).
                    float last = operator_floor_;
                    const float piece = look / 3.0f;
                    // Following you up onto a ledge: its top is ground to them, not a wall.
                    const bool popping = join_ok_ && flat_length(sub(operator_, join_point_)) < look + 1.0f;
                    const float start_low = popping ? std::max(operator_floor_, join_to_) + 0.22f : operator_floor_ + 0.22f;
                    for (const float u : {1.0f / 3.0f, 2.0f / 3.0f, 1.0f}) {
                        const Vec3 at = add(operator_, scale(heading_way, look * u));
                        const auto g = ground_at(at[0], at[2], last + 0.5f);
                        if (!g || (*g - last > 0.25f && !(popping && *g <= join_to_ + 0.15f))) return 1;
                        if (s.stop_at_drops && last - *g > piece * 1.2f + 0.1f) return 1;
                        last = *g;
                    }
                    const Vec3 end = add(operator_, scale(heading_way, look));
                    if (obstructed(Vec3{operator_[0], start_low, operator_[2]}, Vec3{end[0], (popping ? std::max(last, join_to_) : last) + 0.22f, end[2]}, 0.22f, true) ||
                        obstructed(Vec3{operator_[0], operator_floor_ + 1.0f, operator_[2]}, Vec3{end[0], last + 1.0f, end[2]}, 1.0f, true))
                        return 1;
                    // A body wide: either shoulder, at the knee.
                    const Vec3 across{-heading_way[2] * 0.22f, 0.0f, heading_way[0] * 0.22f};
                    for (const float shoulder : {1.0f, -1.0f})
                        if (obstructed(Vec3{operator_[0] + across[0] * shoulder, start_low, operator_[2] + across[2] * shoulder},
                                       Vec3{end[0] + across[0] * shoulder, (popping ? std::max(last, join_to_) : last) + 0.22f, end[2] + across[2] * shoulder}, 0.22f, true))
                            return 2;
                    return 0;
                };
                const auto clear = [&](const Vec3 &heading_way) { return check(heading_way) == 0; };
                const int straight = check(way);
                const bool near_line = at_index && flat_length(sub(trail_[*at_index].at, operator_)) < 1.5f;
                if (straight == 0) {
                    if (now >= steer_hold_) { steer_target_ = 0.0f; steer_side_ = 0.0f; }
                } else if (straight == 2 && near_line && grind_weight_ < 0.3f) {
                    // Tight, but you rode through it: through it on your line, down the middle.
                    steer_target_ = 0.0f;
                    steer_side_ = 0.0f;
                    trail_follow_until_ = now + 0.8;
                } else {
                    // The smallest turn that is clear, either side; the side already being taken
                    // round first at each angle, else the one towards their spot.
                    float first = steer_side_;
                    if (first == 0.0f) first = (way[0] * to[2] - way[2] * to[0]) >= 0.0f ? 1.0f : -1.0f;
                    // (Riding along with you: round it on the side of you they are on, not across in
                    // front of it towards your line.)
                    if (steer_side_ == 0.0f && speed > 1.0f) {
                        const Vec3 across_you{t.heading_z, 0.0f, -t.heading_x};
                        const float out = (operator_[0] - t.position[0]) * across_you[0] + (operator_[2] - t.position[2]) * across_you[2];
                        if (std::abs(out) > 0.3f) {
                            const Vec3 keep = scale(across_you, out > 0.0f ? 1.0f : -1.0f);
                            const Vec3 plus = rotated(way, 0.3f), minus = rotated(way, -0.3f);
                            first = plus[0] * keep[0] + plus[2] * keep[2] >= minus[0] * keep[0] + minus[2] * keep[2] ? 1.0f : -1.0f;
                        }
                    }
                    bool found = false;
                    for (const float a : {20.0f, 35.0f, 50.0f, 70.0f, 90.0f, 120.0f}) {
                        for (const float side_sign : {first, -first}) {
                            if (clear(rotated(way, a * degrees * side_sign))) {
                                steer_target_ = a * degrees * side_sign;
                                steer_side_ = side_sign;
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                    if (!found) {
                        // No clear way round: through it on the line you rode, which is known to be clear.
                        steer_target_ = 0.0f;
                        steer_side_ = 0.0f;
                        if (grind_weight_ < 0.3f) trail_follow_until_ = now + 0.8;
                    }
                    steer_hold_ = now + 0.6;
                }
            }
            if (now < trail_follow_until_ && at_index && !trail_.empty()) {
                // On your own line: a point on it a little ahead of where they are.
                std::size_t k = *at_index;
                float went = 0.0f;
                // (Far enough on to keep your pace, near enough to stay on the line.)
                const float carrot = std::clamp(speed * 0.45f, 0.7f, 3.0f);
                // (Never on up onto what you went up on, a ledge you grind: the line stops at its foot.)
                const float highest = operator_floor_ + (joining_ ? join_height + 0.3f : 0.45f) + root_above_ground_;
                while (k + 1 < trail_.size() && went < carrot && (trail_[k + 1].air || trail_[k + 1].at[1] <= highest)) { went += flat_length(sub(trail_[k + 1].at, trail_[k].at)); ++k; }
                desired = Vec3{trail_[k].at[0], desired[1], trail_[k].at[2]};
            }
            // Turned into gradually, as on a board: a carve, not a snap.
            steer_angle_ += std::clamp((steer_target_ - steer_angle_) * ease(dt, 0.18f), -120.0f * degrees * dt, 120.0f * degrees * dt);
            if (std::abs(steer_angle_) > 0.5f * degrees) {
                // Ride that way at their pace, then back towards the spot once past.
                const Vec3 turned = rotated(way, steer_angle_);
                // (Only round it: a couple of metres that way, not off as far as their spot is.)
                desired = add(Vec3{operator_[0], desired[1], operator_[2]}, scale(turned, std::clamp(dist, 1.2f, 2.2f)));
            }
        }
    } else {
        steer_angle_ = steer_target_ = 0.0f;
        steer_side_ = 0.0f;
    }
    // Riding with you: at your speed (not the spot's jumps from tick to tick).
    const Vec3 desired_rate{t.velocity[0], 0.0f, t.velocity[2]};
    last_desired_ = desired;
    const Vec3 before = operator_;
    const Vec3 velocity_before = operator_velocity_;
    if (planted_ && rolling_stop_) {
        // Stair stop: braking along the way they rode to the stop point, hard enough to make it.
        if (stop_due_) {
            compute_rolling_stop(operator_floor_);
            stop_due_ = false;
        }
        const float remaining = (stop_point_[0] - operator_[0]) * stop_dir_[0] + (stop_point_[2] - operator_[2]) * stop_dir_[2];
        if (!rolling_stop_ || remaining <= 0.02f || roll_speed_ <= 0.05f) {
            rolling_stop_ = false;
            operator_velocity_ = {};
        } else {
            const float brake = std::min(roll_speed_ * roll_speed_ / (2.0f * remaining), 9.0f);
            roll_speed_ = std::max(0.0f, roll_speed_ - brake * dt);
            operator_ = add(operator_, scale(stop_dir_, std::min(roll_speed_ * dt, remaining)));
            operator_velocity_ = scale(stop_dir_, roll_speed_);
        }
    } else if (planted_) {
        operator_velocity_ = {};
    } else if (halt_ != Halt::none && !peeking_) {
        // Rolling to their stop, braking gently (harder only if a drop is close).
        const float remaining = (halt_point_[0] - operator_[0]) * halt_dir_[0] + (halt_point_[2] - operator_[2]) * halt_dir_[2];
        if (remaining <= 0.02f || halt_speed_ <= 0.05f) {
            halt_speed_ = 0.0f;
            operator_velocity_ = {};
            halt_stopped_ = true;
        } else {
            const float brake = std::clamp(halt_speed_ * halt_speed_ / (2.0f * remaining), 1.0f, 12.0f);
            halt_speed_ = std::max(0.0f, halt_speed_ - brake * dt);
            operator_ = add(operator_, scale(halt_dir_, std::min(halt_speed_ * dt, remaining)));
            operator_velocity_ = scale(halt_dir_, halt_speed_);
        }
    } else {
        // At a waiting spot (the end of the ledge, the foot of one): ride there and stop.
        const bool stopping = edge_ || holding_ || post_ != Post::none || peeking_ || halt_ != Halt::none;
        const float omega = post_ != Post::none || peeking_ ? 1.8f : mix(mix(3.2f, 2.0f, reaction), 4.5f, lunge_);
        // Never overtaking you: closing on the line of your shoulders, they ease off in time (no more
        // than about 6 m/s/s of braking to stop half a metre behind it).
        if (speed > 1.0f && post_ != Post::rail_end) {
            const float gap = -((operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2]) - 0.5f + lead;
            const float closing = (operator_velocity_[0] - t.velocity[0]) * heading[0] + (operator_velocity_[2] - t.velocity[2]) * heading[2];
            const float most = std::sqrt(12.0f * std::max(gap, 0.0f));
            if (closing > most) {
                operator_velocity_[0] -= heading[0] * (closing - most);
                operator_velocity_[2] -= heading[2] * (closing - most);
            }
        }
        for (std::size_t axis : {std::size_t{0}, std::size_t{2}})
            spring(operator_[axis], operator_velocity_[axis], desired[axis],
                   stopping ? 0.0f : std::clamp(desired_rate[axis], -25.0f, 25.0f), omega, 1.0f, dt);
        // A person on a board: they speed up a push at a time (about 3 m/s/s), brake harder, and are
        // no rocket. The spring's wishes are cut to that.
        if (dt > 0.0f) {
            Vec3 v{(operator_[0] - before[0]) / dt, 0.0f, (operator_[2] - before[2]) / dt};
            const Vec3 change = sub(v, velocity_before);
            const float faster = flat_length(v) > flat_length(velocity_before) ? 1.0f : 0.0f;
            // Pushing off when you push off: they speed up as you do (and a little more while they are
            // behind where they want to be), so the gap stays about the same. Just placed, they are
            // given a moment to get up to your speed.
            const bool settling_in = now - placed_at_ < 1.5;
            const float behind_by = std::max(0.0f, flat_length(sub(desired, before)) - 0.5f);
            const float push = 3.2f + std::max(0.0f, skater_accel_) * 1.1f + std::min(behind_by * 1.5f, 3.0f);
            // Speed is only gained with a push: in the kick of each stride (the dip of the camera),
            // a step faster each time, never a sudden surge. Between kicks they roll. (Down a hill
            // the slope gives them speed too; just placed, or lunging in, they are let go.)
            const float stride_now = std::fmod(push_phase_, 1.0f);
            const bool kicking = stride_now > 0.0f && stride_now < 0.4f;
            // (Already as close as they mean to be: no push, they roll and drop back to it.)
            const float gap_now = flat_length(sub(before, t.position)), gap_meant = flat_length(sub(desired, t.position));
            // (Out of place to the side, round you, they push to get there too: through a grind, up beside you.)
            const bool want_ground = gap_now > gap_meant - 0.2f || (behind_by > 0.6f && grind_weight_ > 0.3f);
            // (Racing up beside you on a grind: hard, quick pushes.)
            // (On a grind or a slide: from the moment you lock in, quick hard pushes until they are up
            // level with you, so they are beside you by the end of it.)
            {
                const Vec3 heading_now{t.heading_x, 0.0f, t.heading_z};
                const float level_with = (before[0] - t.position[0]) * heading_now[0] + (before[2] - t.position[2]) * heading_now[2];
                sprinting_ = (grinding_ || grind_weight_ > 0.3f) && halt_ == Halt::none && (behind_by > 0.4f || level_with < -0.8f);
                // Knocked back (brushed something, slowed for it) or just dropped behind: quick, hard
                // pushes until they are back where they mean to be, never a slow drift back.
                if (halt_ == Halt::none && post_ == Post::none && !planted_ && gap_now > gap_meant + 0.8f && behind_by > 0.8f) sprinting_ = true;
            }
            float gain = kicking && (want_ground || sprinting_) ? push * (sprinting_ ? 3.5f : 2.2f) : 0.15f;
            // (Out of place at a crawl: a shuffle of the feet gets them going between pushes.)
            if (behind_by > 0.6f && flat_length(velocity_before) < 1.5f && !rest_have_) gain = std::max(gain, 2.5f);
            // On foot they run with you: speeding up a stride at a time, round corners at a run.
            if (on_foot_) gain = gap_now > gap_meant - 0.2f ? 4.5f : 0.5f;
            if (floor_rate_ < -0.3f) gain = std::max(gain, std::min(-floor_rate_ * 2.0f, 4.0f));
            if (lunge_ > 0.1f || settling_in) gain = std::max(6.0f, push);
            // Speeding up takes a push; turning does not: a carve, the board leaning over, about as
            // hard as a skater can carve (the way they ride turned without losing pace). So the change
            // is split: along the way they ride (faster or slower), and across it (turning).
            const float was = flat_length(velocity_before);
            if (was > 0.5f) {
                const Vec3 along_dir = scale(velocity_before, 1.0f / was);
                const float along = change[0] * along_dir[0] + change[2] * along_dir[2];
                Vec3 across = sub(change, scale(along_dir, along));
                across[1] = 0.0f;
                const float along_ok = std::clamp(along, -7.0f * dt, (faster > 0.0f ? gain : 7.0f) * dt);
                const float across_len = flat_length(across), across_most = 9.0f * dt;
                if (across_len > across_most) across = scale(across, across_most / across_len);
                v = add(velocity_before, add(scale(along_dir, along_ok), across));
            } else {
                const float allowed = (faster > 0.0f ? gain : 7.0f) * dt;
                const float changed = flat_length(change);
                if (changed > allowed) v = add(velocity_before, scale(change, allowed / changed));
            }
            // (As fast as you have been going lately, and a good bit more while they are well behind:
            // a moment's slow reading of your speed never caps them under your pace.)
            recent_speed_ = std::max(speed, recent_speed_ - 1.5f * dt);
            const float pace = std::max(speed, recent_speed_);
            float top = on_foot_ ? std::max(4.0f, pace * 1.15f + 0.8f) : std::max(7.5f, pace * 1.2f + 1.0f);
            if (!on_foot_ && gap_now > gap_meant + 2.5f) top *= 1.25f;
            if (const float v_now = flat_length(v); v_now > top) v = scale(v, top / v_now);
            // Easing off before a drop they may have to stop at (and at a jump you are still in the air
            // over): never faster than they could stop from, gently, before it.
            const auto cap_towards = [&](const Vec3 &point, float margin, float decel) {
                const Vec3 to_point{point[0] - before[0], 0.0f, point[2] - before[2]};
                const float dist = flat_length(to_point);
                if (dist < 0.01f) return;
                const Vec3 dir = scale(to_point, 1.0f / dist);
                const float towards = v[0] * dir[0] + v[2] * dir[2];
                const float most_speed = std::sqrt(2.0f * decel * std::max(0.0f, dist - margin));
                // (Eased into: no faster towards it than allowed, but never braking harder than firmly,
                // about 6 m/s/s, to get under it.)
                const float towards_was = velocity_before[0] * dir[0] + velocity_before[2] * dir[2];
                const float allowed_now = std::max(most_speed, towards_was - 6.0f * dt);
                if (towards > allowed_now) { v[0] -= dir[0] * (towards - allowed_now); v[2] -= dir[2] * (towards - allowed_now); }
            };
            if (caution_) cap_towards(caution_point_, 0.8f, 3.5f);
            if (holding_) cap_towards(edge_point_, 0.5f, 5.0f);
            if (post_ == Post::ledge) cap_towards(ledge_face_, 0.9f, 6.0f);
            operator_velocity_ = v;
            operator_ = Vec3{before[0] + v[0] * dt, 0.0f, before[2] + v[2] * dt};
        }
        // Still past it (you braked hard, or turned across them): they drop back, at a rider's pace.
        if (speed > 1.0f && post_ != Post::rail_end) {
            const float past = (operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2] + 0.3f - lead;
            if (past > 0.0f) {
                const float back = std::min(past, 3.0f * dt);
                operator_[0] -= heading[0] * back;
                operator_[2] -= heading[2] * back;
            }
        }
    }
    // Stopping at an edge: never past it, however fast they were riding.
    if (edge_ || holding_) {
        const float past = (operator_[0] - edge_point_[0]) * edge_dir_[0] + (operator_[2] - edge_point_[2]) * edge_dir_[2];
        if (past > 0.0f) {
            operator_[0] -= edge_dir_[0] * past;
            operator_[2] -= edge_dir_[2] * past;
            const float v = operator_velocity_[0] * edge_dir_[0] + operator_velocity_[2] * edge_dir_[2];
            if (v > 0.0f) { operator_velocity_[0] -= edge_dir_[0] * v; operator_velocity_[2] -= edge_dir_[2] * v; }
            halt_speed_ = 0.0f;
        }
    }
    // Waiting behind a ledge: never nosed up against its face.
    if (post_ == Post::ledge) {
        const float in_front = -((operator_[0] - ledge_face_[0]) * ledge_back_[0] + (operator_[2] - ledge_face_[2]) * ledge_back_[2]) + 0.7f;
        if (in_front > 0.0f) {
            const float step = std::min(in_front, 2.0f * dt + 0.02f);
            operator_[0] += ledge_back_[0] * step;
            operator_[2] += ledge_back_[2] * step;
        }
    }
    // Never too close to you: you rolled up on them, they back off, unhurried.
    if (!planted_) {
        const Vec3 rel{operator_[0] - t.position[0], 0.0f, operator_[2] - t.position[2]};
        const float gap = flat_length(rel);
        if (gap < min_gap - 0.05f) {
            const Vec3 out = gap > 0.05f ? scale(rel, 1.0f / gap) : scale(heading, -1.0f);
            const float step = std::min(min_gap - gap, 2.0f * dt);
            operator_[0] += out[0] * step;
            operator_[2] += out[2] * step;
        }
    }
    if (cast_ && (!planted_ || rolling_stop_)) {
        // Each step is checked against the world: real ground under it, nothing in the way a body wide
        // (either shoulder, at the knee), nothing a board would not roll up or off. Blocked, the filmer
        // slides along what stopped them, or stays.
        const bool in_ollie = now < ollie_until_;
        const auto try_step = [&](const Vec3 &to) -> bool {
            auto g = ground_at(to[0], to[2], (in_ollie ? ollie_base_ + ollie_h_ : operator_ground_) + 0.5f, true); // from well above: a rise is found, never missed
            // In the air over what they ollie: its top is passed over, not ridden onto.
            bool landing_up = false;
            if (in_ollie && ollie_gap_) {
                // Over a gap: the hole under them flown over; the far side landed on (up onto it too).
                if (!g || *g < ollie_base_ - 0.3f) g = std::max(operator_ground_, ollie_base_);
                else if (*g > operator_ground_ + 0.3f && *g <= ollie_land_ + 0.1f) landing_up = true;
            } else if (in_ollie && g && *g > ollie_base_ + 0.06f && *g <= ollie_base_ + ollie_h_ + 0.08f) {
                g = ollie_base_;
            }
            if (!g || (!landing_up && !step_allowed(to, *g, s.stop_at_drops))) return false;
            const Vec3 move{to[0] - before[0], 0.0f, to[2] - before[2]};
            const float l = flat_length(move);
            if (l > 1e-4f) {
                // Looked for a little ahead of the step, so the body never ends up against it.
                const Vec3 dir = scale(move, 1.0f / l);
                const Vec3 reach = add(to, scale(dir, 0.3f));
                const Vec3 across{-dir[2] * 0.22f, 0.0f, dir[0] * 0.22f};
                // Ollying up onto the ledge after you: looked along over its top, not into its face.
                const bool popping = join_ok_ && flat_length(sub(to, join_point_)) < 1.6f;
                float low_from = popping ? std::max(operator_ground_, join_to_) : operator_ground_;
                float low_to = popping ? std::max(*g, join_to_) : *g;
                if (in_ollie) low_from = low_to = std::max(low_from, ollie_base_ + ollie_h_ + 0.05f); // (looked along over its top)
                if (obstructed(Vec3{before[0], low_from + 0.25f, before[2]}, Vec3{reach[0], low_to + 0.25f, reach[2]}, 0.25f, true)) return false;
                for (const float shoulder : {1.0f, -1.0f})
                    if (obstructed(Vec3{before[0] + across[0] * shoulder, low_from + 0.25f, before[2] + across[2] * shoulder},
                                   Vec3{reach[0] + across[0] * shoulder, low_to + 0.25f, reach[2] + across[2] * shoulder}, 0.25f, true))
                        return false;
            }
            operator_ground_ = *g;
            return true;
        };
        const bool refused = flat_length(sub(operator_, before)) > 0.0005f && !try_step(operator_);
        if (refused) {
            // Nosed up against something (a planter, a ledge's end, a trunk): step round it, out to the
            // side of the line they want, at a walking pace. Failing that, slide along it, or stay.
            const Vec3 moved = sub(operator_, before);
            const float l = flat_length(moved);
            const Vec3 across{-moved[2] / l, 0.0f, moved[0] / l};
            const float which = (desired[0] - before[0]) * across[0] + (desired[2] - before[2]) * across[2] >= 0.0f ? 1.0f : -1.0f;
            // (A small step at a walking pace, and it leaves them no faster sideways than that: a
            // shuffle round the corner, not a throw off it.)
            const float sidestep = std::clamp(l * 0.5f, 1.5f * dt, 0.06f);
            const Vec3 round = add(before, scale(across, which * sidestep));
            const Vec3 round_other = add(before, scale(across, -which * sidestep));
            const Vec3 along_x{operator_[0], 0, before[2]}, along_z{before[0], 0, operator_[2]};
            const float shuffle = std::min(sidestep / std::max(dt, 1e-3f), 1.5f);
            if (try_step(round)) { operator_ = round; operator_velocity_ = scale(across, which * shuffle); }
            // (Sliding along it, the blocked way's speed is shed over a few ticks, not in one: no jolt.)
            else if (try_step(along_x)) { operator_ = along_x; operator_velocity_[2] *= 0.6f; }
            else if (try_step(along_z)) { operator_ = along_z; operator_velocity_[0] *= 0.6f; }
            else if (try_step(round_other)) { operator_ = round_other; operator_velocity_ = scale(across, -which * shuffle); }
            else { operator_ = before; operator_velocity_ = scale(operator_velocity_, 0.4f); }
        }
        operator_[1] = 0;
        // Held up behind something (a car, a bin) while you ride off: rather than a chase to catch up,
        // a cut back to the usual spot behind you.
        const float made = dt > 0.0f ? flat_length(sub(operator_, before)) / dt : 0.0f;
        const bool held_up = (refused || made < speed * 0.5f) && flat_length(sub(t.position, operator_)) > std::max(5.0f, distance * 2.0f) && speed > 1.5f && !in_ollie;
        // (A cut only with you out of sight behind it: in view, they push round and on after you.)
        if (held_up && unseen_since_ >= 0 && !planted_ && halt_ == Halt::none && post_ == Post::none && (!peeking_ || flat_length(sub(t.position, operator_)) > 8.0f)) {
            if (stuck_since_ < 0.0) stuck_since_ = now;
            else if (now - stuck_since_ > 0.8) {
                operator_have_ = false; // placed afresh next tick, straight in behind you
                stuck_since_ = -1.0;
            }
        } else {
            stuck_since_ = -1.0;
        }
    } else if (!cast_) {
        operator_ground_ = ground; // no world to ask: the level from the trail
    }
    // The ground under them: rising, taken at once, so the lens never sinks into it; falling, followed
    // closely (riding down a hill they stay down by the ground, never floating over it like a drone),
    // only an odd one-tick answer is smoothed over.
    {
        const float previous = operator_floor_;
        if (operator_ground_ > operator_floor_) operator_floor_ = operator_ground_;
        else operator_floor_ += (operator_ground_ - operator_floor_) * ease(dt, 0.05f);
        if (dt > 0.0f) floor_rate_ += ((operator_floor_ - previous) / dt - floor_rate_) * ease(dt, 0.1f);
    }
    // Just up onto something (ollied up after you, hopped a step), or the camera raised to see you:
    // settled where they are, they keep to their side for a few seconds (no swapping sides on it).
    if (operator_floor_ - lens_floor_seen_ > 0.25f || joining_) settled_until_ = now + 3.0;
    lens_floor_seen_ = operator_floor_;
    // Stair stop: once stopped the camera rises a little, to see over the lip and down the set.
    spring(stop_lift_, stop_lift_rate_, framed_ ? std::clamp(s.stop_lift, 0.0f, 1.5f) : 0.0f, 0.0f, 4.0f, 1.0f, dt);
    // You going down off something they will not huck (in the air over it, or landed below it): the
    // camera rises as you fall, the further below the more, so you never drop out of the frame.
    if ((halt_ == Halt::drop || holding_ || edge_) && operator_have_) {
        const float below = operator_floor_ - world(t.position[1]);
        if (below > 0.4f && flat_length(sub(t.position, operator_)) < 25.0f) {
            const float need = std::min(raise_ceiling, height + below * 0.6f + 0.2f) - height;
            // Raised steadily, not snapped up. Off something big (more than about 4 m to the ground
            // under you), not until you have been in the air half a second, then quicker the longer
            // you fall.
            const auto under = t.airborne && cast_ && world_answers() ? ground_at(t.position[0], t.position[2], world(t.position[1]) + 0.3f) : std::optional<float>{};
            const float drop = under ? operator_floor_ - *under : below;
            const float airtime = t.airborne ? static_cast<float>(now - t.air_since) : 1.0f;
            float rate = 1.5f;
            if (drop > 4.0f && t.airborne) rate = airtime < 0.5f ? 0.0f : 0.5f + 1.6f * (airtime - 0.5f);
            if (need > wall_raise_target_) {
                wall_raise_target_ = std::min(need, wall_raise_target_ + rate * dt);
                if (hidden_since_ < 0.0) hidden_since_ = now;
                seen_low_since_ = -1.0;
            }
        }
    }
    // The camera raised: a little over a grind (or hardly at all, pushed out for the landing), over the
    // ledge they wait behind, and to see you over a wall or a fence. Gently, at a person's pace.
    {
        // Through a grind the camera comes up to about the height of the board on the rail (a touch
        // over it, so the board shows on it, not hidden behind it), no higher: on a flat bar a little,
        // down a handrail as it goes, never towering over you.
        const float board_over = std::clamp(world(t.position[1]) - operator_floor_, 0.0f, 1.6f);
        const float grind_lift = grind_weight_ * std::clamp(board_over + 0.12f - height, 0.0f, 1.2f);
        const float ledge_lift = post_ == Post::ledge ? ledge_lift_ : 0.0f;
        // Something low coming up in front of the lens (a manual pad, a ledge, a planter, a kerb-high
        // box): the camera goes up over it before it gets there, whatever height it is set to, so it
        // never bumps into it. (Too tall to see over, it is a wall: the way round is the planner's.)
        float guard = 0.0f;
        if (cast_ && world_answers()) {
            const float v = flat_length(operator_velocity_);
            Vec3 way = v > 0.3f ? scale(Vec3{operator_velocity_[0], 0.0f, operator_velocity_[2]}, 1.0f / v)
                                : Vec3{std::sin(aim_.yaw), 0.0f, std::cos(aim_.yaw)};
            probe_budget_ += 4;
            // (A step up: sudden, not the gentle rise of a bank or a hill, which they ride up anyway.)
            // (Steeper than about 40 degrees between two looks, and more than a kerb: a step.)
            float top = operator_floor_, last = operator_floor_, last_out = 0.0f;
            for (const float out : {0.0f, 0.35f, 0.75f, 0.75f + std::min(v, 8.0f) * 0.15f}) {
                const auto g = ground_at(operator_[0] + way[0] * out, operator_[2] + way[2] * out, operator_floor_ + raise_ceiling - 0.9f);
                if (!g) continue;
                const float rise = *g - last, apart = std::max(out - last_out, 0.2f);
                if (*g < operator_floor_ + raise_ceiling - 0.3f && rise > 0.15f && rise / apart > 0.85f) top = std::max(top, *g);
                last = *g;
                last_out = out;
            }
            if (top > operator_floor_ + 0.1f) guard = std::min(raise_ceiling, top - operator_floor_ + 0.3f) - height;
        }
        guard_ = std::max(0.0f, guard);
        // You stepped off your board: they hop off theirs, and the camera comes up a little (a person
        // on foot, not down by a board).
        spring(foot_lift_, foot_lift_rate_, on_foot_ ? 0.35f : 0.0f, 0.0f, 3.0f, 1.0f, dt);
        float lift_target = std::max({grind_lift, ledge_lift, wall_raise_target_, guard_});
        // (A zoom of their own still going in: the camera is not lowered until it is done.)
        if (zoom_moving_ && auto_zooming_ && lift_target < lift_) lift_target = lift_;
        // (Up to see you over a wall: a touch quicker, as the arms go up.)
        const bool for_wall = (wall_raise_target_ > 0.0f && wall_raise_target_ >= lift_target - 1e-3f) || (guard_ > 0.0f && guard_ >= lift_target - 1e-3f);
        spring(lift_, lift_rate_, lift_target, 0.0f, for_wall ? 6.5f : 1.8f, 1.0f, dt);
        lift_rate_ = std::clamp(lift_rate_, -1.5f, for_wall ? 3.6f : 0.8f); // (back down moderately quickly)
        lift_ = std::max(0.0f, lift_);
        wall_raise_ = wall_raise_target_ > 0.0f ? std::min(lift_, wall_raise_target_) : 0.0f;
        // Through an air the hands come up a touch and back: a soft rise and fall with you, not a jolt.
        // Ollying up onto a ledge after you: the camera goes up with them just before, not after.
        float air_target = t.airborne && lunge_ < 0.2f ? 0.08f : 0.0f;
        if (join_ok_ && operator_floor_ < join_to_ - 0.2f && flat_length(sub(operator_, join_point_)) < 1.4f)
            air_target = std::max(air_target, join_to_ - operator_floor_ + 0.05f);
        spring(air_lift_, air_lift_rate_, air_target, 0.0f, 5.0f, 1.0f, dt);
    }
    // (The lift over something and the hop up after you onto it are the same lift: the higher of them.)
    // Ollying over something after you: the camera up quickly with the pop, over it, and down again.
    float ollie_lift = 0.0f;
    if (now < ollie_until_ && ollie_until_ > ollie_pop_at_) {
        const float u = static_cast<float>((now - ollie_pop_at_) / (ollie_until_ - ollie_pop_at_));
        ollie_lift = (ollie_h_ + 0.12f) * std::sin(3.14159265f * std::pow(std::clamp(u, 0.0f, 1.0f), 0.65f));
    }
    const float extra = stop_lift_ + std::max(lift_ + std::min(air_lift_, 0.08f), air_lift_) + std::max(0.0f, foot_lift_);
    spring(operator_height_, operator_height_rate_, operator_floor_ + height + extra, floor_rate_, 5.5f, 0.9f, dt);
    operator_height_ = std::clamp(operator_height_, operator_floor_ + lens_clearance,
                                  operator_floor_ + std::max(height + extra + 0.15f, std::min(raise_ceiling, height + 0.15f)));
    const float operator_speed = flat_length(operator_velocity_);
    // Pushing to keep up: a dip and a nod every stride.
    const float behind = flat_length(sub(desired, operator_));
    pushing_ = !planted_ && halt_ == Halt::none && !edge_ && speed > 1.5f && (behind > 0.6f || operator_speed < speed - 0.6f || operator_speed > flat_length(velocity_before) + 0.5f * dt);
    if (halt_ == Halt::none && post_ != Post::none && operator_speed > 1.5f && behind > 1.0f) pushing_ = true;
    if (on_foot_ && operator_speed > 1.0f) pushing_ = true; // (running: a step every 0.4 s)
    if (sprinting_) pushing_ = true;
    // (Out of place by more than a body, you slow or not (you swung round, a 180): a push to get there.)
    if (!planted_ && halt_ == Halt::none && !edge_ && !rest_have_ && behind > 1.0f) pushing_ = true;
    if (pushing_ || std::fmod(push_phase_, 1.0f) > 0.02f) push_phase_ += dt / (on_foot_ ? 0.4f : sprinting_ ? 0.65f : 0.95f);
    const float stride = std::fmod(push_phase_, 1.0f);
    const float dip = stride < 0.4f ? std::sin(stride / 0.4f * pi) : 0.0f;
    // (The ollie's rise and fall straight on the lens: it goes with the board, not eased in.)
    Vec3 eye{operator_[0], std::max(operator_height_ - dip * 0.03f * (0.4f + human), operator_floor_ + lens_clearance) + ollie_lift, operator_[2]};
    // The lens travels along the path the filmer rode, at their eased speed: a step the world refused
    // for a tick (and the catch-up after it) is never a jolt in the picture, and as the path is ground
    // they rode, it never cuts a corner over something.
    {
        const Vec3 here{operator_[0], operator_floor_, operator_[2]};
        if (!eye_have_ || lens_path_.empty() || flat_length(sub(here, lens_path_.back().at)) > 2.0f) {
            lens_path_.clear();
            lens_path_.push_back({here, 0.0f});
            lens_at_ = 0.0f;
            lens_speed_ = 0.0f;
            eye_have_ = true;
        } else if (flat_length(sub(here, lens_path_.back().at)) > 0.002f) {
            lens_path_.push_back({here, lens_path_.back().along + flat_length(sub(here, lens_path_.back().at))});
        } else {
            lens_path_.back().at[1] = operator_floor_;
        }
        const float end = lens_path_.back().along;
        const float body_speed = dt > 0.0f ? flat_length(sub(here, eye_last_)) / dt : 0.0f;
        eye_last_ = here;
        // Eased speed, a hand's length behind at a roll (so a stalled tick has slack to ride through),
        // and a pull on the gap, so they never fall further behind and close it when stopped.
        lens_speed_ += (std::min(body_speed, 25.0f) - lens_speed_) * ease(dt, 0.3f);
        const float slack = std::min(0.25f, lens_speed_ * 0.05f);
        lens_at_ = std::min(end, lens_at_ + std::max(0.0f, lens_speed_ + (end - lens_at_ - slack) * 2.0f) * dt);
        if (end - lens_at_ > 0.6f) lens_at_ = end - 0.6f; // never far off the body
        while (lens_path_.size() > 2 && lens_path_[1].along <= lens_at_) lens_path_.pop_front();
        Vec3 at = lens_path_.back().at;
        for (std::size_t i = 1; i < lens_path_.size(); ++i)
            if (lens_path_[i].along >= lens_at_) {
                const auto &p0 = lens_path_[i - 1], &p1 = lens_path_[i];
                const float span = p1.along - p0.along;
                at = lerp(p0.at, p1.at, span > 1e-5f ? std::clamp((lens_at_ - p0.along) / span, 0.0f, 1.0f) : 1.0f);
                break;
            }
        eye[0] = at[0];
        eye[2] = at[2];
        // The height above the ground carried to where the lens is (a hand's length back along the
        // path): over a hump the lens rises and falls with the ground under it, not with the ground
        // under their feet a moment ahead (the two pulling against each other was a jolt).
        const float above = eye[1] - operator_floor_;
        eye[1] = at[1] + above;
        lens_ground_ = at[1];
        // And the ground right under the lens, asked every tick: never into a curve, a bank, a kerb.
        if (cast_ && world_answers())
            if (const auto g = ground_at(eye[0], eye[2], std::max(at[1], operator_floor_) + 0.6f, true); g && *g > lens_ground_ - 0.5f)
                lens_ground_ = std::max(lens_ground_, *g);
        // A rise just ahead of the lens steeper than a hill (the start of a hump, a bank, a kerb): the
        // lens starts up before it gets there, so it rides up and over in one curve, never a kink as
        // it meets it.
        const float moving = flat_length(operator_velocity_);
        if (cast_ && world_answers() && moving > 0.5f) {
            const Vec3 dir = scale(Vec3{operator_velocity_[0], 0.0f, operator_velocity_[2]}, 1.0f / moving);
            float ahead_floor = -1e9f;
            for (const float secs : {0.1f, 0.2f, 0.32f}) {
                const float along = std::min(moving * secs, 2.0f);
                if (const auto g = ground_at(eye[0] + dir[0] * along, eye[2] + dir[2] * along, std::max(at[1], operator_floor_) + 0.6f); g && *g > lens_ground_ - 0.5f)
                    ahead_floor = std::max(ahead_floor, *g - along * 0.2f);
            }
            if (ahead_floor > lens_ground_) eye[1] = std::max(eye[1], ahead_floor + (above > 0.0f ? std::min(above, 0.4f) : lens_clearance));
        }
    }
    // Never far down below you (into water, under the map, down a hole the ground gives no answer
    // for): no lower than a long way under where you are, closer the nearer you are.
    if (halt_ != Halt::drop) {
        const float near = flat_length(sub(Vec3{eye[0], 0.0f, eye[2]}, Vec3{t.position[0], 0.0f, t.position[2]}));
        lens_ground_ = std::max(lens_ground_, world(t.position[1]) - (1.6f + near * 0.5f));
    }
    // The lens height, finally, eased (no hitch where one lift hands over to another), and never under
    // the ground.
    if (!eye_y_have_ || std::abs(eye_y_ - eye[1]) > 3.0f) { eye_y_ = eye_y_target_ = eye[1]; eye_y_rate_ = eye_y_target_rate_ = 0.0f; eye_y_have_ = true; }
    // (Led by how fast the target itself goes up or down, a hill's slope: no lag riding down one.)
    if (dt > 0.0f) eye_y_target_rate_ += (std::clamp((eye[1] - eye_y_target_) / dt, -15.0f, 15.0f) - eye_y_target_rate_) * ease(dt, 0.15f);
    eye_y_target_ = eye[1];
    // (Close over the ground and it rising under the lens: stiffer, so it never meets the ground and
    // stops dead on it.)
    const float margin = eye_y_ - (lens_ground_ + lens_clearance);
    const float stiff = margin < 0.08f && eye_y_target_rate_ > 0.2f ? mix(28.0f, 14.0f, std::clamp(margin / 0.08f, 0.0f, 1.0f)) : 14.0f;
    spring(eye_y_, eye_y_rate_, eye[1], eye_y_target_rate_, stiff, 1.0f, dt);
    if (eye_y_ < lens_ground_ + lens_clearance) { eye_y_ = lens_ground_ + lens_clearance; eye_y_rate_ = std::max(eye_y_rate_, 0.0f); }
    eye[1] = eye_y_;
    // Can the filmer see you? Hidden behind something not too tall (you went over a fence or a wall,
    // dropped down behind it, slammed behind it), after a beat the camera is lifted, gently, as far as
    // it takes to see you again (your body or at least your head), up to about 7 ft. If no height up to
    // that sees you from where they are, they roll up closer to whatever is in the way (never past a
    // drop, never into it) and look over from there. Once you are in sight from down low again for a
    // moment, back down; and they ride on as usual.
    if (cast_) {
        const Vec3 head{centre[0], centre[1] + 0.65f, centre[2]};
        const auto sees = [&](const Vec3 &from, bool always) {
            return !blocked(from, centre, nullptr, always) || !blocked(from, head, nullptr, always);
        };
        const bool hidden = !sees(eye, true);
        if (!hidden) unseen_since_ = -1.0;
        else if (unseen_since_ < 0) unseen_since_ = now;
        if (hidden && hidden_since_ < 0.0) {
            hidden_since_ = now;
            wall_delay_ = 0.05f + 0.1f * unit(random_);
        } else if (!hidden && wall_raise_target_ <= 0.0f && !peeking_) {
            hidden_since_ = -1.0;
        }
        if (now >= wall_next_) {
            wall_next_ = now + 0.2;
            const float base_eye = operator_floor_ + height;
            const float away = flat_length(sub(t.position, operator_));
            const float raised_to = operator_floor_ + height + wall_raise_target_;
            if (hidden && hidden_since_ >= 0.0 && now - hidden_since_ > wall_delay_ && away < 30.0f) {
                // Higher (than it is already going to), from where they are.
                bool found = false;
                for (const float above : {0.5f, 0.8f, 1.1f, 1.4f, 1.7f, 1.95f, raise_ceiling}) {
                    const float h = operator_floor_ + above;
                    if (h <= std::max(base_eye, raised_to) + 0.05f) continue;
                    if (sees(Vec3{eye[0], h, eye[2]}, true)) {
                        wall_raise_target_ = std::min(raise_ceiling, above + 0.2f) - height;
                        found = true;
                        break;
                    }
                }
                if (!found && !sees(Vec3{eye[0], raised_to, eye[2]}, true)) {
                    // Not from here: closer to what is in the way, a metre at a time, up to just short
                    // of it, looking from 7 ft at most.
                    wall_raise_target_ = raise_ceiling - height;
                    Vec3 hit{};
                    const Vec3 top{eye[0], operator_floor_ + raise_ceiling, eye[2]};
                    if (blocked(top, centre, &hit, true) || blocked(Vec3{eye[0], operator_floor_ + 0.5f, eye[2]}, centre, &hit, true)) {
                        const Vec3 to{hit[0] - operator_[0], 0.0f, hit[2] - operator_[2]};
                        const float reach = flat_length(to) - 0.6f;
                        if (reach > 0.4f) {
                            const Vec3 dir = scale(to, 1.0f / (reach + 0.6f));
                            for (float step = std::min(1.0f, reach); step <= reach + 1e-3f; step += 1.0f) {
                                const Vec3 at = add(operator_, scale(dir, std::min(step, reach)));
                                const auto g = ground_at(at[0], at[2], operator_floor_ + 0.5f, true);
                                if (!g || std::abs(*g - operator_floor_) > 0.3f) break; // no ground at their level: not past here
                                if (sees(Vec3{at[0], *g + raise_ceiling, at[2]}, true) || step + 1.0f > reach + 1e-3f) {
                                    peek_spot_ = Vec3{at[0], 0.0f, at[2]};
                                    peeking_ = true;
                                    if (sees(Vec3{at[0], *g + raise_ceiling, at[2]}, true)) break;
                                }
                            }
                        }
                    }
                }
                seen_low_since_ = -1.0;
            } else if (wall_raise_target_ > 0.0f || peeking_) {
                // Raised (or gone closer): is there a view from down at the usual height again?
                if (sees(Vec3{eye[0], base_eye, eye[2]}, true)) {
                    if (seen_low_since_ < 0.0) seen_low_since_ = now;
                    else if (now - seen_low_since_ > 1.0) { wall_raise_target_ = 0.0f; hidden_since_ = -1.0; peeking_ = false; }
                } else {
                    seen_low_since_ = -1.0;
                }
                if (away > 35.0f) { wall_raise_target_ = 0.0f; hidden_since_ = -1.0; peeking_ = false; }
            }
        }
    }
    // The zoom: you slammed and look small in the frame (a long way off), in until you fill it; now and
    // then (about 4 in 10) when they stop by themselves, a slight zoom onto you. As much as it takes for
    // how far off you are (kept so as you move), at a steady, moderate pace. Held at least 3.5 seconds,
    // and only after they ride on (or you are up), back out at the same pace.
    {
        const float far_off = std::max(length(sub(centre, eye)), 0.5f);
        const float base_half = render_fov(s.lens) * 0.5f * degrees;
        const float share = std::atan(0.9f / far_off) / base_half; // half your height against half the frame
        const bool seen = unseen_since_ < 0.0;
        // In the shot and nothing in the way of your body (not just your head over a wall).
        const auto in_shot = [&] {
            if (!seen || !aim_.have) return false;
            const Vec3 to{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
            if (std::abs(wrap_angle(yaw_of(to[0], to[2]) - aim_.yaw)) > half_view(s) * 0.9f) return false;
            return !cast_ || !blocked(eye, centre, nullptr, true);
        };
        const bool far_enough = far_off > (fish ? 5.0f : 10.0f);
        // You slammed: zoomed in on you lying there, only if you are in the shot, nothing in the way.
        if (halt_ == Halt::slam && (halt_stopped_ || operator_speed < 0.3f) && !halt_zoom_rolled_) {
            halt_zoom_rolled_ = true;
            zoom_kind_ = 0;
            // (Now and then, 1 in 10, you in full view with nothing in the way: in on your face.)
            const bool clear = slam_zoom_ && in_shot();
            if (clear && far_off > 2.5f && unit(random_) < 0.1f) {
                zoom_kind_ = 3;
                zoom_since_ = now;
            } else if (clear && far_enough && share < 0.45f) {
                zoom_kind_ = 1;
                zoom_since_ = now;
            }
        }
        // Stopped at the top of a drop: zoomed in on you as you skate away from it (once you are a
        // way off, and still in the shot), before the shot is let go.
        // (Stopped at the top of a rail you grind down: a zoom on you only now and then, 4 in 10.)
        if (halt_ == Halt::drop && halt_for_grind_ && halt_stopped_ && zoom_kind_ == 0 && !halt_zoom_rolled_ && !halt_grind_zoom_rolled_) {
            halt_grind_zoom_rolled_ = true;
            if (unit(random_) >= 0.4f) halt_zoom_rolled_ = true;
        }
        if (halt_ == Halt::drop && halt_stopped_ && zoom_kind_ == 0 && !drifting_ && stop_zoom_ && !halt_zoom_rolled_) {
            const Vec3 rel{t.position[0] - eye[0], 0.0f, t.position[2] - eye[2]};
            const float away = flat_length(rel);
            const bool skating_away = !t.airborne && !subject.bailing && speed > 1.0f && (t.velocity[0] * rel[0] + t.velocity[2] * rel[2]) > 0.5f * speed * away;
            if (skating_away && far_off > (fish ? 4.0f : 7.0f) && share < 0.4f && in_shot()) {
                halt_zoom_rolled_ = true;
                zoom_kind_ = 2;
                zoom_since_ = now;
            }
        }
        // As far off as you are, so the pace: a turn of the ring that takes longer the further it has
        // to go, never a snap (a VX1000 zoomed by hand).
        zoom_pace_ = std::clamp(0.12f + far_off * 0.012f, 0.15f, 0.4f);
        if (zoom_kind_ == 3) zoom_pace_ = 0.22f; // (in on your face: a moderate, even pace)
        face_weight_ += ((zoom_kind_ == 3 ? 1.0f : 0.0f) - face_weight_) * ease(dt, zoom_kind_ == 3 ? 0.8f : 0.5f);
        if (zoom_kind_ != 0) {
            const bool still_on = halt_ != Halt::none || now - zoom_since_ < 3.5;
            if (!still_on) {
                zoom_kind_ = 0;
                auto_zoom_ = 1.0f;
            } else if ((seen || auto_zoom_ <= 1.0f) && !drifting_) {
                const float fill = zoom_kind_ == 1 ? 0.55f : 0.4f;
                const float most = zoom_kind_ == 1 ? (fish ? 2.0f : max_zoom) : (fish ? 1.5f : 2.5f);
                auto_zoom_ = std::clamp(fill / std::max(share, 0.02f), 1.0f, most);
                if (zoom_kind_ == 3) {
                    // Your face (about a hand and a half across) to a good third of the frame's height.
                    const float face_share = std::atan(0.13f / far_off) / base_half;
                    auto_zoom_ = std::clamp(0.3f / std::max(face_share, 0.01f), 1.0f, fish ? 3.0f : max_zoom);
                }
            }
        } else {
            auto_zoom_ = 1.0f;
        }
    }
    // Aim: at where the skater was a moment ago (reaction), a little off-centre (a person framing),
    // caught up hard when the skater is about to leave the frame (a filmer never loses a trick).
    Vec3 now_point = aim_point(t, s);
    // The height the framing follows is eased: up and down with an ollie as a soft rise and fall
    // (never out of the frame: the frame limits below still hold the board in it).
    if (!focus_have_) { focus_y_ = now_point[1]; focus_have_ = true; }
    focus_y_ += (now_point[1] - focus_y_) * ease(dt, t.airborne ? 0.4f : 0.15f);
    const float true_y = now_point[1];
    now_point[1] = focus_y_;
    history_.push_back({now, now_point});
    while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
    const Vec3 late = delayed_point(now, 0.04f + 0.16f * reaction * (0.5f + human));
    // Stopped by you (the left stick): the camera stays on the framing it had, held by hand (it does
    // not follow you round). Stopped by themselves: still, but panning with you.
    edge_locked_ = false;
    const bool frozen = planted_ && !rolling_stop_;
    // Stair stop: from the moment you stop them (still braking), they stop following you and frame the set.
    const bool framing_set = planted_;
    if (!frozen && !framing_set) {
        framed_ = pan_active_ = composing_ = false;
    } else if (!framed_ && aim_.have) {
        // Just stopped. Stair stop: the shot is framed on the set (the way they were riding), tilted
        // down over it from where the camera will have risen to, and held.
        framed_ = true;
        {
            set_yaw_ = rolling_stop_ ? yaw_of(stop_dir_[0], stop_dir_[2]) : aim_.yaw;
            const Vec3 from = rolling_stop_ ? Vec3{stop_point_[0], eye[1], stop_point_[2]} : eye;
            pan_pitch_ = stair_pan_pitch(Vec3{from[0], from[1] + std::clamp(s.stop_lift, 0.0f, 1.5f), from[2]}, set_yaw_, aim_.pitch, operator_floor_);
            pan_active_ = composing_ = true;
            compose_start_ = now;
        }
    }
    // The end of the shot: stopped by themselves and you rode a good way off, the pan carries on
    // slowly the way you were going and drifts down at the same pace, as a filmer lets the shot go.
    if (halt_ != Halt::none && halt_ != Halt::slam && aim_.have) {
        const Vec3 rel{t.position[0] - eye[0], 0.0f, t.position[2] - eye[2]};
        const float away = flat_length(rel);
        const bool leaving = away > 12.0f && (t.velocity[0] * rel[0] + t.velocity[2] * rel[2]) > 0.5f * away;
        // (A zoom of their own going in is finished first: only then is the camera let down.)
        if (!drifting_ && leaving && !(auto_zooming_ && zoom_moving_) && (zoom_kind_ == 0 || now - zoom_since_ > 1.0)) {
            drifting_ = true;
            drift_done_at_ = -1.0;
            drift_done_ = 0.0f;
            // The way the pan was going (the way you cross the frame).
            const float cross = rel[0] * t.velocity[2] - rel[2] * t.velocity[0];
            drift_sign_ = std::abs(aim_.yaw_rate) > 0.02f ? (aim_.yaw_rate > 0.0f ? 1.0f : -1.0f) : (cross > 0.0f ? -1.0f : 1.0f);
        } else if (drifting_ && away < 9.0f) {
            drifting_ = false;
        }
    }
    if ((frozen || framing_set) && composing_) {
        // Framing the set: a quick, smooth swing round to look along it and down over it, as a filmer
        // brings the camera onto the spot (the faintest settle as it lands), then held. It does not
        // follow you: you ride into the shot.
        steer_aim(dt, set_yaw_, pan_pitch_, 8.0f, 0.9f);
        const bool calm = std::abs(aim_.yaw_rate) + std::abs(aim_.pitch_rate) < 0.05f &&
                          std::abs(wrap_angle(aim_.yaw - set_yaw_)) < 0.02f && std::abs(aim_.pitch - pan_pitch_) < 0.02f;
        if (rolling_stop_) compose_start_ = now; // held from when they have come to a halt
        else if ((now - compose_start_ > 0.2 && calm) || now - compose_start_ > 1.5) {
            composing_ = pan_active_ = false;
            aim_.yaw_rate = aim_.pitch_rate = 0.0f;
        }
    } else if (frozen && pan_active_) {
        aim_.pitch += (pan_pitch_ - aim_.pitch) * ease(dt, 0.4f);
        aim_.pitch_rate = 0.0f;
    } else if (drifting_ && aim_.have) {
        // About 20 degrees a second, easing in: across for a while, and down, moderately quickly, to
        // the ground in front of them. Then a beat, and they are off after you again (a cut: in
        // behind you or beside you, as usual).
        const float rate = 20.0f * degrees * std::clamp(drift_done_ / (8.0f * degrees) + 0.3f, 0.3f, 1.0f);
        const float step = rate * dt;
        if (drift_done_ < 25.0f * degrees) aim_.yaw = wrap_angle(aim_.yaw + drift_sign_ * step);
        aim_.pitch = std::max(aim_.pitch - step, -1.15f);
        drift_done_ += step;
        aim_.yaw_rate = aim_.pitch_rate = 0.0f;
        if (aim_.pitch <= -1.149f && drift_done_at_ < 0.0) drift_done_at_ = now;
        if (drift_done_at_ >= 0.0 && now - drift_done_at_ > 0.6) {
            drift_done_at_ = -1.0;
            drift_over_ = true; // (the cut, from the next tick)
        }
    }
    if ((!frozen && !composing_ && !drifting_) || !aim_.have) {
        const Vec3 to_late = sub(late, eye), to_now = sub(now_point, eye);
        float want_yaw = yaw_of(to_late[0], to_late[2]);
        float want_pitch = frame_pitch(eye, late, t.airborne, 0.0f, s);
        // Through a grind, the board in the middle of the frame.
        if (board_weight_ > 0.01f) {
            const float board_pitch = std::atan2(late[1] + 0.05f - eye[1], std::max(flat_length(to_late), 0.3f));
            want_pitch = mix(want_pitch, board_pitch, board_weight_);
        }
        // In on your face (a slam, now and then): the framing goes up from your board to your face.
        if (face_weight_ > 0.01f) {
            const float face_y = world(t.position[1]) + (subject.bailing ? 0.25f : 1.5f);
            const float face_pitch = std::atan2(face_y - eye[1], std::max(flat_length(to_late), 0.3f));
            want_pitch = mix(want_pitch, face_pitch, face_weight_);
        }
        want_yaw += wobble_[2].sample(now * 0.4) * 3.5f * degrees * human;
        want_pitch += wobble_[3].sample(now * 0.17) * 2.5f * degrees * human;
        const float true_yaw = yaw_of(to_now[0], to_now[2]);
        const Vec3 real_point{now_point[0], true_y, now_point[2]};
        const float true_pitch = frame_pitch(eye, real_point, t.airborne, 0.0f, s);
        // How far off the skater is, against how much of the frame there is to the edge.
        const float view = half_view(s) * (fish ? 1.0f : 0.95f);
        const float off = aim_.have ? std::max(std::abs(wrap_angle(true_yaw - aim_.yaw)), std::abs(true_pitch - aim_.pitch)) : 0.0f;
        const float slack = view * mix(0.3f, 0.55f, human);
        const float catch_up = off > slack ? std::clamp((off - slack) / slack * 3.0f, 0.0f, 5.0f) : 0.0f;
        const float aim_omega = mix(9.0f, 4.2f, reaction) * (1.0f + catch_up) * (halt_ != Halt::none ? 0.7f : 1.0f);
        const float aim_damping = mix(0.95f, 0.62f, human);
        steer_aim(dt, want_yaw, want_pitch, aim_omega, aim_damping);
        // Never let them out of the frame: past 85% of the way to the edge sideways, or the board about to
        // leave the bottom or the top, the aim is dragged back in.
        if (aim_.have) {
            const float limit = view * 0.85f;
            const float dy = wrap_angle(true_yaw - aim_.yaw);
            if (std::abs(dy) > limit) aim_.yaw = wrap_angle(true_yaw - std::copysign(limit, dy));
            const float horizontal = std::max(flat_length(to_now), 0.3f);
            const float feet = std::atan2(true_y + 0.05f - eye[1], horizontal);
            if (face_weight_ < 0.3f) aim_.pitch = std::clamp(aim_.pitch, feet - 0.85f * half_view(s), feet + 0.85f * half_view(s));
        }
    }
    // Leaning into the turns the filmer rides.
    const float operator_yaw = operator_speed > 0.5f ? yaw_of(operator_velocity_[0], operator_velocity_[2]) : last_operator_yaw_;
    const float turn = dt > 0 ? wrap_angle(operator_yaw - last_operator_yaw_) / std::max(dt, 0.001f) : 0.0f;
    last_operator_yaw_ = operator_yaw;
    lean_ = mix(lean_, std::clamp(-turn * operator_speed * 0.012f, -0.12f, 0.12f), ease(dt, 0.25f));
    float yaw = aim_.yaw, pitch = aim_.pitch, roll = lean_;
    shake(now, operator_speed, human, yaw, pitch, roll, eye);
    eye[1] = std::max(eye[1], lens_ground_ + lens_clearance); // the hand's shake never takes it under
    Frame frame;
    frame.camera = build(eye, yaw, std::clamp(pitch, -1.45f, 1.45f), roll);
    frame.fov = render_fov(s.lens) / zoom_;
    frame.valid = valid_camera(frame.camera);
    return frame;
}

Frame Rig::film(double now, float dt, const Subject &self, const Subject *target, const StickInput &stick,
                const RigSettings &s) {
    if (!self.valid || !finite(self.position)) return {};
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    filming_ = true;
    track(now, dt, self, self_, false, true);
    const bool aiming_at = target && target->valid && finite(target->position);
    if (aiming_at) track(now, dt, *target, subject_, true, false);
    if (aiming_at != framing_) ff_have_ = false;
    framing_ = aiming_at;
    ease_zoom(dt, s);
    const float human = std::clamp(s.human, 0.0f, 1.0f) * 0.6f; // your own hands add the rest
    const auto &me = self_;
    const float speed = flat_length(me.velocity);
    const float heading = yaw_of(me.heading_x, me.heading_z);
    // The stick, with a curve: fine framing near the centre, a whip at the edge.
    const auto curve = [](float v) { return std::copysign(std::pow(std::abs(std::clamp(v, -1.0f, 1.0f)), 1.6f), v); };
    const float rate = 160.0f * degrees * std::clamp(s.sensitivity, 0.2f, 3.0f);
    const float sx = curve(stick.x), sy = curve(stick.y) * (s.invert_y ? -1.0f : 1.0f);
    const bool touched = std::abs(stick.x) > 0.01f || std::abs(stick.y) > 0.01f;
    if (touched) stick_idle_since_ = now;
    offset_yaw_ = wrap_angle(offset_yaw_ - sx * rate * dt);
    offset_pitch_ = std::clamp(offset_pitch_ + sy * rate * 0.7f * dt, -1.2f, 1.2f);
    float want_yaw, want_pitch;
    float omega = 16.0f, damping = 1.0f;
    // The camera is held out towards where it points, so turning it round never looks through you.
    // Held at chest height: the eye rises with you through an ollie, smoothed so a landing is a bump.
    if (!operator_have_) {
        operator_height_ = world(me.position[1]);
        operator_height_rate_ = 0;
        operator_have_ = true;
    }
    spring(operator_height_, operator_height_rate_, world(me.position[1]), 0.0f, 9.0f, 0.75f, dt);
    // Where your hands hold it (filmer_hold.cpp shapes the arm the same way, so the picture is taken
    // from the camera you are seen holding): level, out in front at the chest under the eyes; aimed
    // down, lower and a little closer, down to the stomach pointed at the ground; aimed up, up past
    // the face and over the head at the sky. One smooth sweep with the tilt, eased, never a jump.
    {
        const float pointing_y = std::sin(aim_.have ? aim_.pitch : 0.0f);
        film_up_ += (std::max(pointing_y, 0.0f) - film_up_) * ease(dt, 0.12f);
        film_down_ += (std::max(-pointing_y, 0.0f) - film_down_) * ease(dt, 0.12f);
    }
    const float shoulder = std::clamp(s.hold_shoulder, 0.6f, 1.6f);
    const float hold = shoulder - 0.18f * (1.0f - film_up_) + 0.55f * film_up_ - 0.45f * film_down_;
    const Vec3 body{me.position[0], operator_height_ + hold, me.position[2]};
    const float reach_yaw = aim_.have ? aim_.yaw : heading;
    const float reach = 0.48f * (1.0f - 0.45f * film_up_) * (1.0f - 0.15f * film_down_);
    Vec3 eye = add(body, Vec3{std::sin(reach_yaw) * reach, 0, std::cos(reach_yaw) * reach});
    if (cast_) {
        const Vec3 chest{me.position[0], operator_height_ + std::max(hold, 0.9f), me.position[2]};
        Vec3 hit{};
        if (blocked(chest, eye, &hit)) {
            const Vec3 to = sub(eye, chest);
            const float full = length(to);
            if (full > 0.01f) eye = add(chest, scale(to, std::max(0.0f, length(sub(hit, chest)) - 0.12f) / full));
        }
        if (const auto g = ground_at(eye[0], eye[2], world(me.position[1]) + 0.3f, true)) {
            // Over the ground under the lens: kerb or bank, it never goes into it.
            if (*g > film_floor_) film_floor_ = *g;
            else film_floor_ += (*g - film_floor_) * ease(dt, 0.12f);
            if (!film_floor_known_) { film_floor_ = *g; film_floor_known_ = true; }
            eye[1] = std::max(eye[1], film_floor_ + lens_clearance);
        }
    }
    float yaw_rate = 0.0f, pitch_rate = 0.0f;
    if (s.assist && aiming_at) {
        // Framing a skater: the aim keeps on them as a person would (a beat late), and the stick
        // shifts the framing; let go and it settles back on them. Worked out from where the camera is
        // held from (your body), not the lens out at arm's length (which swings with the aim, and
        // would chase itself). How fast their bearing moves is fed forward, so the aim turns with them
        // at their pace (less lag, no overshoot) and only small errors are corrected; the hand's shake
        // comes after, from shake().
        const Vec3 now_point = aim_point(subject_, s);
        history_.push_back({now, now_point});
        while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
        const Vec3 late = delayed_point(now, 0.03f + 0.12f * std::clamp(s.reaction, 0.0f, 1.0f));
        const Vec3 pivot{body[0], eye[1], body[2]};
        const Vec3 to = sub(late, pivot);
        want_yaw = yaw_of(to[0], to[2]) + offset_yaw_;
        want_pitch = frame_pitch(pivot, late, subject_.airborne, 0.0f, s) + offset_pitch_;
        if (!touched && now - stick_idle_since_ > 0.35) {
            const float k = ease(dt, 0.9f);
            offset_yaw_ = mix(offset_yaw_, 0.0f, k);
            offset_pitch_ = mix(offset_pitch_, 0.0f, k);
        }
        if (ff_have_ && dt > 0.0f) {
            const float raw_yaw = std::clamp(wrap_angle(want_yaw - ff_last_yaw_) / dt, -8.0f, 8.0f);
            const float raw_pitch = std::clamp((want_pitch - ff_last_pitch_) / dt, -4.0f, 4.0f);
            ff_yaw_rate_ += (raw_yaw - ff_yaw_rate_) * ease(dt, 0.2f);
            ff_pitch_rate_ += (raw_pitch - ff_pitch_rate_) * ease(dt, 0.2f);
        } else {
            ff_yaw_rate_ = ff_pitch_rate_ = 0.0f;
        }
        ff_last_yaw_ = want_yaw;
        ff_last_pitch_ = want_pitch;
        ff_have_ = true;
        // Half of it: all of it amplifies the small noise in where they are (measured on a trace:
        // half frames them closer than none, as smoothly).
        yaw_rate = ff_yaw_rate_ * 0.5f;
        pitch_rate = ff_pitch_rate_ * 0.5f;
        omega = mix(11.0f, 5.5f, std::clamp(s.reaction, 0.0f, 1.0f));
        damping = 1.0f;
    } else {
        ff_have_ = false;
        // Your own framing, held relative to the way you ride: turn and the camera turns with you.
        if (s.recenter && !touched && now - stick_idle_since_ > 0.6) {
            const float k = ease(dt, 0.8f);
            offset_yaw_ = mix(offset_yaw_, 0.0f, k);
            offset_pitch_ = mix(offset_pitch_, -0.12f, k);
        }
        want_yaw = heading + offset_yaw_;
        want_pitch = offset_pitch_;
    }
    // A filmer turns from the waist, not right round: the camera points no further than square off
    // either side of the way you face (from your 9 o'clock round the front to your 3).
    // The middle is the way your body faces (from your hips): on a board that is toe side, where your
    // feet point, not the nose; on foot, ahead.
    {
        constexpr float side = 1.5707963f;
        const float middle = body_yaw_ ? *body_yaw_ : heading;
        const float rel = wrap_angle(want_yaw - middle);
        // Pushing past the stop (the stick, or the assist after someone round behind you): said, so
        // that on foot your whole body can turn round after the camera (filmer.cpp).
        edge_push_ = std::abs(rel) > side + 2.0f * degrees ? (rel > 0.0f ? 1.0f : -1.0f) : 0.0f;
        if (std::abs(rel) > side) {
            want_yaw = middle + std::copysign(side, rel);
            yaw_rate = 0.0f;
            // Your own framing stops at the edge too (no winding on past it).
            if (!(s.assist && aiming_at)) offset_yaw_ = wrap_angle(want_yaw - heading);
        }
        offset_yaw_ = std::clamp(offset_yaw_, -side * 2.0f, side * 2.0f);
    }
    steer_aim(dt, want_yaw, want_pitch, omega, damping, yaw_rate, pitch_rate);
    const float turn = dt > 0 ? wrap_angle(heading - last_operator_yaw_) / std::max(dt, 0.001f) : 0.0f;
    last_operator_yaw_ = heading;
    lean_ = mix(lean_, std::clamp(-turn * speed * 0.012f, -0.12f, 0.12f), ease(dt, 0.25f));
    float yaw = aim_.yaw, pitch = aim_.pitch, roll = lean_;
    // Pushing: you can tell from your own speed picking up on the ground.
    pushing_ = !me.airborne && speed > 0.5f && me.velocity[1] > -0.2f;
    shake(now, speed, human, yaw, pitch, roll, eye);
    if (film_floor_known_) eye[1] = std::max(eye[1], film_floor_ + lens_clearance);
    Frame frame;
    frame.camera = build(eye, yaw, std::clamp(pitch, -1.45f, 1.45f), roll);
    frame.fov = render_fov(s.lens) / zoom_;
    frame.valid = valid_camera(frame.camera);
    return frame;
}
} // namespace dingosdk::filmer
