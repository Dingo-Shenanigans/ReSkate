// Style editor clips: retime, sample, the file format, and the cut of Skatepedia's demonstration into pop, catch and landing.
#include "Extension/Skater/style_takes.h"
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
    Clip clip{2, false, {}};
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

    // The file.
    const auto bytes = encode_clip(low);
    const auto back = decode_clip(bytes);
    check(back.trick == 2 && !back.learned && back.frames.size() == low.frames.size() && back.frames[33].time == low.frames[33].time &&
              back.frames[33].at == 330 && back.frames[33].pose.skater[1].position[1] == 33 && back.frames[33].pose.board.size() == 2,
          "a clip reads back from its file");
    auto damaged = bytes;
    damaged[damaged.size() / 2] ^= 0x5a;
    damaged.resize(damaged.size() - 9);
    check(refused(damaged) && refused(std::span(bytes).first(20)) && refused({}), "damaged or cut-short files are refused");
    auto wrong = bytes;
    wrong[8] = 99; // the trick number
    check(refused(wrong), "a file for a trick that does not exist is refused");

    // The game's demonstration: root 0, trajectory 1, deck 2 and the feet 3 and 4 under it.
    const std::vector<std::int32_t> parents{-1, 0, 1, 1, 1};
    std::vector<RigFrame> shown;
    for (std::size_t i = 0; i < 140; ++i) {
        RigFrame frame{static_cast<std::uint32_t>(i * 16), std::vector<Transform>(5)};
        frame.joints[1].position = {static_cast<float>(i) * 0.1f, 0, 5};
        const bool air = i >= 50 && i < 90;
        const float up = air ? 0.5f * std::sin(static_cast<float>(i - 50) / 40.0f * 3.14159f) + 0.06f : 0.0f;
        frame.joints[2].position = {0, 0.1f + up, 0};
        // The feet leave the board during the flip and return at frame 75.
        const float off = air && i < 75 ? 0.3f : 0.0f;
        frame.joints[3].position = {0.2f, 0.2f + up + off, 0};
        frame.joints[4].position = {-0.2f, 0.2f + up + off, 0};
        shown.push_back(std::move(frame));
    }
    const RigJoints joints{1, 2, 3, 4, parents};
    std::vector<Transform> board(3);
    board[0].position = {100, 0, 0};
    board[2].position = {100, 0.05f, 0};
    std::string why;
    const auto learned = clip_from_capture(3, shown, joints, board, {}, why);
    check(learned && learned->learned && learned->trick == 3, "a demonstrated jump becomes a clip");
    if (learned) {
        const auto at = [&](float time) {
            return static_cast<std::size_t>(std::ranges::find_if(learned->frames, [&](const ClipFrame &f) { return f.time >= time; }) -
                                            learned->frames.begin()) + 21; // the clip starts 24 frames before the flick at 45
        };
        check(at(0) == 45 && at(1) == 75 && at(2) == 90, "the flick, the feet meeting the board and the touchdown are found");
        const auto &mid = learned->frames[60 - 21].pose;
        check(close_to(mid.root.position[0], 6.0f, 1e-3f) && close_to(mid.board[0].position[0], 6.0f, 1e-3f) && mid.board[0].position[1] > 0.3f,
              "the clip's board follows the deck joint through the air");
        check(close_to(mid.board[2].position[1] - mid.board[0].position[1], 0.05f, 1e-3f), "and the board's own rig keeps its place on it");
        bool on_clock = true;
        for (std::size_t i = 0; i < learned->frames.size(); ++i) on_clock &= learned->frames[i].recorded == i * 16;
        check(on_clock, "each frame keeps its time on the recording clock, for the camera and board samples");
    }
    // The demonstration loops. Frames from before its restart, or after the next one, are not in the clip.
    auto restarted = shown;
    for (std::size_t i = 0; i < 30; ++i) restarted[i].joints[1].position[0] += 10;
    for (std::size_t i = 110; i < restarted.size(); ++i) restarted[i].joints[1].position[0] -= 10;
    const auto trimmed = clip_from_capture(3, restarted, joints, board, {}, why);
    check(trimmed && trimmed->frames.size() == 110 - 30, "the clip starts after the demonstration starts again, and ends before the next start");
    if (trimmed) {
        bool smooth = true;
        for (std::size_t i = 1; i < trimmed->frames.size(); ++i)
            smooth &= std::abs(trimmed->frames[i].pose.root.position[0] - trimmed->frames[i - 1].pose.root.position[0]) < teleport_metres;
        check(smooth, "so the clip has no jump in it");
    }
    auto broken = shown;
    for (std::size_t i = 70; i < broken.size(); ++i) broken[i].joints[1].position[0] -= 10;
    check(!clip_from_capture(3, broken, joints, board, {}, why) && !why.empty(), "a restart in the air spoils the jump");
    std::vector<RigFrame> rolling(shown.begin(), shown.begin() + 45);
    check(!clip_from_capture(3, rolling, joints, board, {}, why) && !why.empty(), "a recording with no jump says so");
    if (!failures) std::cout << "style takes tests passed\n";
    return failures ? 1 : 0;
}
