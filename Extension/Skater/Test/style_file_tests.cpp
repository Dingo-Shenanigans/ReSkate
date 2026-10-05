// Style files: saved data reads back, and a foreign file cannot hold data that a style does not permit.
#include "Extension/Skater/style_file.h"
#include <iostream>

namespace {
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
bool refused(std::string_view text) {
    try {
        (void)decode_style(text);
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
} // namespace

int main() {
    Style style;
    style.times[2] = {0.25f, 1.0f, 2.75f, 1.5f};
    style.rotations[{Target{true, 2, 0}, "LeftArm"}] = {0, 110, -12.5f};
    style.rotations[{Target{true, 2, 3}, "Hips"}] = {0, -4, 0};
    style.rotations[{Target{false, 1}, "Head"}] = {0, 0, 30};
    const auto text = encode_style(style);
    check(decode_style(text) == style, "a saved style reads back, keyframe times and all");
    check(text.find("\"kickflip\"") != std::string::npos && text.find("\"grind\"") != std::string::npos,
          "tricks and states are saved by name");
    // A trick starts with no keyframes. A rotation on a missing keyframe is not saved.
    Style plain;
    plain.rotations[{Target{true, 3, 1}, "Head"}] = {10, 0, 0};
    const auto read = decode_style(encode_style(plain));
    check(read.rotations.empty() && read.keys(3).empty(), "a trick starts with no keyframes");

    const auto loose = decode_style(R"({"format":2,"tricks":{
        "kickflip":[{"time":9,"joints":{"LeftArm":[500,0,-500],"Reference":[10,0,0],"Head":[0,0,0],"Spine":[1,2]}},{"joints":{"Head":[9,0,0]}}],
        "moonflip":[{"time":1,"joints":{"Head":[10,0,0]}}],"riding":[{"time":1,"joints":{"Head":[10,0,0]}}]}})");
    check(loose.rotations.size() == 1 && loose.rotations.begin()->second == std::array<float, 3>{max_degrees, 0, -max_degrees} &&
              loose.keys(2) == std::vector<float>{trick_end},
          "angles and times are clamped; unknown tricks, placement joints, untimed keyframes and empty rotations are dropped");
    std::string many = R"({"format":2,"tricks":{"ollie":[)";
    for (int i = 0; i < 40; ++i) many += std::string(i ? "," : "") + R"({"time":1})";
    check(decode_style(many + "]}}").keys(1).size() == max_keys, "a trick keeps no more keyframes than the editor allows");

    const auto twice = decode_style(R"({"format":2,"tricks":{
        "kickflip":[{"time":1,"joints":{"Head":[10,0,0]}},{"time":2,"joints":{"Head":[20,0,0]}}],
        "Kickflip":[{"time":0.5,"joints":{"Head":[30,0,0]}}]}})");
    check((twice.keys(2) == std::vector<float>{1, 2} && twice.rotations.size() == 2) || (twice.keys(2) == std::vector<float>{0.5f} && twice.rotations.size() == 1),
          "a trick named twice keeps one entry, with no stray rotations");

    check(refused("[]") && refused("{\"format\":3}") && refused("{\"format\":1}") && refused("not json"), "text that is not a style is refused");
    check(refused(R"({"format":2,"tricks":{"kickflip":[{"time":1,"joints":{"Head":["a",0,0]}}]}})"), "an angle that is not a number is refused");
    check(refused(std::string(maximum_style_bytes + 1, ' ')), "an oversized file is refused");
    if (!failures) std::cout << "style file tests passed\n";
    return failures ? 1 : 0;
}
