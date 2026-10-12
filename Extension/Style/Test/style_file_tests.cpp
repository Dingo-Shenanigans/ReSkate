#include "Extension/Style/style_file.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
// Any other exception ends the test: decode_style throws only runtime_error.
bool refused(std::string_view text) {
    try {
        (void)decode_style(text);
        return false;
    } catch (const NewerStyle &) {
        return false;
    } catch (const std::runtime_error &) {
        return true;
    }
}
bool newer(std::string_view text) {
    try {
        (void)decode_style(text);
        return false;
    } catch (const NewerStyle &) {
        return true;
    } catch (const std::runtime_error &) {
        return false;
    }
}
} // namespace

int style_presets_tests();
int style_internal_tests();

int main() {
    Style style;
    style.times[2] = {0.25f, 1.0f, 1.5f, 2.75f};
    style.rotations[{Target{true, 2, 0}, "LeftArm"}] = {0, 110, -12.5f};
    style.rotations[{Target{true, 2, 3}, "Hips"}] = {0, -4, 0};
    style.rotations[{Target{false, 1}, "Head"}] = {0, 0, 30};
    style.blend_outs[Target{true, 2, 1}] = 300;
    const auto text = encode_style(style);
    check(decode_style(text) == style, "a saved style reads back, keyframe times and blend outs and all");
    check(text.find("blend_out_ms") != std::string::npos && decode_style(text).blend_outs.size() == 1,
          "only a keyframe with a blend out saves one");
    check(text.find("\"kickflip\"") != std::string::npos && text.find("\"grind\"") != std::string::npos,
          "tricks and states are saved by name");
    check(text.find("\"format\": 1") != std::string::npos, "a style is saved as format 1");
    check(text.find("skeleton_joints") == std::string::npos, "a style does not save the skeleton's joint count");
    {
        const auto style_at = [](std::vector<float> times) {
            Style unordered;
            for (std::uint8_t key = 0; key < times.size(); ++key) {
                unordered.rotations[{Target{true, 2, key}, "Head"}] = {10.0f * (key + 1), 0, 0};
                unordered.blend_outs[Target{true, 2, key}] = 100.0f * (key + 1);
            }
            unordered.times[2] = std::move(times);
            return unordered;
        };
        const auto head = [](const Style &loaded, std::uint8_t key) { return loaded.rotations.at({Target{true, 2, key}, "Head"})[0]; };
        const auto saved = encode_style(style_at({2.98f, 2.97f, 0.99f}));
        check(saved.find("0.99") < saved.find("2.97") && saved.find("2.97") < saved.find("2.98"), "keyframes are saved in time order");
        // Not a reversal of the saved order, so a keyframe's place cannot pass for its ID.
        const auto rotated = decode_style(encode_style(style_at({2.98f, 0.99f, 2.97f})));
        check(rotated.keys(2) == std::vector<float>{0.99f, 2.97f, 2.98f} && head(rotated, 0) == 20 && head(rotated, 1) == 30 &&
                  head(rotated, 2) == 10 &&
                  rotated.blend_outs == BlendOuts{{Target{true, 2, 0}, 200.0f}, {Target{true, 2, 1}, 300.0f}, {Target{true, 2, 2}, 100.0f}},
              "each rotation and blend-out stays at its keyframe's time in any order");
    }
    Style plain;
    plain.rotations[{Target{true, 3, 1}, "Head"}] = {10, 0, 0};
    const auto read = decode_style(encode_style(plain));
    check(read.rotations.empty() && read.keys(3).empty(), "a trick starts with no keyframes");

    const auto loose = decode_style(R"({"format":1,"tricks":{
        "kickflip":[{"time":9,"joints":{"LeftArm":[500,0,-500],"Reference":[10,0,0],"Head":[0,0,0],"Spine":[1,2]}},
                    {"joints":{"Head":[9,0,0]}}],
        "moonflip":[{"time":1,"joints":{"Head":[10,0,0]}}],"riding":[{"time":1,"joints":{"Head":[10,0,0]}}]}})");
    check(loose.rotations.size() == 1 && loose.rotations.begin()->second == std::array<float, 3>{max_degrees, 0, -max_degrees} &&
              loose.keys(2) == std::vector<float>{trick_end},
          "angles and times are clamped; unknown tricks, placement joints, untimed keyframes and empty rotations are dropped");
    const auto blends = decode_style(R"({"format":1,"tricks":{"kickflip":[{"time":1},{"time":1.5,"blend_out_ms":99999},
        {"time":2,"blend_out_ms":-5},{"time":2.5,"blend_out_ms":"soon"}]}})");
    check(blends.blend_outs == BlendOuts{{Target{true, 2, 1}, max_blend_out_ms}},
          "a keyframe without a blend out goes to the next keyframe; a blend out is clamped, and one that is not a time is dropped");
    Style stray;
    stray.blend_outs[Target{true, 3, 1}] = 200;
    check(decode_style(encode_style(stray)).blend_outs.empty(), "a blend out on a missing keyframe is not saved");
    std::string many =R"({"format":1,"tricks":{"ollie":[)";
    for (int i = 0; i < 40; ++i) many += std::string(i ? "," : "") + R"({"time":1})";
    check(decode_style(many + "]}}").keys(1).size() == max_keys, "a trick keeps no more keyframes than the editor allows");

    const auto twice = decode_style(R"({"format":1,"tricks":{
        "kickflip":[{"time":1,"joints":{"Head":[10,0,0]}},{"time":2,"joints":{"Head":[20,0,0]}}],
        "Kickflip":[{"time":0.5,"joints":{"Head":[30,0,0]}}]}})");
    check((twice.keys(2) == std::vector<float>{1, 2} && twice.rotations.size() == 2) ||
              (twice.keys(2) == std::vector<float>{0.5f} && twice.rotations.size() == 1),
          "a trick named twice keeps one entry, with no stray rotations");

    const auto tied = decode_style(R"({"format":1,"tricks":{"kickflip":[{"time":1},{"time":1},{"time":3},{"time":3}]}})");
    check(tied.keys(2) == std::vector<float>{1.0f, 1.0f + key_nudge, trick_end, last_key_time},
          "keyframes that share a time in a file are kept apart");
    check(decode_style(encode_style(tied)).keys(2) == std::vector<float>{1.0f, 1.0f + key_nudge, last_key_time, trick_end},
          "and a save and a load keep those times, in time order");

    check(refused("[]") && refused("not json") && refused("{}") && refused(R"({"format":"2"})") && refused(R"({"format":1.5})") &&
              refused(R"({"format":true})") && refused(R"({"format":null})") && refused(R"({"format":0})") &&
              refused(R"({"format":-2})"),
          "text that is not a style is refused");
    check(newer(R"({"format":2})") && newer(R"({"format":2.0})") && newer(R"({"format":3,"tricks":{"kickflip":[{"time":1}]}})"),
          "a whole format above 1 is a preset from a newer ReSkate");
    check(std::string_view(NewerStyle().what()) == "this preset is from a newer ReSkate", "and the issue says so");
    check(refused(R"({"format":1,"tricks":{"kickflip":[{"time":1,"joints":{"Head":["a",0,0]}}]}})"),
          "an angle that is not a number is refused");
    check(refused(std::string(maximum_style_bytes + 1, ' ')), "an oversized file is refused");

    const auto spelled = decode_style(R"({"format":1,"tricks":{"kickflip":[{"time":1,"joints":{"hips":[10,0,0]}}]}})");
    check(spelled.rotations.size() == 1 && spelled.rotations.begin()->first.joint == "Hips" &&
              encode_style(spelled).find("\"Hips\"") != std::string::npos,
          "a joint name in another case keeps its rotation and is saved with the editor's spelling");
    if (!failures) std::cout << "style file tests passed\n";
    const int presets = style_presets_tests(), internal = style_internal_tests();
    return failures || presets || internal ? 1 : 0;
}
