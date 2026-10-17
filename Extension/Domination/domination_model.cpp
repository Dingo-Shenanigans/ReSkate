#include "domination_model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <stdexcept>

namespace dingosdk::domination {
namespace {
[[noreturn]] void fail(const std::string &text) { throw std::runtime_error(text); }

float number(const Json &value, const std::string &what) {
    if (!value.is_number()) fail(what + " is not a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result) || std::abs(result) > 1e6) fail(what + " is out of range");
    return static_cast<float>(result);
}
Vec2 pair(const Json &value, const std::string &what) {
    if (!value.is_array() || value.size() != 2) fail(what + " is not [x, z]");
    return {number(value.at(std::size_t{0}), what), number(value.at(std::size_t{1}), what)};
}
Vec3 triple(const Json &value, const std::string &what) {
    if (!value.is_array() || value.size() != 3) fail(what + " is not [x, y, z]");
    return {number(value.at(std::size_t{0}), what), number(value.at(std::size_t{1}), what), number(value.at(std::size_t{2}), what)};
}
std::string text(const Json &object, std::string_view key, const std::string &what) {
    if (!object.contains(key) || !object.at(key).is_string() || object.at(key).string().empty()) fail(what + " has no " + std::string(key));
    return object.at(key).string();
}
std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool in_polygon(const std::vector<Vec2> &points, float x, float z) {
    bool inside = false;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto &p = points[i], &q = points[j];
        if ((p[1] > z) != (q[1] > z) && x < (q[0] - p[0]) * (z - p[1]) / (q[1] - p[1]) + p[0]) inside = !inside;
    }
    return inside;
}
bool in_capsule(const Spot &spot, float x, float z) {
    const float abx = spot.b[0] - spot.a[0], abz = spot.b[1] - spot.a[1];
    const float length = abx * abx + abz * abz;
    float t = length > 0 ? ((x - spot.a[0]) * abx + (z - spot.a[1]) * abz) / length : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = x - (spot.a[0] + t * abx), dz = z - (spot.a[1] + t * abz);
    return dx * dx + dz * dz <= spot.radius * spot.radius;
}
bool covers(const Spot &spot, float x, float z) { return spot.capsule ? in_capsule(spot, x, z) : in_polygon(spot.points, x, z); }
std::array<float, 4> bounds(const Spot &spot) { // x0, z0, x1, z1
    if (spot.capsule)
        return {std::min(spot.a[0], spot.b[0]) - spot.radius, std::min(spot.a[1], spot.b[1]) - spot.radius,
                std::max(spot.a[0], spot.b[0]) + spot.radius, std::max(spot.a[1], spot.b[1]) + spot.radius};
    std::array<float, 4> r{spot.points[0][0], spot.points[0][1], spot.points[0][0], spot.points[0][1]};
    for (const auto &p : spot.points) r = {std::min(r[0], p[0]), std::min(r[1], p[1]), std::max(r[2], p[0]), std::max(r[3], p[1])};
    return r;
}
} // namespace

Park parse_park(const Json &json) {
    if (!json.is_object()) fail("the park file is not an object");
    if (json.value("schema", 0) != 1) fail("the park file's schema is not 1");
    Park park;
    park.level = lower(text(json, "level", "the park"));
    park.name = json.value("park", std::string("park"));
    park.map = json.value("map", std::string());
    park.package = json.value("package", std::string());
    if (park.name.size() > 80 || park.map.size() > 80 || park.package.size() > 128) fail("the park's name, map or package is too long");
    if (!json.contains("spots") || !json.at("spots").is_array() || json.at("spots").empty()) fail("the park has no spots");
    if (json.at("spots").size() > 64) fail("the park has more than 64 spots");
    for (const auto &row : json.at("spots")) {
        if (!row.is_object()) fail("a spot is not an object");
        Spot spot;
        spot.id = text(row, "id", "a spot");
        const auto what = "spot " + spot.id;
        spot.name = row.value("name", spot.id);
        spot.kind = row.value("kind", std::string("spot"));
        const auto shape = text(row, "shape", what);
        if (shape == "polygon") {
            if (!row.contains("points") || !row.at("points").is_array() || row.at("points").size() < 3) fail(what + " needs 3 or more points");
            for (const auto &p : row.at("points")) spot.points.push_back(pair(p, what + " point"));
        } else if (shape == "capsule") {
            spot.capsule = true;
            spot.a = pair(row.contains("a") ? row.at("a") : Json(), what + " a");
            spot.b = pair(row.contains("b") ? row.at("b") : Json(), what + " b");
            spot.radius = number(row.contains("radius") ? row.at("radius") : Json(), what + " radius");
            if (!(spot.radius > 0.05f && spot.radius < 50)) fail(what + " radius is out of range");
        } else fail(what + " has an unknown shape \"" + shape + "\"");
        if (!row.contains("y")) fail(what + " has no y range");
        const auto y = pair(row.at("y"), what + " y");
        spot.y_min = y[0];
        spot.y_max = y[1];
        if (!(spot.y_max > spot.y_min)) fail(what + " y range is empty");
        spot.marker = triple(row.contains("marker") ? row.at("marker") : Json(), what + " marker");
        // Within a map's reach, and the size of a spot (which also bounds the overlap check below).
        const auto box = bounds(spot);
        const auto near = [](float v) { return std::abs(v) <= 100000; };
        if (!near(box[0]) || !near(box[1]) || !near(box[2]) || !near(box[3]) || !near(spot.y_min) || !near(spot.y_max) || !near(spot.marker[0]) ||
            !near(spot.marker[1]) || !near(spot.marker[2]))
            fail(what + " is farther than 100 km from the map's origin");
        if (box[2] - box[0] > 500 || box[3] - box[1] > 500) fail(what + " is more than 500 m across");
        for (const auto &other : park.spots)
            if (other.id == spot.id) fail("two spots are called " + spot.id);
        park.spots.push_back(std::move(spot));
    }
    if (json.contains("starts")) {
        if (!json.at("starts").is_array() || json.at("starts").size() > 64) fail("the park's starts are not a list of up to 64");
        for (const auto &row : json.at("starts")) {
            if (!row.is_object() || !row.contains("position")) fail("a start has no position");
            Start start;
            start.position = triple(row.at("position"), "a start's position");
            start.heading = row.contains("heading") ? number(row.at("heading"), "a start's heading") : 0.0f;
            park.starts.push_back(start);
        }
    }
    // No two spots may share ground: sampled on a 0.25 m grid, off its lines (shared edges are fine).
    for (std::size_t i = 0; i < park.spots.size(); ++i)
        for (std::size_t j = i + 1; j < park.spots.size(); ++j) {
            const auto &s = park.spots[i], &t = park.spots[j];
            if (s.y_max <= t.y_min || t.y_max <= s.y_min) continue;
            const auto a = bounds(s), b = bounds(t);
            const float x0 = std::max(a[0], b[0]), z0 = std::max(a[1], b[1]), x1 = std::min(a[2], b[2]), z1 = std::min(a[3], b[3]);
            if (x0 > x1 || z0 > z1) continue;
            for (float x = std::floor(x0 * 4) / 4 + 0.1237f; x <= x1; x += 0.25f)
                for (float z = std::floor(z0 * 4) / 4 + 0.1291f; z <= z1; z += 0.25f)
                    if (covers(s, x, z) && covers(t, x, z))
                        fail(std::format("spots {} and {} overlap at ({:.2f}, {:.2f})", s.id, t.id, x, z));
        }
    return park;
}

int start_for(const Park &park, int index) {
    if (park.starts.empty()) return -1;
    const int n = static_cast<int>(park.starts.size());
    return ((index % n) + n) % n;
}

bool contains(const Spot &spot, const Vec3 &p) {
    return p[1] >= spot.y_min && p[1] <= spot.y_max && covers(spot, p[0], p[2]);
}
int spot_at(const Park &park, const Vec3 &position) {
    for (std::size_t i = 0; i < park.spots.size(); ++i)
        if (contains(park.spots[i], position)) return static_cast<int>(i);
    return -1;
}

void even_out(std::vector<float> &heights, float jump) {
    const std::size_t n = heights.size();
    constexpr std::size_t widest = 2;
    if (n < widest + 2) return;
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (std::size_t w = 1; w <= widest; ++w)
            for (std::size_t i = 0; i < n; ++i) {
                const float left = heights[(i + n - 1) % n], right = heights[(i + w) % n];
                bool above = true, below = true;
                for (std::size_t k = 0; k < w; ++k) {
                    const float h = heights[(i + k) % n];
                    above = above && h > std::max(left, right) + jump;
                    below = below && h < std::min(left, right) - jump;
                }
                if (!above && !below) continue;
                for (std::size_t k = 0; k < w; ++k) heights[(i + k) % n] = above ? std::max(left, right) : std::min(left, right);
                changed = true;
            }
        if (!changed) return;
    }
}

void fill_in(std::vector<float> &heights, float fallback) {
    const std::size_t n = heights.size();
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isnan(heights[i])) found.push_back(i);
    if (found.empty()) {
        std::ranges::fill(heights, fallback);
        return;
    }
    for (std::size_t f = 0; f < found.size(); ++f) {
        const std::size_t a = found[f], b = found[(f + 1) % found.size()];
        const std::size_t gap = (b + n - a) % n == 0 ? n : (b + n - a) % n; // steps from a on to b
        for (std::size_t k = 1; k < gap; ++k)
            heights[(a + k) % n] = heights[a] + (heights[b] - heights[a]) * static_cast<float>(k) / static_cast<float>(gap);
    }
}

// ---- line spots -------------------------------------------------------------------------
void LineSpots::reset(std::size_t spots) {
    time_.assign(spots, 0.0);
    total_ = 0;
}
void LineSpots::add(int spot, double seconds) {
    if (!(seconds > 0) || seconds > 1) return; // a hitch is not time on a spot
    total_ += seconds;
    if (spot >= 0 && static_cast<std::size_t>(spot) < time_.size()) time_[spot] += seconds;
}
int LineSpots::winner(double minimum) const {
    int best = -1;
    for (std::size_t i = 0; i < time_.size(); ++i)
        if (time_[i] > 0 && time_[i] >= minimum && (best < 0 || time_[i] > time_[best])) best = static_cast<int>(i);
    return best;
}
double LineSpots::seconds(int spot) const { return spot >= 0 && static_cast<std::size_t>(spot) < time_.size() ? time_[spot] : 0; }

// ---- tricks -----------------------------------------------------------------------------
namespace {
// The line's points before the multiplier.
float unscaled(float score, float multiplier) { return multiplier > 0.01f ? score / multiplier : score; }
} // namespace

int TrickWatch::trick_spot() const { return action_ ? trick_.winner(0) : -1; }

TrickEvent TrickWatch::update(const TrickSignals &in) {
    const double dt = in.dt > 0 ? in.dt : 1.0 / 60;
    if (in.spot >= 0) {
        recent_spot_ = in.spot;
        since_spot_ = 0;
    } else since_spot_ += dt;
    if (!std::exchange(seen_, true)) {
        score_ = in.line_score;
        multiplier_ = in.multiplier;
    }
    // A slam or a wipeout: skate. drops the line, and the trick going on scores nothing.
    if (in.slam || in.wipeout) {
        const bool had = action_ || waiting_;
        action_ = false;
        waiting_ = 0;
        score_ = in.line_score;
        multiplier_ = in.multiplier;
        return had ? TrickEvent{TrickEvent::Kind::broken} : TrickEvent{};
    }
    if (in.action) {
        if (!action_) { // a trick starts (a previous one whose points never came scored nothing)
            action_ = true;
            waiting_ = 0;
            trick_.reset(spots_);
            takeoff_spot_ = since_spot_ <= takeoff_window ? recent_spot_ : -1;
        }
        trick_.add(in.spot, dt);
        landing_spot_ = in.spot;
        return {};
    }
    if (action_) { // the trick ended this tick: its points show in the line's score shortly
        action_ = false;
        waiting_ = wait_ticks_;
        landing_spot_ = in.spot;
    }
    if (waiting_ > 0) {
        if (in.line_score > score_ + 0.5f) {
            const float scale = in.multiplier > 0.01f ? in.multiplier : 1.0f;
            const float points = std::max(0.0f, (unscaled(in.line_score, in.multiplier) - unscaled(score_, multiplier_)) * scale);
            score_ = in.line_score;
            multiplier_ = in.multiplier;
            waiting_ = 0;
            int spot = trick_.winner(0);
            if (spot < 0) spot = landing_spot_;
            if (spot < 0) spot = takeoff_spot_;
            return {TrickEvent::Kind::landed, spot, points};
        }
        if (--waiting_ > 0) return {};
    }
    // No trick pending: the line's score follows whatever else moves it (its end resets it to 0).
    score_ = in.line_score;
    multiplier_ = in.multiplier;
    return {};
}

// ---- board ------------------------------------------------------------------------------
namespace {
// Whether claim a beats claim b for a spot.
bool beats(Player a, const Claim &ca, Player b, const Claim &cb) {
    if (ca.score != cb.score) return ca.score > cb.score;
    if (ca.ms != cb.ms) return ca.ms < cb.ms;
    return a < b;
}
} // namespace

ClaimOutcome Board::claim(int spot, Player who, float score, std::uint32_t ms) {
    ClaimOutcome out;
    out.spot = spot;
    out.player = who;
    out.score = score;
    if (spot < 0 || static_cast<std::size_t>(spot) >= claims_.size() || !who || !(score > 0) || !std::isfinite(score)) return out;
    out.before = owner(spot);
    out.before_score = top(spot);
    auto &claims = claims_[spot];
    const auto mine = claims.find(who);
    const bool raises = mine == claims.end() || score > mine->second.score || (score == mine->second.score && ms < mine->second.ms);
    if (raises) {
        claims[who] = {score, ms};
        out.best = true;
    }
    // The player's history on the spot: their bests in the order they were made, each higher than
    // every earlier one, whatever order the claims reached this copy in (a later, higher claim may
    // have come first; a later, lower one heard first stops being a best once an earlier, higher one
    // comes), so that every copy with the same claims has the same history.
    auto &history = history_[spot];
    using Entry = std::tuple<std::uint32_t, Player, float>;
    const auto theirs = [&] {
        std::vector<Entry> entries;
        for (const auto &e : history)
            if (std::get<1>(e) == who) entries.push_back(e);
        return entries;
    };
    const auto before = theirs();
    history.insert({ms, who, score});
    auto entries = theirs();
    std::ranges::sort(entries, [](const Entry &x, const Entry &y) { // in time; at one time, the highest first
        return std::get<0>(x) != std::get<0>(y) ? std::get<0>(x) < std::get<0>(y) : std::get<2>(x) > std::get<2>(y);
    });
    float best = 0;
    for (const auto &e : entries) {
        if (std::get<2>(e) > best) best = std::get<2>(e);
        else history.erase(e);
    }
    out.kept = theirs() != before;
    out.after = owner(spot);
    return out;
}
std::size_t Board::history_size(int spot) const {
    return spot < 0 || static_cast<std::size_t>(spot) >= history_.size() ? 0 : history_[spot].size();
}
std::vector<Event> Board::history(int spot) const {
    std::vector<Event> events;
    if (spot < 0 || static_cast<std::size_t>(spot) >= history_.size()) return events;
    for (const auto &[ms, who, score] : history_[spot]) events.push_back({who, score, ms});
    return events;
}
Player Board::owner(int spot) const {
    if (spot < 0 || static_cast<std::size_t>(spot) >= claims_.size()) return 0;
    Player best = 0;
    const Claim *top = nullptr;
    for (const auto &[who, claim] : claims_[spot])
        if (!top || beats(who, claim, best, *top)) {
            best = who;
            top = &claim;
        }
    return best;
}
float Board::top(int spot) const {
    const auto who = owner(spot);
    return who ? claims_[spot].at(who).score : 0.0f;
}
float Board::best(int spot, Player who) const {
    if (spot < 0 || static_cast<std::size_t>(spot) >= claims_.size()) return 0;
    const auto it = claims_[spot].find(who);
    return it == claims_[spot].end() ? 0.0f : it->second.score;
}
int Board::owned(Player who) const {
    int count = 0;
    for (std::size_t i = 0; i < claims_.size(); ++i)
        if (who && owner(static_cast<int>(i)) == who) ++count;
    return count;
}
float Board::owned_total(Player who) const {
    float total = 0;
    for (std::size_t i = 0; i < claims_.size(); ++i)
        if (who && owner(static_cast<int>(i)) == who) total += top(static_cast<int>(i));
    return total;
}
std::vector<Board::Standing> Board::standings() const {
    std::map<Player, Standing> by;
    for (const auto &claims : claims_)
        for (const auto &[who, claim] : claims) by[who].player = who;
    for (std::size_t i = 0; i < claims_.size(); ++i)
        if (const auto who = owner(static_cast<int>(i))) {
            ++by[who].spots;
            by[who].total += top(static_cast<int>(i));
        }
    std::vector<Standing> result;
    for (const auto &[who, standing] : by) result.push_back(standing);
    std::ranges::sort(result, [](const Standing &a, const Standing &b) {
        if (a.spots != b.spots) return a.spots > b.spots;
        if (a.total != b.total) return a.total > b.total;
        return a.player < b.player;
    });
    return result;
}

RoundStats round_stats(const Board &board, std::uint32_t end_ms, const Teams &teams) {
    RoundStats stats;
    stats.spot_hold.assign(board.spots(), {0, 0.0});
    const auto team_of = [&](Player who) { const auto it = teams.find(who); return it == teams.end() ? -1 : it->second; };
    for (std::size_t spot = 0; spot < board.spots(); ++spot) {
        const auto events = board.history(static_cast<int>(spot));
        std::map<Player, double> held;
        const Event *top = nullptr;
        std::uint32_t since = 0;
        for (const auto &e : events) {
            if (e.ms > end_ms) break;
            if (top && !beats(e.player, {e.score, e.ms}, top->player, {top->score, top->ms})) continue;
            if (!top || top->player != e.player) {
                if (top) {
                    held[top->player] += (e.ms - since) / 1000.0;
                    const bool rival = teams.empty() ? top->player != e.player : team_of(top->player) != team_of(e.player);
                    if (rival) ++stats.steals[e.player];
                }
                since = e.ms;
            }
            top = &e;
        }
        if (top) held[top->player] += (end_ms - since) / 1000.0;
        for (const auto &[who, seconds] : held) {
            stats.hold[who] += seconds;
            if (seconds > stats.spot_hold[spot].second) stats.spot_hold[spot] = {who, seconds};
        }
        for (const auto &e : events)
            if (e.ms <= end_ms && e.score > stats.best.score) {
                stats.best = e;
                stats.best_spot = static_cast<int>(spot);
            }
    }
    return stats;
}

std::vector<Ranked> ranked(const Board &board, const RoundStats &stats, const Teams &teams) {
    std::map<Player, Ranked> rows;
    const auto key = [&](Player who) -> Player {
        if (teams.empty()) return who;
        const auto it = teams.find(who);
        return it == teams.end() ? 0 : static_cast<Player>(it->second);
    };
    const auto row = [&](Player who) -> Ranked & {
        auto &r = rows[key(who)];
        r.player = key(who);
        return r;
    };
    for (std::size_t spot = 0; spot < board.spots(); ++spot)
        for (const auto &e : board.history(static_cast<int>(spot)))
            if (teams.empty() || teams.contains(e.player)) row(e.player);
    for (std::size_t spot = 0; spot < board.spots(); ++spot)
        if (const auto who = board.owner(static_cast<int>(spot)); who && (teams.empty() || teams.contains(who))) {
            ++row(who).spots;
            row(who).total += board.top(static_cast<int>(spot));
        }
    for (const auto &[who, seconds] : stats.hold)
        if (teams.empty() || teams.contains(who)) row(who).hold += seconds;
    for (const auto &[who, count] : stats.steals)
        if (teams.empty() || teams.contains(who)) row(who).steals += count;
    std::vector<Ranked> result;
    for (const auto &[who, r] : rows) result.push_back(r);
    std::ranges::sort(result, [&](const Ranked &a, const Ranked &b) {
        if (a.spots != b.spots) return a.spots > b.spots;
        if (a.total != b.total) return a.total > b.total;
        return a.player < b.player;
    });
    return result;
}

Teams balance_teams(const Teams &placed, const std::vector<Player> &players) {
    Teams teams = placed;
    std::array<int, 2> size{};
    for (const auto &[who, team] : teams) ++size[team == 1 ? 1 : 0];
    auto sorted = players;
    std::ranges::sort(sorted);
    for (const auto who : sorted) {
        if (!who || teams.contains(who)) continue;
        const int team = size[1] < size[0] ? 1 : 0;
        teams[who] = team;
        ++size[team];
    }
    return teams;
}
}
