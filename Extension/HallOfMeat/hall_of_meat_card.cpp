#include "hall_of_meat_card.h"
#include <format>

namespace dingosdk::hall_of_meat {
namespace {
std::string hits(int value) { return grouped(value) + (value == 1 ? " hit" : " hits"); }
}

Card card(const View& view, const Standing& against) {
    if (view.phase == Phase::riding) return {};
    const auto& t = view.tally;
    Card result;
    result.opacity = view.alpha;
    const float road_rash = t.scraped * feet_per_metre, fallen = t.fallen * feet_per_metre,
        speed = t.top_speed * mph_per_metre_per_second;
    const auto row = [&](bool shown, Stat stat, std::string value, int points) {
        if (shown) result.rows.push_back({stat, std::move(value), points});
    };
    row(true, Stat::time, std::format("{:.1f} s", t.seconds), t.time_points);
    row(t.impacts >= hits_shown, Stat::hits, hits(t.impacts), t.hit_points + t.head_bonus + t.vehicle_bonus);
    row(t.broken >= broken_shown, Stat::broken, grouped(t.broken) + " broken", t.broken * points_per_break);
    row(road_rash >= road_rash_shown_feet, Stat::road_rash, std::format("{:.1f} ft", road_rash), t.scrape_points);
    row(t.airtime >= airtime_shown_seconds, Stat::airtime, std::format("{:.1f} s", t.airtime), t.airtime_points);
    row(fallen >= fall_shown_feet, Stat::fall, std::format("{:.1f} ft", fallen), t.fall_points);
    row(speed >= speed_shown_mph, Stat::speed, std::format("{:.1f} MPH", speed), t.speed_points);
    row(t.rotations >= rotations_shown, Stat::rotations, std::format("{:.1f} rotations", t.rotations), t.rotation_points);
    result.total = t.score;
    if (against.new_best) {
        result.badge = "NEW BEST";
        result.highlight = true;
    } else if (against.best > 0) {
        result.badge = "BEST " + grouped(against.best);
    }
    return result;
}

std::string grouped(long long value) {
    const bool negative = value < 0;
    const auto magnitude = negative ? 0ull - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
    auto digits = std::to_string(magnitude);
    for (auto at = static_cast<long long>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return negative ? "-" + digits : digits;
}
}
