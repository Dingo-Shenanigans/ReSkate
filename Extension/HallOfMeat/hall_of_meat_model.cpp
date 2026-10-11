#include "hall_of_meat_model.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::hall_of_meat {
namespace {
// The physics thread steps and the render thread draws, each reading the real clock itself:
// a moment ago may be a moment ahead.
std::uint64_t elapsed(std::uint64_t now, std::uint64_t since) noexcept { return now > since ? now - since : 0; }
// A step's length on the game's clock; nothing for one the game did not report sensibly.
double length_ms(float seconds) noexcept { return std::isfinite(seconds) && seconds > 0 ? seconds * 1000.0 : 0.0; }
float speed_of(float value) noexcept { return std::isfinite(value) && value > 0 ? value : 0.0f; }
bool is_foot(std::size_t bone) noexcept { return skater_body::foot(static_cast<Bone>(bone)); }
// Sliding along anything but the board scrapes: the feet ride on it all the time.
bool scrapes(const skater_body::HitKinds& hit) noexcept {
    return hit.world || hit.vehicle || hit.kind_5 || hit.kind_11;
}
int points(float amount, float per_unit) noexcept { return static_cast<int>(amount * per_unit + 0.5f); }
}

bool Tracker::step(std::uint64_t now, const Step& step, Summary* ended) noexcept {
    const double step_ms = length_ms(step.seconds);
    played_ += step_ms;
    const double flown = step.airborne ? step_ms : 0.0;
    if (step.airborne) landed_.reset();
    else if (!landed_) landed_ = played_;
    const bool ragdoll = step.ragdoll.value_or(false);
    // How far the skater went down in the step.
    const float descent = step.velocity ? std::max(0.0f, -(*step.velocity)[1]) * static_cast<float>(step_ms / 1000.0) : 0.0f;
    bool began{};
    if (phase_ == Phase::riding) {
        if (step.airborne) {
            flight_ms_ += flown;
            flight_fallen_ += descent;
        } else if (played_ - *landed_ > wipeout_after_impact_ms) { // landed, and stayed up
            flight_ms_ = 0;
            flight_fallen_ = 0;
        }
        // Each body's hardest recent hit, for a bail that starts a few steps after it.
        for (std::size_t index = 1; index < skater_body::count; ++index) { // 0 is the board
            const auto& contact = step.body.bodies[index];
            const float speed = speed_of(contact.impact);
            auto& lead = bones_[index].lead;
            if (speed <= 0 || (lead.at && played_ - *lead.at <= wipeout_after_impact_ms && speed <= lead.speed)) continue;
            lead = {speed, contact.hit.vehicle, played_};
        }
        if (!step.wipeout) return false;
        begin(now); // the flight it came from holds this step's airtime and fall
        began = true;
    }
    ragdolled_ = ragdolled_ || ragdoll;
    const auto speed = step.velocity ? std::optional<float>(game::length(*step.velocity)) : std::nullopt;
    rest(speed);
    if (phase_ == Phase::bailing) {
        if (!began) {
            airtime_ms_ += flown;
            fallen_ += descent;
        }
        if (speed) top_speed_ = std::max(top_speed_, *speed);
        if (step.spin) turned_ += speed_of(*step.spin) * static_cast<float>(step_ms / 1000.0);
        count(now, step, step_ms);
    }
    // The skater stood up, or never went down.
    const bool stood_up = ragdolled_ ? step.ragdoll.has_value() && !*step.ragdoll : played_ - started_ >= ragdoll_wait_ms;
    if (breaks_ != seen_breaks_) { // a break this step: its effect runs from now
        seen_breaks_ = breaks_;
        if (breaks_seen_ < breaks_seen_at_.size()) breaks_seen_at_[breaks_seen_++] = now;
    }
    return stood_up && end(now, ended);
}

// Whether the body has come to rest, by how fast it moves: then the bail's time and points are final.
void Tracker::rest(std::optional<float> speed) noexcept {
    if (phase_ != Phase::bailing || !speed) return;
    if (*speed >= still_speed) {
        still_since_.reset();
    } else if (!still_since_) {
        still_since_ = played_;
    } else if (played_ - *still_since_ >= rest_ms) {
        phase_ = Phase::down;
        stopped_at_ = still_since_; // it has rested since it went still
    }
}

// What each body hit and how far it slid in one step of the bail.
void Tracker::count(std::uint64_t now, const Step& step, double step_ms) noexcept {
    const float step_seconds = static_cast<float>(step_ms / 1000.0);
    float sliding{}; // the scraping bodies' slides, summed
    int scraping{};
    for (std::size_t index = 1; index < skater_body::count; ++index) {
        const auto& contact = step.body.bodies[index];
        const float speed = speed_of(contact.impact);
        if (speed > 0) hit(index, speed, contact.hit.vehicle, played_, now);
        const float slide = game::length(contact.slide);
        if (!contact.touching || is_foot(index) || !scrapes(contact.hit) || !std::isfinite(slide) || slide < scrape_speed)
            continue;
        auto& bone = bones_[index];
        bone.scraped += slide * step_seconds;
        if (bone.scraped >= bruising_scrape) hurt_ = true;
        sliding += slide;
        ++scraping;
    }
    if (scraping) scraped_ += sliding / static_cast<float>(scraping) * step_seconds;
}

bool Tracker::lose(std::uint64_t now, Summary* ended) noexcept {
    // Nothing from before the skater went carries over to the one that comes back.
    flight_ms_ = 0;
    landed_.reset();
    breaks_ = seen_breaks_ = 0;
    breaks_seen_ = 0;
    flight_fallen_ = 0;
    for (auto& bone : bones_) bone.lead = {};
    return phase_ != Phase::riding && end(now, ended);
}

// A bail from this step (`now` in real time), with the flight it came from and the hits just before it.
void Tracker::begin(std::uint64_t now) noexcept {
    const auto played = played_;
    const auto flight = flight_ms_;
    const auto flight_fallen = flight_fallen_;
    const auto before = bones_;
    *this = {};
    played_ = played;
    phase_ = Phase::bailing;
    started_ = played_;
    airtime_ms_ = flight; // this step's share is in it
    fallen_ = flight_fallen;
    for (std::size_t index = 1; index < skater_body::count; ++index) {
        const auto& lead = before[index].lead;
        if (lead.at && played_ - *lead.at <= wipeout_after_impact_ms) hit(index, lead.speed, lead.vehicle, *lead.at, now);
    }
}

// A hit of one bone at `at` on the game's clock, shown from `now`: a new impact, or the harder step
// of the contact it belongs to.
void Tracker::hit(std::size_t index, float speed, bool vehicle, double at, std::uint64_t now) noexcept {
    auto& bone = bones_[index];
    if (bone.peak < broken_speed && speed >= broken_speed) ++breaks_;
    bone.peak = std::max(bone.peak, speed);
    if (speed < hit_speed) return;
    hurt_ = true;
    if (bone.hit_at && at - *bone.hit_at <= impact_gap_ms) {
        auto& impact = impacts_[bone.impact];
        impact.speed = std::max(impact.speed, speed);
        impact.vehicle = impact.vehicle || vehicle;
    } else if (impact_count_ < max_impacts) {
        bone.impact = impact_count_;
        impacts_[impact_count_++] = {index, speed, vehicle};
    }
    bone.hit_at = at;
    bone.flashed_at = now;
}

bool Tracker::end(std::uint64_t now, Summary* ended) noexcept {
    phase_ = Phase::riding;
    ended_ = now;
    if (!stopped_at_) stopped_at_ = played_;
    if (ended) {
        ended->shown = hurt_;
        for (std::size_t index = 0; index < skater_body::count; ++index) {
            ended->peaks[index] = bones_[index].peak;
            ended->scraped[index] = bones_[index].scraped;
        }
        ended->tally = tally();
    }
    return true;
}

// The bail's time, on the game's clock: until the body came to rest, or the bail ended, or the latest step.
double Tracker::bail_ms() const noexcept {
    return std::max(0.0, (stopped_at_ ? *stopped_at_ : played_) - started_);
}

Phase Tracker::phase(std::uint64_t now) const noexcept {
    if (phase_ != Phase::riding) return phase_;
    return ended_ && elapsed(now, ended_) < linger_ms + fade_ms ? Phase::getting_up : Phase::riding;
}

Injury Tracker::injury_of(const BoneState& bone) const noexcept {
    const auto by_hits = injury(bone.peak);
    return by_hits == Injury::none && bone.scraped >= bruising_scrape ? Injury::hit : by_hits;
}

Tally Tracker::tally() const noexcept {
    Tally result;
    for (std::size_t index = 0; index < impact_count_; ++index) {
        const auto& impact = impacts_[index];
        result.hit_points += hit_points(impact.speed);
        result.head_bonus += head_bonus(impact);
        result.vehicle_bonus += vehicle_bonus(impact);
    }
    result.impacts = static_cast<int>(impact_count_);
    for (std::size_t index = 1; index < skater_body::count; ++index) // 0 is the board
        if (injury(bones_[index].peak) == Injury::broken) ++result.broken;
    result.scraped = scraped_;
    result.scrape_points = points(result.scraped, points_per_scraped_metre);
    result.damage = result.hit_points + result.head_bonus + result.vehicle_bonus + result.scrape_points;
    result.seconds = static_cast<float>(bail_ms() / 1000.0);
    result.time_points = points(result.seconds, points_per_second);
    result.airtime = static_cast<float>(airtime_ms_ / 1000.0);
    result.airtime_points = points(result.airtime, points_per_air_second);
    result.fallen = fallen_;
    result.fall_points = points(result.fallen, points_per_metre_fallen);
    result.top_speed = top_speed_;
    result.speed_points = points(result.top_speed, points_per_speed);
    result.rotations = turned_ / radians_per_rotation;
    result.rotation_points = points(result.rotations, points_per_rotation);
    result.score = result.damage + result.broken * points_per_break + result.time_points + result.airtime_points +
        result.fall_points + result.speed_points + result.rotation_points;
    return result;
}

View Tracker::view(std::uint64_t now) const noexcept {
    View view;
    const auto phase = this->phase(now);
    if (phase == Phase::riding || !hurt_) return view;
    view.phase = phase;
    const auto since_end = phase == Phase::getting_up ? elapsed(now, ended_) : 0;
    view.alpha = since_end > linger_ms ? 1.0f - static_cast<float>(since_end - linger_ms) / static_cast<float>(fade_ms) : 1.0f;
    view.tally = tally();
    for (std::size_t index = 0; index < skater_body::count; ++index) {
        const auto& bone = bones_[index];
        view.injuries[index] = injury_of(bone);
        const auto since_hit = elapsed(now, bone.flashed_at);
        if (bone.flashed_at && since_hit < flash_ms)
            view.flashes[index] = 1.0f - static_cast<float>(since_hit) / static_cast<float>(flash_ms);
    }
    view.break_pulse = break_effect(now);
    return view;
}

float Tracker::break_effect(std::uint64_t now) const noexcept {
    const auto smooth = [](std::uint64_t part, std::uint64_t whole) {
        const float t = static_cast<float>(part) / static_cast<float>(whole);
        return t * t * (3.0f - 2.0f * t);
    };
    float strongest = 0.0f;
    for (std::size_t index = 0; index < breaks_seen_; ++index) {
        const auto since = elapsed(now, breaks_seen_at_[index]);
        if (since >= break_effect_ms) continue;
        const float effect = since < break_rise_ms ? smooth(since, break_rise_ms)
            : since < break_rise_ms + break_hold_ms ? 1.0f
            : 1.0f - smooth(since - break_rise_ms - break_hold_ms, break_fall_ms);
        strongest = std::max(strongest, effect);
    }
    return strongest;
}

float Tracker::game_speed(std::uint64_t now) const noexcept {
    return 1.0f - (1.0f - break_game_speed) * break_effect(now);
}
}
