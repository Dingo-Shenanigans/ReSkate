#include "domination.h"
#include "domination_internal.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/World/level_loading.h"
#include <algorithm>
#include <format>
#include <sstream>

// Park-making: height maps (scan), a teleport facing a heading (tp), and surveys of many places
// (domination_internal.h, README.md "Making a park").
namespace dingosdk::domination::detail {
// ---- scan -------------------------------------------------------------------------------
void finish_scan(State &s) {
    auto &scan = *s.scan;
    const auto seconds = std::chrono::duration<double>(Clock::now() - scan.started).count();
    const auto directory = data_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto path = directory / std::filesystem::path("scan-" + stamp() + ".bin");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) {
        file.write("DOMSCAN1", 8);
        file.write(reinterpret_cast<const char *>(&scan.nx), 4);
        file.write(reinterpret_cast<const char *>(&scan.nz), 4);
        for (const float value : {scan.x0, scan.z0, scan.step, scan.top, scan.bottom}) file.write(reinterpret_cast<const char *>(&value), 4);
        file.write(reinterpret_cast<const char *>(scan.heights.data()), static_cast<std::streamsize>(scan.heights.size() * sizeof(float)));
    }
    if (file) {
        s.last_scan = path.string();
        say(logging::Level::info, std::format("scan done: {} x {} points, {} without a hit, {:.1f} s. File: {}", scan.nx, scan.nz,
                                              scan.misses, seconds, s.last_scan));
    } else {
        s.last_scan = "error: could not write " + path.string();
        say(logging::Level::warning, "scan " + s.last_scan);
    }
    s.scan.reset();
}
void tick_scan(State &s) {
    if (!s.scan) return;
    auto &scan = *s.scan;
    const auto until = Clock::now() + scan_budget;
    while (scan.next < scan.heights.size() && Clock::now() < until) {
        const auto i = scan.next % scan.nx, j = scan.next / scan.nx;
        const float x = scan.x0 + static_cast<float>(i) * scan.step, z = scan.z0 + static_cast<float>(j) * scan.step;
        const auto hit = local_ground_height(x, z, scan.top, scan.bottom);
        if (!hit) ++scan.misses;
        scan.heights[scan.next++] = hit ? *hit : std::numeric_limits<float>::quiet_NaN();
    }
    if (scan.next == scan.heights.size()) finish_scan(s);
}

// ---- survey -------------------------------------------------------------------------------
bool face(const Vec3 &p, float heading) { return teleport_local_skater_heading({p[0], p[1], p[2]}, heading * 3.14159265f / 180); }
void tick_survey(State &s, Clock::time_point now) {
    if (!s.survey || now < s.survey->until || !level_content_active() || !trainer::telemetry().skater) return;
    auto &v = *s.survey;
    if (v.next >= v.places.size()) {
        say(logging::Level::info, std::format("survey done: {} places in {:.0f} s, {} without ground found", v.places.size(),
                                              std::chrono::duration<double>(now - v.started).count(), v.missed));
        s.survey.reset();
        return;
    }
    const auto &p = v.places[v.next];
    if (v.phase == 0) { // a little above the ground expected there; the area loads around the skater
        face({p[0], p[1] + 1.0f, p[2]}, 0);
        v.phase = 1;
        v.until = now + survey_settle;
        return;
    }
    if (v.phase == 1) {
        const auto ground = local_ground_height(p[0], p[2], p[1] + 40, p[1] - 40);
        if (!ground) ++v.missed;
        v.ground = ground ? *ground : p[1];
        if (v.half > 0 && !s.scan) s.scan = scan_job(p[0] - v.half, p[2] - v.half, p[0] + v.half, p[2] + v.half, v.step, v.ground + 60, v.ground - 60);
        v.phase = 2;
        return;
    }
    if (v.phase == 2) {
        if (s.scan) return; // the height map first
        v.phase = 3;
    }
    if (v.phase == 7) {
        ++v.next;
        v.phase = 0;
        return;
    }
    const float heading = 90.0f * static_cast<float>(v.phase - 3);
    face({p[0], v.ground + 0.5f, p[2]}, heading);
    say(logging::Level::info, std::format("survey {}/{} at ({:.1f}, {:.2f}, {:.1f}) facing {:.0f}", v.next + 1, v.places.size(), p[0], v.ground, p[2], heading));
    ++v.phase;
    v.until = now + survey_look;
}

} // namespace dingosdk::domination::detail

namespace dingosdk::domination {
using namespace detail;
std::string scan(const std::vector<std::string> &words) {
    // x0 z0 x1 z1 [step] [top] [bottom]
    if (words.size() < 4 || words.size() > 7) return "usage: domination scan <x0> <z0> <x1> <z1> [step 0.25] [top 60] [bottom -40]";
    std::array<float, 7> v{0, 0, 0, 0, .25f, 60, -40};
    for (std::size_t n = 0; n < words.size(); ++n) {
        const auto value = number(words[n]);
        if (!value) return "error: not a number: " + words[n];
        v[n] = *value;
    }
    const float x0 = std::min(v[0], v[2]), x1 = std::max(v[0], v[2]), z0 = std::min(v[1], v[3]), z1 = std::max(v[1], v[3]);
    const float step = v[4], top = v[5], bottom = v[6];
    if (!(step >= .02f && step <= 10) || !(top > bottom)) return "error: step must be 0.02 to 10 and top above bottom.";
    // (Counted in doubles first: far-off coordinates would overflow an integer.)
    const double cx = std::floor((static_cast<double>(x1) - x0) / step) + 1, cz = std::floor((static_cast<double>(z1) - z0) / step) + 1;
    if (cx * cz > static_cast<double>(scan_limit)) return std::format("error: {:.0f} points; at most {}.", cx * cz, scan_limit);
    const auto nx = static_cast<std::uint64_t>(cx), nz = static_cast<std::uint64_t>(cz);
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.scan) return "error: a scan is running.";
    s.scan = scan_job(x0, z0, x1, z1, step, top, bottom);
    const auto reply = std::format("Scanning {} x {} points ({:.1f} to {:.1f}, {:.1f} to {:.1f}, step {:g}).", nx, nz, x0, x1, z0, z1, step);
    say(logging::Level::info, reply);
    return reply;
}

std::string teleport(const std::vector<std::string> &words) {
    if (words.size() < 3 || words.size() > 4) return "usage: domination tp <x> <y|ground> <z> [heading]";
    std::array<float, 4> v{};
    for (std::size_t n = 0; n < words.size(); ++n) {
        if (n == 1 && lower(words[n]) == "ground") { // standing on whatever is below (found once there)
            v[n] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        const auto value = number(words[n]);
        if (!value) return "error: not a number: " + words[n];
        v[n] = *value;
    }
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (multiplayer_session_active() && !session_noclip_allowed()) return "error: the host has turned off teleporting.";
    s.teleport = std::pair<Vec3, float>{{v[0], v[1], v[2]}, v[3]};
    const auto y = std::isnan(v[1]) ? std::string("the ground") : std::format("{:g}", v[1]);
    return std::format("Teleporting to ({:g}, {}, {:g}) facing {:g} (degrees from +z towards +x).", v[0], y, v[2], v[3]);
}

std::string survey(const std::vector<std::string> &words) {
    auto &s = state();
    if (words.size() == 1 && words[0] == "stop") {
        std::lock_guard lock(s.mutex);
        if (!s.survey) return "No survey is running.";
        const auto done = s.survey->next;
        s.survey.reset();
        return std::format("Survey stopped after {} places.", done);
    }
    if (words.empty() || words.size() > 3) return "usage: domination survey <places file> [half size 60] [step 0.5] | stop";
    float half = 60, step = 0.5f;
    for (std::size_t n = 1; n < words.size(); ++n) {
        const auto value = number(words[n]);
        if (!value) return "error: not a number: " + words[n];
        (n == 1 ? half : step) = *value;
    }
    if (half < 0 || half > 500 || !(step >= .1f && step <= 10)) return "error: half size 0 to 500, step 0.1 to 10.";
    const double side = std::floor(2.0 * half / step) + 1;
    if (side * side > static_cast<double>(scan_limit)) return std::format("error: {:.0f} points per place; at most {} (a bigger step, or a smaller square).", side * side, scan_limit);
    // One place per line: x y z (y: the ground expected there); # starts a comment.
    std::ifstream file{std::filesystem::path(words[0])};
    if (!file) return "error: could not read " + words[0];
    Survey v;
    std::string line;
    while (std::getline(file, line) && v.places.size() < 10000) {
        if (line.empty() || line[0] == '#') continue;
        std::array<float, 3> p{};
        std::istringstream in(line);
        if (in >> p[0] >> p[1] >> p[2]) v.places.push_back(p);
    }
    if (v.places.empty()) return "error: no places in " + words[0] + " (one \"x y z\" per line).";
    v.half = half;
    v.step = step;
    v.started = Clock::now();
    std::lock_guard lock(s.mutex);
    if (multiplayer_session_active() && !session_noclip_allowed()) return "error: the host has turned off teleporting.";
    if (s.survey) return "error: a survey is running (domination survey stop).";
    const auto count = v.places.size();
    s.survey = std::move(v);
    const auto reply = std::format("Surveying {} places: about {:.0f} minutes.", count, count * 9.5 / 60);
    say(logging::Level::info, reply);
    return reply;
}

} // namespace dingosdk::domination
