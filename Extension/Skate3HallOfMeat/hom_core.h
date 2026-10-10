#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Hall Of Meat scoring core. Pure logic with no game or UI dependencies, so it can be unit tested:
// feed it one Sample per client tick and it tracks a bail from the moment the skater wipes out
// until they are back on their feet, breaks bones from the hits each X-ray bone takes and scores
// the bail the way Skate 3's HUD does: 500 a bone, plus air, drop, bail time, speed and rotation.
namespace dingosdk::skate3_hom {

struct Sample {
    double time{}; // seconds, monotonic
    std::array<float, 3> position{};
    float speed{};    // metres per second over the ground
    float vertical{}; // metres per second, upward positive
    bool airborne{};
    std::uint32_t physics_state{};
    float heading{}; // degrees, 0..360
    // Middle of each X-ray bone in world space (hom_rig.h order), when the skeleton could be read.
    bool bones_valid{};
    bool upright{}; // head well above the hips
    bool lying{};   // head no higher than ~0.3 m above the hips: the body is down, not crouched
    std::array<std::array<float, 3>, 19> bone_centres{};
    bool vehicle{}; // the body touched a car or truck
};

// Skate 3's own HoM damage model (Sk8::Score::HoM in its attribute database): 25 body parts, each
// with up to six damage levels (bruised up to broken), each worth points once a hit reaches its
// impact, and links through which a part takes a share of a neighbour's hit.
inline constexpr std::size_t body_parts = 25;
struct DamageLevel {
    int points;
    float impact; // m/s a hit must reach
};
struct BodyLink {
    int part;     // -1: none
    float weight; // share of that part's hit this one takes
};
struct BodyPart {
    BodyLink links[4];
    DamageLevel levels[6]; // unused slots are {0, 0}
};
const std::array<BodyPart, body_parts> &body_parts_table();
// Number of real damage levels of a part (its top one is a break).
int level_count(std::size_t part);

// The 19 bones of the X-ray, each drawn for one body part.
struct Bone {
    const char *name;
    const char *region; // Skate 3's sound material: head, torso, arm, leg, foot
    int part;           // index in body_parts_table()
};
const std::vector<Bone> &bones();

struct BrokenBone {
    std::size_t bone{};
    float impact{}; // m/s of speed lost in the hit that broke it
    double time{};  // when it broke (Sample::time)
};
// A body part reaching a new damage level, for the sound of it.
struct DamageEvent {
    int part{};
    int level{}; // 1..level_count(part)
    bool top{};  // broken
    float impact{};
    double time{};
};

struct Result {
    std::uint32_t serial{};
    std::vector<BrokenBone> broken;
    // Each X-ray bone's damage level as a share of its top one (1 = broken; 0 = untouched).
    std::array<float, 19> damage{};
    std::array<int, body_parts> level{}; // damage level reached by each body part
    std::vector<DamageEvent> events;
    int impacts{};
    float distance{};    // metres from where the bail began
    float peak_speed{};  // m/s
    float peak_height{}; // metres above the start of the bail
    float air_time{};    // seconds off the ground during the bail
    float duration{};    // seconds
    float biggest_hit{}; // m/s
    float drop{};     // metres below where the bail began, at its lowest
    float rotation{}; // degrees turned about the vertical during the bail
    // Points per row of the original HUD: the bone chip on top, then the metrics.
    int bone_points{}, rotation_points{}, air_points{}, drop_points{}, duration_points{}, speed_points{};
    // Skate 3's car bonus (Sk8::Score::HoM VehicleData): each car hit, apart by its gap.
    int car_count{}, car_points{};
    int total{};
    std::string title;
    // When each HUD row first appeared (rotation, air, drop, bail time, speed; -1 = not yet). The
    // original stacks rows in that order, the first one nearest the score.
    std::array<double, 5> shown_at{-1, -1, -1, -1, -1};
};
// Whether a metric has a row on the HUD yet (0 rotation, 1 air, 2 drop, 3 bail time, 4 speed).
bool metric_visible(const Result &r, int metric);

struct Config {
    std::uint32_t wipeout_state = 300; // physics state that starts a bail
    float impact_threshold = 3.5f;     // m/s lost between two ticks to count as a hit
    float hit_torso = 3.5f;             // m/s the chest or hips lose in ~0.13 s: the body hit something
    float hit_any = 9.0f;               // or any bone does (a limb slammed hard)
    float down_time = 0.20f;            // seconds the body must be lying for an off-board bail
    float still_speed = 1.2f;           // bones' average speed below which the body has stopped (a
                                        // ragdoll at rest still twitches at up to ~1 m/s)
    float still_time = 0.4f;            // for this long: the bail's score is final
    float settle_speed = 0.6f;         // below this for settle_time ends the bail
    float settle_time = 1.0f;
    float min_duration = 0.4f; // shorter wipeouts are not worth scoring
    // Skate 3's VehicleData: 2,000 a car; its 1,500 beside it read as the gap (ms) before the next.
    int car_bonus = 2000;
    double car_gap = 1.5;
    float max_duration = 30.0f;
    // The metrics pay by Skate 3's own score graphs (metric_points in hom_core.cpp).
};
// Points for a metric by Skate 3's score graph (0 rotation degrees, 1 air seconds, 2 drop metres,
// 3 bail seconds, 4 speed m/s).
int metric_points(int metric, float value);

enum class Phase { idle, bailing, finished };

class Tracker {
public:
    explicit Tracker(Config config = {}) : config_(config) {}
    // Returns true on the tick a bail ends (result() is then valid).
    bool update(const Sample &sample);
    Phase phase() const { return phase_; }
    // A bail that is on screen: not one still waiting to see whether a glide's landing was a crash.
    bool showing() const { return phase_ == Phase::bailing && pending_until_ < 0; }
    // The skater is getting up: upright and settled, not falling feet first (the X-ray fades then).
    // The live bail is the previous one carrying on (a second drop straight after it).
    bool carried_on() const { return carried_on_; }
    bool getting_up() const { return phase_ == Phase::bailing && upright_since_ >= 0; }
    // Bails taken back because the skater was straight back on the board or their feet.
    int cancelled() const { return cancelled_; }
    // Why the last bail started, for the log.
    const std::string &trigger() const { return trigger_; }
    const Result &live() const { return live_; }   // the bail in progress, scored so far
    const Result &result() const { return last_; } // the last finished bail
    void reset();
    const Config &config() const { return config_; }
    // Off the board and not bailing: what the crash test sees, for the log (tuning glides and dives).
    std::string watch_line() const;

private:
    void score(Result &r) const;
    void register_impact(float drop);
    void hit_part(int part, float impact, double time);
    Config config_;
    Phase phase_{Phase::idle};
    Result live_{}, last_{};
    Sample previous_{}, start_{};
    bool have_previous_{};
    double slow_since_{-1}, air_start_{-1};
    std::uint32_t serial_{};
    std::uint64_t rng_{};
    std::vector<bool> is_broken_;
    // Each bone's recent speeds (newest last), to measure how much it lost over a short window.
    std::array<std::array<float, 8>, 19> bone_speeds_{};
    std::array<double, 8> bone_times_{};
    int bone_samples_{};
    bool went_down_{};          // the body has been off its feet during this bail
    double upright_since_{-1};
    void track_bones(const Sample &previous, const Sample &now, double dt);
    void apply_bone_hits(const Sample &now);
    std::array<float, 19> bone_change_{}; // speed each bone lost over the last ~0.13 s
    // The part of that loss that was the bone's own hit: in full for the bones touching the ground
    // (the lowest), else only what it lost beyond the body as a whole (it struck something).
    std::array<float, 19> bone_hit_{};
    double down_since_{-1}, hit_time_{-1}, upright_idle_since_{-1};
    Sample down_start_{};
    bool armed_{true}; // the skater has been on their feet since the last bail
    std::string trigger_;
    // Strongest hit each bone took in the last 3 s while not bailing, applied when a bail
    // starts after its own impact (an off-board crash is only recognised after the hit).
    std::array<float, 19> recent_hit_{};
    std::array<double, 19> recent_hit_time_{};
    float hips_vertical_{}; // smoothed up/down speed of the hips (m/s)
    float recent_peak_speed_{}; // the body's top speed lately, decaying (m/s)
    double fast_fall_time_{-1}; // when the hips last fell at 3 m/s or more
    double hard_fall_time_{-1}; // ... at 6 m/s or more
    // The hips' velocity and smoothed acceleration: in free fall the body drops at g and keeps its
    // sideways speed, which sliding down a ramp (also fast and downward) does not.
    std::array<float, 3> hips_velocity_{}, hips_accel_{};
    bool free_fall_{};
    double free_fall_time_{-1}; // when the body was last in free fall
    // Where the skater left the board (or their feet) before a crash, and the free fall since:
    // a slam recognised on impact still counts its fall, as Skate 3's bail starts at the bail.
    Sample offboard_start_{};
    bool have_offboard_start_{};
    float pre_air_{};
    bool slam_now_{};
    // Gliding: off the board, flat out and fast for a while (skate.'s spread eagle and dive). A crash
    // right after one waits pending_until_ to see the skater stay down: a glide lands with a jolt and
    // puts the skater straight back on the board.
    float glide_time_{};
    double last_glide_time_{-1}, pending_until_{-1};
    int cancelled_{};
    // When the physics state last switched in or out of the off-board states: skate. snaps the pose
    // over then, which flings the hands and feet, so no limb takes more than the torso's hit then.
    double switch_time_{-1};
    double pending_from_{-1}; // when the glide-landing wait began
    double riding_since_{-1};
    // The last bail as it ended, so a crash straight after it (still falling, a second drop) carries
    // on as the same bail instead of starting another.
    double ended_at_{-1};
    double vehicle_time_{-1}; // last tick the body touched a vehicle
    double car_awarded_{-1e9}; // when this bail last paid a car bonus
    bool carried_on_{};
    Sample ended_start_{};
    float ended_turn_{};
    std::vector<bool> ended_broken_;
    // The tick the skater came off the board (state 1xx or 2xx into 504) and from which, so the
    // crash that caused it can be recognised over the next few ticks.
    double left_board_time_{-1};
    std::uint32_t left_board_from_{};
    float turn_{}; // signed turn of the body during the bail (degrees); rotation is its size
    // Off the board since leaving it in the air (a dive, a spread eagle, a jump off): only a
    // falling impact makes that a crash, not the jolts of steering a glide.
    bool from_air_{};
    // Once the body has stopped moving the bail's numbers are final, like the original's
    // wipeout-over: the X-ray stays until the skater is up, the score no longer counts.
    bool frozen_{};
    double still_since_{-1};
    float body_speed_{};
};

const char *title_for(int total, std::size_t bones_broken);
} // namespace dingosdk::skate3_hom
