// Style editor clips: retime, sample, the file format, and the cut of a recorded jump into pop, catch and landing.
#include "Extension/Skater/style_takes.h"
#include <format>
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
bool close_to(float a, float b, float within = 1e-4f) { return std::abs(a - b) <= within; }
// A clip of `frames` frames 10 ms apart whose parts change at the given frames.
Clip skated(std::size_t frames, std::size_t flick, std::size_t caught, std::size_t landed) {
    Clip clip{2, {}, {}};
    for (std::size_t i = 0; i < frames; ++i) {
        ClipFrame frame;
        frame.at = static_cast<std::uint32_t>(i * 10);
        // A rough pace inside each part, which retime replaces.
        frame.time = i < flick ? -1.0f : i < caught ? 0.9f : i < landed ? 1.2f : i < landed + 45 ? 2.0f : 4.0f;
        frame.pose.root.position = {static_cast<float>(i), 0, 0};
        frame.pose.skater.assign(3, Transform{});
        frame.pose.skater[1].position = {0, static_cast<float>(i), 0};
        frame.pose.board.assign(2, Transform{});
        clip.frames.push_back(frame);
    }
    return clip;
}
bool refused(std::span<const std::uint8_t> bytes) {
    try {
        (void)decode_clip(bytes);
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
} // namespace

int main() {
    // A low jump and a high jump give the same times at the same points of the trick.
    auto low = skated(120, 20, 40, 60), high = skated(200, 20, 70, 130);
    check(retime(low) && retime(high), "a whole trick can be placed on the timeline");
    check(close_to(low.frames[20].time, 0) && close_to(low.frames[40].time, 1) && close_to(low.frames[60].time, 2), "its flick, catch and touchdown sit at 0, 1 and 2");
    check(close_to(low.frames[30].time, 0.5f) && close_to(high.frames[45].time, 0.5f), "halfway to the catch is 0.5 whatever the jump's height");
    check(close_to(low.frames[50].time, 1.5f) && close_to(high.frames[100].time, 1.5f), "and halfway down is 1.5");
    check(close_to(low.frames[105].time, trick_end) && low.frames[110].time > trick_end, "the landing ends a moment after touchdown");
    check(low.frames[10].time < 0 && low.frames[10].time > low.frames[5].time, "the lead-in counts up to the flick in seconds");
    auto slam = skated(100, 20, 40, 60);
    for (auto &frame : slam.frames)
        if (frame.time > 1) frame.time = 4; // never caught and landed
    check(!retime(slam), "a trick with no landing is not a clip");
    auto bailed = skated(100, 20, 40, 60);
    for (auto &frame : bailed.frames)
        if (frame.time >= 2) frame.time = 4; // caught, then slammed
    check(!retime(bailed), "nor is one that was caught and then slammed");

    // sample blends between frames.
    check(close_to(sample(low, 0.5f).root.position[0], 30) && close_to(sample(low, 0.55f).root.position[0], 31), "a time on a frame gives that frame");
    check(close_to(sample(low, 0.525f).root.position[0], 30.5f, 1e-3f), "a time between two frames gives the pose between them");
    check(close_to(sample(low, -100).root.position[0], 0) && close_to(sample(low, 100).root.position[0], 119), "times off either end hold the end frames");
    check(duration(low) == 1190 && close_to(time_at(low, 300), 0.5f) && close_to(time_at(low, 305), 0.525f) && close_to(time_at(low, 302.5f), 0.5125f),
          "playback time maps onto the timeline, also between whole milliseconds");
    check(close_to(ms_at(low, 0.5f), 300, 1e-2f) && close_to(ms_at(low, 0.525f), 305, 1e-2f) && close_to(ms_at(low, -100), 0) && close_to(ms_at(low, 100), 1190),
          "a timeline time maps back onto playback time");
    for (const float ms : {0.0f, 155.0f, 401.5f, 777.0f, 1190.0f})
        check(close_to(ms_at(low, time_at(low, ms)), ms, 0.05f), "the two maps are each other's reverse");
    const auto halfway = blend(low.frames[0].pose, low.frames[10].pose, 0.5f);
    check(close_to(halfway.root.position[0], 5) && close_to(halfway.skater[1].position[1], 5), "two poses blend");
    Pose odd;
    odd.skater.assign(1, Transform{});
    check(close_to(blend(low.frames[0].pose, odd, 0.5f).root.position[0], 0), "poses of different shapes do not blend");

    // The recipe: one key for everything that shapes a recording.
    const Recipe recipe{recipe_version, "25414733", 24, 600};
    check(recipe_text(recipe) == std::format("recipe {} game 25414733 flick 24 still 600 lead {} follow {}", recipe_version, lead_ms, follow_ms),
          "a recipe reads as one line");
    check(recipe_key(recipe) == recipe_key(Recipe{recipe_version, "25414733", 24, 600}), "the same recipe gives the same key");
    for (auto changed : {Recipe{recipe_version + 1, "25414733", 24, 600}, Recipe{recipe_version, "25414734", 24, 600},
                         Recipe{recipe_version, "25414733", 30, 600}, Recipe{recipe_version, "25414733", 24, 500},
                         Recipe{recipe_version, "25414733", 24, 600, 700}, Recipe{recipe_version, "25414733", 24, 600, lead_ms, 900}})
        check(recipe_key(changed) != recipe_key(recipe), "a change to any part of the recipe changes the key");
    // The file, with how the clip was recorded.
    low.capture = {recipe_key(recipe), recipe_text(recipe), "1.2.3", 1760000000};
    const auto bytes = encode_clip(low);
    const auto back = decode_clip(bytes);
    check(back.trick == 2 && back.frames.size() == low.frames.size() && back.frames[33].time == low.frames[33].time &&
              back.frames[33].at == 330 && back.frames[33].pose.skater[1].position[1] == 33 && back.frames[33].pose.board.size() == 2,
          "a clip reads back from its file");
    check(back.capture == low.capture, "with its capture");
    const auto head = clip_capture(std::span(bytes).first(clip_header_bytes));
    check(head && *head == low.capture, "and the capture reads from the header alone");
    auto long_name = low;
    long_name.capture.reskate = std::string(40, 'v');
    check(decode_clip(encode_clip(long_name)).capture.reskate == std::string(31, 'v'), "a long version is cut to its field");
    auto other_format = bytes;
    other_format[4] = 7; // the format version
    check(!clip_capture(other_format) && refused(other_format), "a file of another format version is refused");
    auto damaged = bytes;
    damaged[damaged.size() / 2] ^= 0x5a;
    damaged.resize(damaged.size() - 9);
    check(refused(damaged) && refused(std::span(bytes).first(20)) && refused({}), "damaged or cut-short files are refused");
    auto wrong = bytes;
    wrong[8] = 99; // the trick number
    check(refused(wrong), "a file for a trick that does not exist is refused");

    // A recorded jump, `step` ms per frame: root 0, trajectory 1, deck 2 and the feet 3 and 4 under it. The board is in the air
    // from 1200 to 1840 ms, and the feet leave it until 1600 ms.
    const std::vector<std::int32_t> parents{-1, 0, 1, 1, 1};
    const auto recorded = [](std::uint32_t step, std::uint32_t until = 3000) {
        std::vector<RigFrame> frames;
        for (std::uint32_t at = 0; at < until; at += step) {
            RigFrame frame{at, std::vector<Transform>(5)};
            frame.joints[1].position = {static_cast<float>(at) * 0.001f, 0, 5};
            const bool air = at >= 1200 && at < 1840;
            const float up = air ? 0.5f * std::sin(static_cast<float>(at - 1200) / 640.0f * 3.14159f) + 0.06f : 0.0f;
            frame.joints[2].position = {0, 0.1f + up, 0};
            const float off = air && at < 1600 ? 0.3f : 0.0f;
            frame.joints[3].position = {0.2f, 0.2f + up + off, 0};
            frame.joints[4].position = {-0.2f, 0.2f + up + off, 0};
            frames.push_back(std::move(frame));
        }
        return frames;
    };
    const RigJoints joints{1, 2, 3, 4, parents};
    std::vector<Transform> board(3);
    board[0].position = {100, 0, 0};
    board[2].position = {100, 0.05f, 0};
    std::string why;
    const auto clip = clip_from_capture(3, recorded(16), joints, board, {}, why);
    check(clip && clip->trick == 3, "a recorded jump becomes a clip");
    if (clip) {
        const auto at = [&](float time) {
            return clip->began + std::ranges::find_if(clip->frames, [&](const ClipFrame &f) { return f.time >= time; })->at;
        };
        check(at(0) == 1120 && at(1) == 1600 && at(2) == 1840, "the flick, the feet meeting the board and the touchdown are found");
        check(clip->began == 1120 - lead_ms && clip->began + duration(*clip) + 16 >= 1840 + follow_ms, "the clip keeps time before the flick and after the touchdown");
        const auto &mid = std::ranges::find_if(clip->frames, [&](const ClipFrame &f) { return clip->began + f.at >= 1440; })->pose;
        check(close_to(mid.root.position[0], 1.44f, 1e-3f) && close_to(mid.board[0].position[0], 1.44f, 1e-3f) && mid.board[0].position[1] > 0.3f,
              "the clip's board follows the deck joint through the air");
        check(close_to(mid.board[2].position[1] - mid.board[0].position[1], 0.05f, 1e-3f), "and the board's own rig keeps its place on it");
    }
    // The cut is in milliseconds, so a faster frame rate keeps the same lead-in and follow-through.
    const auto fast = clip_from_capture(3, recorded(4), joints, board, {}, why);
    check(fast && clip && fast->began == clip->began && fast->frames.size() > 3 * clip->frames.size() &&
              std::abs(static_cast<int>(duration(*fast)) - static_cast<int>(duration(*clip))) <= 16,
          "a recording at 250 frames a second gives the same clip in time");
    check(!clip_from_capture(3, recorded(16, 1000), joints, board, {}, why) && !why.empty(), "a recording with no jump says so");
    check(!clip_from_capture(3, recorded(16, 1900), joints, board, {}, why) && !why.empty(), "nor does a recording that ends at the touchdown");
    if (!failures) std::cout << "style takes tests passed\n";
    return failures ? 1 : 0;
}
