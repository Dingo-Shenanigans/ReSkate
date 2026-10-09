#pragma once
#include "Engine/Game/Skater/skater_body.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

// Hall of Meat, the model: what a bail did to the skater, and what it scores. Fed one
// physics step at a time (hall_of_meat_skater.h) with what each body touched
// (Engine/Game/Skater/skater_body.h), how the skater moves and whether the game simulates the
// body as a ragdoll; read by the overlay. No game access, no locking: the caller owns both.
//
// A bail follows the skater, not a clock:
//   riding  → bailing     the game's wipeout, and nothing else: a fall from height ragdolls in the
//                         air before the wipeout of its impact (its flight and its hits of just
//                         before carry into the bail), and skate.'s flumps (a dive, a roll along
//                         the ground, from any height) are ragdolls the game never wipes out
//   bailing → down        the body has come to rest: the bail's time and points are final
//   bailing, down → getting up   the ragdoll ends (the skater stands up), or the skater is gone
//                         (a respawn, a teleport); the bail is over, stays and fades out
// Measured in play: the ragdoll begins with the wipeout or in the flight before it, and
// ends exactly once, when the skater stands up on foot; skate. may put them back on the board
// a moment later. The bail shows only once it has hurt a bone: a fall that leaves the skater
// unbruised shows nothing.
//
// Two clocks. The bail runs on the game's: its steps' own lengths added up, so in slow motion
// (a break: hall_of_meat.h) the moment the ragdoll has to begin, the rest, the impacts and every
// stat still measure what the body did. What the player sees runs on the real one: a hit's
// flash, a break's effect, the linger and the fade after the bail.
namespace dingosdk::hall_of_meat {
using skater_body::Bone;

// One physics step of the local skater.
struct Step {
    float seconds{}; // how long the step simulated, in the game's time
    bool wipeout{};  // the step asks for a wipeout
    bool airborne{}; // the skater is in the air, on the board or off it
    // The body is a ragdoll; empty when the skater state could not be read: such a step neither
    // starts nor ends a bail by it.
    std::optional<bool> ragdoll;
    // How the skater moves (with the board while on it, else as its pelvis), metres per second;
    // empty when unknown: such a step neither rests nor stirs the body, and adds nothing to its
    // fall or its speed.
    std::optional<game::Vec3> velocity;
    // How fast the pelvis turns, any way round, radians per second; empty when unknown: such a
    // step adds nothing to its rotations.
    std::optional<float> spin;
    skater_body::Contacts body; // what each body touched in the step
};

enum class Injury : std::uint8_t { none, hit, broken };

// Impact speeds along the contact normal, as measured in play: a fall on flat ground hits the
// bones it lands on at 3 to 9 m/s and grazes the rest at 2 to 3, a hard landing hits the legs at
// 8 to 10, a drop from height everything at 13 to 35. So a hard fall on flat ground bruises, and
// only a drop from height breaks bones.
inline constexpr float hit_speed = 7.0f;
inline constexpr float broken_speed = 12.0f;
// A wipeout whose ragdoll does not begin within this long was a stumble: its bail ends.
inline constexpr std::uint64_t ragdoll_wait_ms = 500;
// The body has come to rest once it moves slower than still_speed for rest_ms: from then on
// nothing counts. Measured in play: a body lying moves at 0.0 to 0.3 m/s (a skater standing
// 0.0 to 0.1), a tumbling or sliding one at 2 and more. A rest is final: getting up starts inside
// the ragdoll (its animated part), moving the body fast again before the ragdoll ends.
inline constexpr float still_speed = 0.5f;
inline constexpr std::uint64_t rest_ms = 750;
// Once the skater gets up, the skeleton and the card stay linger_ms, then fade out over fade_ms.
inline constexpr std::uint64_t linger_ms = 1000;
inline constexpr std::uint64_t fade_ms = 800;
inline constexpr std::uint64_t flash_ms = 350; // a fresh hit flashes this long
// A bone breaking hits the whole screen, as in skate. 3's Hall of Meat: its edges pulse red and the
// game slows down to break_game_speed (single player only: hall_of_meat.h), together, by one
// effect: easing in over break_rise_ms, held break_hold_ms, easing out over break_fall_ms; in real
// time, from the step that saw the break (a bail takes over the breaks of just before it, which
// happened earlier). Each break has its own and the strongest counts, so a further break hits
// again without the screen or the game's speed jumping.
inline constexpr std::uint64_t break_rise_ms = 150;
inline constexpr std::uint64_t break_hold_ms = 1000;
inline constexpr std::uint64_t break_fall_ms = 1000;
inline constexpr std::uint64_t break_effect_ms = break_rise_ms + break_hold_ms + break_fall_ms;
inline constexpr float break_game_speed = 0.3f;
// A bone's contact lasts several physics steps: hits of one bone closer together than
// this are one impact, as hard as its hardest step.
inline constexpr std::uint64_t impact_gap_ms = 200;
inline constexpr std::size_t max_impacts = 64;
// The game reports the wipeout of an impact a few physics steps after it (0 to 2 steps in
// play; a drop from height hit the right thigh at 35 m/s a step before its wipeout): a bail takes the flight and the hits of this long before it starts.
inline constexpr std::uint64_t wipeout_after_impact_ms = 250;

// The Meat a bail scores: every hit by how hard it was, and each bone broken a bonus once.
// A hit's points grow faster than its speed but slower than its energy: the lightest hit
// (hit_speed) scores 100, a 30 m/s slam about 900 (not the 18 bruises its energy would make it).
// A break scores what ten of the lightest hits do, so a heavy bail's hits and breaks do not
// outweigh the rest of the card.
inline constexpr float hit_reference_speed = hit_speed;
inline constexpr float points_per_reference_hit = 100.0f;
inline constexpr float hit_points_exponent = 1.5f;
inline constexpr int points_per_break = 1000;
// A hit to the head (the upper neck it rides on, Bone::neck1) counts double; one from a
// vehicle half again. Both together count 2.5 times.
inline constexpr float head_multiplier = 2.0f;
inline constexpr float vehicle_multiplier = 1.5f;
// Road rash: a body sliding along the ground or a wall, not the board, at least this fast
// (metres per second along the surface) scrapes. A long slide moves the body at 3 to 10
// m/s for seconds without a single hit. The skater's road rash is how far it slid: in
// each step, how fast its scraping bodies slid on average, not their sum (a dozen bodies on the
// ground at once would make one metre a dozen). A metre scores what four of the lightest hits do.
inline constexpr float scrape_speed = 1.5f;
inline constexpr float points_per_scraped_metre = 400.0f;
inline constexpr float bruising_scrape = 1.0f; // metres one bone slid: it shows as bruised
// The rest of the bail scores as skate. 3's Hall of Meat did, each stat its own points: its time
// (falling and sliding, not resting), its airtime, how far the body fell (every metre it went down,
// from the flight the bail came from on), its top speed and how often it turned over. Its example card
// scored a 6.2 s bail 5,416, 3.4 s of air 10,816, a 50 m fall 13,251 and 25 MPH 1,590. A fall
// scores half of that here: at the card's rate it outweighed everything else in play.
inline constexpr float points_per_second = 600.0f;
inline constexpr float points_per_air_second = 2000.0f;
inline constexpr float points_per_metre_fallen = 125.0f;
inline constexpr float points_per_speed = 130.0f; // per metre per second
// Rotations: how far the body turned, any way round, while it fell and slid. Measured in
// play: a skater turning on the spot turns the pelvis at 30 to 300 degrees a second; a body
// tumbling down stairs turns over several times a second. A rotation scores what a second's bail does.
inline constexpr float points_per_rotation = 600.0f;
inline constexpr float radians_per_rotation = 6.2831853f;

constexpr Injury injury(float peak) noexcept {
    return peak >= broken_speed ? Injury::broken : peak >= hit_speed ? Injury::hit : Injury::none;
}
// A hit's points by its speed alone.
inline int hit_points(float speed) noexcept {
    if (!(speed >= hit_speed)) return 0;
    return static_cast<int>(
        points_per_reference_hit * std::pow(speed / hit_reference_speed, hit_points_exponent) + 0.5f);
}

struct Impact {
    std::size_t bone{};
    float speed{};    // the hardest step of the contact
    bool vehicle{};   // a vehicle took part in it
};
constexpr bool head(std::size_t bone) noexcept { return bone == skater_body::index(Bone::neck1); }
// What an impact adds on top of its hit points, for the head and for a vehicle.
inline int head_bonus(const Impact& impact) noexcept {
    return head(impact.bone) ? static_cast<int>(hit_points(impact.speed) * (head_multiplier - 1.0f) + 0.5f) : 0;
}
inline int vehicle_bonus(const Impact& impact) noexcept {
    return impact.vehicle ? static_cast<int>(hit_points(impact.speed) * (vehicle_multiplier - 1.0f) + 0.5f) : 0;
}
inline int impact_points(const Impact& impact) noexcept {
    return hit_points(impact.speed) + head_bonus(impact) + vehicle_bonus(impact);
}

// A bail's numbers, so far or in the end.
struct Tally {
    int impacts{};       // hits of at least hit_speed
    int broken{};        // bones
    int hit_points{};    // the hits by their speed
    int head_bonus{};    // what hits to the head added
    int vehicle_bonus{}; // what hits from vehicles added
    float scraped{};     // metres the skater slid along surfaces: the road rash
    int scrape_points{};
    int damage{};        // the hits, their bonuses and the road rash
    float seconds{};     // falling and sliding, not resting
    int time_points{};
    float airtime{};     // seconds in the air
    int airtime_points{};
    float fallen{};      // metres the body went down
    int fall_points{};
    float top_speed{};   // metres per second
    int speed_points{};
    float rotations{};   // turns of the body, any way round
    int rotation_points{};
    int score{};         // the damage, the breaks, the time, the airtime, the fall, the speed and the rotations: the Meat
};
// A bail's score against the best on the same map before it.
struct Standing {
    int best{};      // the best, this bail included
    bool new_best{}; // this bail set it
};
constexpr Standing standing(int best, int score) noexcept {
    return score > best ? Standing{score, true} : Standing{best, false};
}

enum class Phase : std::uint8_t { riding, bailing, down, getting_up };
// What the overlay draws at one moment: riding (nothing) until the bail hurts a bone.
struct View {
    Phase phase{};
    float alpha{}; // the skeleton's and the card's: 1 through the bail, fading to 0 as the skater gets up
    std::array<Injury, skater_body::count> injuries{};
    std::array<float, skater_body::count> flashes{}; // 1 at a fresh hit, falling to 0
    float break_pulse{}; // the red edge after a bone breaks: rising to 1, falling to 0
    Tally tally;
};
// A finished bail, for the log and the map's best.
struct Summary {
    bool shown{}; // it hurt a bone, so it showed (and counts for a best)
    std::array<float, skater_body::count> peaks{};   // each body's hardest hit
    std::array<float, skater_body::count> scraped{}; // and how far it slid, each on its own
    Tally tally;
};

class Tracker {
public:
    // One physics step, at `now` in real time (milliseconds). True when this step ended a bail;
    // `ended` then receives it.
    bool step(std::uint64_t now, const Step& step, Summary* ended = nullptr) noexcept;
    // The skater is gone (a respawn, a teleport, a map change): a bail ends now. True when one did.
    bool lose(std::uint64_t now, Summary* ended = nullptr) noexcept;
    View view(std::uint64_t now) const noexcept;
    // Where the bail is at `now`, whether it hurt a bone (and so shows) or not.
    Phase phase(std::uint64_t now) const noexcept;
    // How fast the game is to run now: down to break_game_speed as a break hits, else 1.
    float game_speed(std::uint64_t now) const noexcept;
    void reset() noexcept { *this = {}; }

private:
    // Times on the game's clock (played_) are milliseconds of its time, as doubles; real times are
    // GetTickCount64()'s milliseconds, never 0.
    //
    // A bone's hardest hit of the last wipeout_after_impact_ms while riding, which a bail takes.
    struct Lead {
        float speed{};
        bool vehicle{};
        std::optional<double> at; // the game's clock
    };
    struct BoneState {
        float peak{};                 // the hardest hit this bail
        std::optional<double> hit_at; // its last step with a hit, on the game's clock
        std::uint64_t flashed_at{};   // and when that hit showed, in real time; 0 never
        std::size_t impact{};         // which impact that step belonged to
        float scraped{};              // metres it slid
        Lead lead;
    };
    void begin(std::uint64_t now) noexcept;
    void rest(std::optional<float> speed) noexcept;
    void count(std::uint64_t now, const Step& step, double step_ms) noexcept;
    void hit(std::size_t bone, float speed, bool vehicle, double at, std::uint64_t now) noexcept;
    bool end(std::uint64_t now, Summary* ended) noexcept;
    double bail_ms() const noexcept;
    Tally tally() const noexcept;
    Injury injury_of(const BoneState& bone) const noexcept;

    Phase phase_{};    // riding, bailing or down; getting_up is the linger and the fade after a bail's end
    bool ragdolled_{}; // the bail's ragdoll has begun
    bool hurt_{};      // a bone was hit at least at hit_speed, or scraped bruised, this bail
    unsigned breaks_{}, seen_breaks_{}; // bones broken this bail, and how many of them the steps saw
    std::array<std::uint64_t, skater_body::count> breaks_seen_at_{}; // the steps that saw breaks, oldest first
    std::size_t breaks_seen_{};                                       // how many of them
    float break_effect(std::uint64_t now) const noexcept; // 0 to 1
    double played_{};     // the game's clock: every step's length added up
    double started_{};    // the game's clock at the bail's wipeout
    std::uint64_t ended_{}; // real time: when the bail ended, for its linger and fade; 0 none yet
    std::optional<double> still_since_; // bailing: when the body last went slower than still_speed
    std::optional<double> stopped_at_;  // when the bail's time stopped: the body came to rest, or the bail ended
    double flight_ms_{};             // while riding: the last flight, which a bail takes over
    float flight_fallen_{};          // and how far it went down
    std::optional<double> landed_;   // when it touched down; empty while in the air
    double airtime_ms_{};
    float fallen_{};    // metres the body went down
    float top_speed_{}; // metres per second
    float turned_{};    // radians the body turned, any way round
    float scraped_{};   // metres the skater slid: the road rash
    std::array<BoneState, skater_body::count> bones_{};
    std::array<Impact, max_impacts> impacts_{};
    std::size_t impact_count_{};
};
}
