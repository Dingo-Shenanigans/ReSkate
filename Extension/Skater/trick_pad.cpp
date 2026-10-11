#include "trick_pad.h"
#include "style_layer.h"
#include "trick_gestures.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Game/Skater/style_pose.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace dingosdk::trick_pad {
namespace {
std::atomic<std::uint64_t> ticks{};
void log(const std::string &text) { logging::log(logging::Level::info, logging::Channel::skater, "Trick pad: {}", text); }
XINPUT_GAMEPAD stick(float x, float y, std::uint16_t buttons = 0) {
    XINPUT_GAMEPAD pad{};
    pad.sThumbRX = static_cast<SHORT>(std::clamp(x, -1.0f, 1.0f) * 32767);
    pad.sThumbRY = static_cast<SHORT>(std::clamp(y, -1.0f, 1.0f) * 32767);
    pad.wButtons = buttons;
    return pad;
}
// The game's gestures, read on a background thread at the first trick.
struct Gestures {
    std::mutex mutex;
    bool started{}, ready{};
    std::string issue;
    std::vector<style::TrickGesture> list;
};
Gestures &gestures() { static auto *value = new Gestures; return *value; }
void read_gestures() noexcept {
    auto &g = gestures();
    std::vector<style::TrickGesture> list;
    std::string issue;
    try {
        std::wstring exe(32768, L'\0');
        exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
        list = style::read_trick_gestures(std::filesystem::path(exe).parent_path());
    } catch (const std::exception &error) {
        issue = error.what();
    }
    log(issue.empty() ? std::format("read {} flip trick gestures from the game's data.", list.size()) : "the gestures could not be read: " + issue);
    std::lock_guard lock(g.mutex);
    g.list = std::move(list);
    g.issue = std::move(issue);
    g.ready = true;
}
// A stick position at an angle (degrees clockwise from up) and a magnitude.
std::array<float, 2> at_angle(float degrees, float magnitude) {
    const float radians = degrees / 57.29578f;
    return {std::sin(radians) * magnitude, std::cos(radians) * magnitude};
}
// The middle of the shorter way between two angles.
float middle(float from, float to) { return std::fmod(from + std::remainder(to - from, 360.0f) / 2 + 360.0f, 360.0f); }
// The arcs' middles, each at the middle of its magnitude range. The first is the crouch, against the stick's edge: a stick held
// between the middle and the edge is a manual.
std::vector<std::array<float, 2>> arc_points(const style::TrickGesture &gesture) {
    std::vector<std::array<float, 2>> points;
    for (const auto &arc : gesture.arcs) {
        // An arc near the middle is the stick let go.
        float magnitude = arc.high <= 0.25f ? 0.0f : (arc.low + arc.high) / 2;
        if (points.empty() && arc.high >= 1.0f) magnitude = 1.0f;
        points.push_back(at_angle(middle(arc.from, arc.to), magnitude));
    }
    return points;
}
// A crouch of 18 ticks at the first arc, then the stick moves in straight lines through the other arcs over `flick_ms`, one
// tick at least for each arc, and is let go. The game reads the stick once a tick and measures the gesture's speed from this
// move: it sets the pop height and the flip. The board starts to lift when the stick leaves the crouch and pops when the game
// knows the trick, so each arc gets no more ticks than the flick needs.
std::vector<overlay::PadStep> flick(const style::TrickGesture &gesture, std::uint32_t flick_ms) {
    constexpr std::uint32_t crouch = 18, release = 4, end = 36;
    const auto points = arc_points(gesture);
    const auto moves = static_cast<std::uint32_t>(std::max<std::size_t>(points.size() - 1, 1));
    const std::uint32_t total = std::max(moves, to_ticks(flick_ms));
    std::vector<overlay::PadStep> script{{0, stick(points[0][0], points[0][1])}};
    std::uint32_t tick = crouch;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto &a = points[i - 1], &b = points[i];
        // After the move out of the crouch, the stick turns 90 degrees at most in a tick: the game misses an arc after a longer turn.
        const float turn = std::abs(std::remainder(std::atan2(b[0], b[1]) - std::atan2(a[0], a[1]), 6.2831853f)) * 57.29578f;
        const auto least = i > 1 ? static_cast<std::uint32_t>(std::ceil(turn / 90.0f)) : 1u;
        const std::uint32_t share = std::max(total / moves + (i - 1 < total % moves ? 1 : 0), least);
        for (std::uint32_t t = 1; t <= share; ++t) {
            const float f = static_cast<float>(t) / static_cast<float>(share);
            script.push_back({tick + t - 1, stick(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)});
        }
        tick += share;
    }
    script.push_back({tick - 1 + release, stick(0, 0)});
    script.push_back({tick - 1 + end, stick(0, 0)});
    return script;
}
// Starts the read of the game's gestures on first use. True once they are read. Holds the gestures' lock.
bool gestures_read(Gestures &g) {
    if (g.ready) return true;
    if (!std::exchange(g.started, true)) try {
            std::thread(read_gestures).detach();
        } catch (...) {
            g.started = false;
        }
    return false;
}
} // namespace

bool gestures_ready() {
    auto &g = gestures();
    std::lock_guard lock(g.mutex);
    return gestures_read(g);
}
bool play(std::string_view gesture, std::string_view steps, std::string &error) {
    std::vector<overlay::PadStep> script;
    // For a trick, `steps` may give the flick's duration in milliseconds. A fast flick gives the highest pop.
    std::uint32_t flick_ms{default_flick_ms};
    if (gesture != "steps" && !steps.empty() && (sscanf_s(std::string(steps).c_str(), "%u", &flick_ms) != 1 || flick_ms > 1000)) {
        error = "Give the flick's duration in milliseconds, 0 to 1000.";
        return false;
    }
    const auto named = std::ranges::find(style::flip_trick_names.begin() + 1, style::flip_trick_names.end(), gesture);
    if (named != style::flip_trick_names.end()) {
        const auto trick = static_cast<std::uint8_t>(named - style::flip_trick_names.begin());
        auto &g = gestures();
        std::lock_guard lock(g.mutex);
        if (!gestures_read(g)) {
            error = "Reading the game's gestures. Try again in a few seconds.";
            return false;
        }
        const auto found = std::ranges::find(g.list, trick, &style::TrickGesture::trick);
        if (found == g.list.end()) {
            error = g.issue.empty() ? "The game's data has no gesture for that trick." : "The gestures could not be read: " + g.issue;
            return false;
        }
        script = flick(*found, flick_ms);
    } else if (gesture == "mount") {
        // Y also has a use on the board, so a skater who is on it already is left alone.
        std::uint32_t physics{};
        std::int32_t flip{};
        if (style_layer::local_trick(physics, flip) && physics != addr::style::offboard_state) {
            log(std::format("mount skipped: the skater is in physics state {}, not on foot.", physics));
            return true;
        }
        script = {{0, stick(0, 0, XINPUT_GAMEPAD_Y)}, {9, stick(0, 0)}, {24, stick(0, 0)}};
    } else if (gesture == "steps") {
        std::istringstream text{std::string(steps)};
        for (std::string item; text >> item;) {
            unsigned at{}, buttons{}, left_trigger{}, right_trigger{};
            float x{}, y{}, lx{}, ly{};
            if (sscanf_s(item.c_str(), "%u:%f,%f,%x,%f,%f,%u,%u", &at, &x, &y, &buttons, &lx, &ly, &left_trigger, &right_trigger) < 3) {
                error = "Use ms:rx,ry[,buttons hex,lx,ly,lt,rt] items.";
                return false;
            }
            auto pad = stick(x, y, static_cast<std::uint16_t>(buttons));
            pad.sThumbLX = static_cast<SHORT>(std::clamp(lx, -1.0f, 1.0f) * 32767);
            pad.sThumbLY = static_cast<SHORT>(std::clamp(ly, -1.0f, 1.0f) * 32767);
            pad.bLeftTrigger = static_cast<BYTE>(std::min(left_trigger, 255u));
            pad.bRightTrigger = static_cast<BYTE>(std::min(right_trigger, 255u));
            script.push_back({to_ticks(at), pad});
        }
        if (script.empty() || !std::ranges::is_sorted(script, {}, &overlay::PadStep::tick)) {
            error = "Give the steps in time order.";
            return false;
        }
    } else {
        error = "Unknown gesture.";
        return false;
    }
    overlay::play_pad(std::move(script));
    return true;
}
std::uint64_t game_ticks() noexcept { return ticks.load(std::memory_order_acquire); }
void tick() noexcept {
    ticks.fetch_add(1, std::memory_order_acq_rel);
    overlay::pad_tick();
}
} // namespace dingosdk::trick_pad
