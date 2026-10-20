#include "Extension/AnimationStudio/trick_gestures.h"
#include "Engine/Core/Json/json.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace gestures = dingosdk::anim_studio::gestures;
using dingosdk::Json;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json inspect(const gestures::Gesture& gesture) {
    Json arcs = Json::array(), scripts = Json::array();
    for (const auto& arc : gesture.arcs) {
        for (const float number : {arc.from, arc.to, arc.low, arc.high, arc.wait})
            check(std::isfinite(number), "Gesture data contains nonfinite arc fields");
        check(arc.low >= 0 && arc.high >= arc.low, "Gesture magnitude bounds are ordered");
        arcs.push_back({{"from_degrees", arc.from}, {"to_degrees", arc.to}, {"low", arc.low}, {"high", arc.high}, {"wait_seconds", arc.wait}});
    }
    check(!gesture.arcs.empty(), "Exported gestures require arcs");
    for (const unsigned ms : {260u, 110u, 24u}) {
        const auto script = gestures::flick(gesture, ms);
        check(script.size() >= 3 && script.front().tick == 0, "Pad scripts start at tick zero");
        Json steps = Json::array();
        for (std::size_t i = 0; i < script.size(); ++i) {
            const auto& step = script[i];
            if (i) check(step.tick > script[i - 1].tick, "Pad script ticks are strictly ordered");
            steps.push_back({{"tick", step.tick}, {"rx", step.pad.sThumbRX}, {"ry", step.pad.sThumbRY}});
        }
        check(script.back().pad.sThumbRX == 0 && script.back().pad.sThumbRY == 0, "Every flick releases the stick");
        check(script.back().tick <= 200, "Gesture scripts stay bounded even for complex arcs");
        scripts.push_back({{"flick_ms", ms}, {"steps", std::move(steps)}});
    }
    return {{"enum", gesture.trick}, {"name", gesture.name}, {"nollie", gesture.nollie},
            {"hold_from", gesture.hold_from}, {"hold_to", gesture.hold_to}, {"hold_magnitude", gesture.hold_magnitude},
            {"arcs", std::move(arcs)}, {"scripts", std::move(scripts)}};
}
}
int main(int argc, char** argv) {
    try {
        check(gestures::stick_axis(-2) == -32767 && gestures::stick_axis(2) == 32767, "Stick axes clamp before conversion");
        check(gestures::stick_axis(std::numeric_limits<float>::quiet_NaN()) == 0 &&
              gestures::stick_axis(std::numeric_limits<float>::infinity()) == 0, "Nonfinite axes return neutral");
        gestures::Gesture example; example.arcs = {{170, 190, 0.8f, 1, 0}, {350, 10, 0.8f, 1, 0}, {0, 360, 0, 0.1f, 0}};
        (void)inspect(example);
        check(gestures::flick(example, 260).back().tick > gestures::flick(example, 24).back().tick, "Low-pop script moves more slowly than high-pop script");
        check(gestures::flick({}, 24).empty(), "Missing arcs produce no input");
        if (argc == 3) {
            const auto native = gestures::read(std::filesystem::absolute(argv[1]));
            Json entries = Json::array(); std::set<unsigned> ids;
            for (const auto& gesture : native) {
                check(gesture.trick >= 1 && gesture.trick <= 63 && ids.insert(gesture.trick).second, "Native gesture enums are valid and unique");
                entries.push_back(inspect(gesture));
            }
            std::ofstream out(argv[2], std::ios::binary);
            if (!out) throw std::runtime_error("Cannot write gesture inspection output");
            out << Json{{"schema", 1}, {"scope", "ground flip gestures; excludes fromgrind and underflip"},
                        {"validation", "read-only data and script checks; no gameplay input sent"}, {"gestures", std::move(entries)}}.dump(2);
            if (!out) throw std::runtime_error("Gesture inspection output write failed");
            std::cout << "Native gestures: " << native.size() << " decoded and inspected\n";
        } else if (argc != 1) throw std::runtime_error("Usage: dingosdk_trick_gesture_tests [game-root output.json]");
        std::cout << "animation_gestures: all passed\n";
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
