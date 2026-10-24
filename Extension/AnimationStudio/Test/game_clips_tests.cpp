// The game's own clips read out of its data (game_clips.h), against the installed game.
//   dingosdk_game_clips_tests <Skate folder> [<clip file made by make_trick_library.py> <its asset name>]
// With a clip file, the clip read here must match it: the same frames, joints within 1 mm and 0.2
// degrees, the same board and the same takeoff and landing.
#include "Extension/AnimationStudio/game_clips.h"
#include <Windows.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace clips = dingosdk::anim_studio::game_clips;
namespace anim = dingosdk::anim_studio;
namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::cout << "FAILED: " << what << "\n";
}
float angle(const anim::Quat& a, const anim::Quat& b) {
    const float dot = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * std::acos(std::min(dot, 1.0f)) * 57.29578f;
}
float apart(const anim::Vec3& a, const anim::Vec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: dingosdk_game_clips_tests <Skate folder> [<clip file> <asset name>]\n";
        return 2;
    }
    try {
        const auto started = GetTickCount64();
        const clips::Library library(argv[1]);
        std::cout << "read the game's animations in " << GetTickCount64() - started << " ms\n";
        const auto tricks = library.names("c_proto_onb_trick_o_", 5000);
        std::cout << tricks.size() << " trick animations\n";
        if (argc == 3 && std::string_view(argv[2]) == "--names")
            for (std::uint16_t j = 0; j < anim::skeleton_joints; ++j) std::cout << j << " " << library.joint_name(j) << "\n";
        check(tricks.size() > 100, "the trick animations are listed");
        const auto loaded_at = GetTickCount64();
        const auto clip = library.load("Animation/Dingo/c_proto_onb_trick_o_kickflip_high");
        std::cout << "kickflip high: " << clip.times.size() << " frames, " << clip.duration() << " s, takeoff " << clip.takeoff << " landing "
                  << clip.landing << ", in " << GetTickCount64() - loaded_at << " ms\n";
        check(clip.joints == anim::skeleton_joints && clip.times.size() == 122 && clip.board.size() == clip.times.size(), "a clip of 121 frames at 60 a second");
        check(clip.takeoff > 0 && clip.landing > clip.takeoff, "its takeoff and landing");
        for (const auto& j : clip.frames) check(anim::finite(j), "every joint is a number");
        // Its stance, and the mirror: the other stance, and twice is the clip again.
        const auto stance = library.goofy(clip);
        std::cout << "kickflip high stands " << (stance ? (*stance ? "goofy" : "regular") : "unknown") << "\n";
        check(stance.has_value(), "a game trick's stance is told from its feet and trucks");
        const auto mirrored = library.mirror(clip);
        const auto mirrored_stance = library.goofy(mirrored);
        check(stance && mirrored_stance && *mirrored_stance != *stance, "mirrored, it stands the other way");
        {
            // The board's nose stays ahead: its length (local Z) points the same way at the first frame.
            const auto z0 = anim::rotate(clip.board[0].rotation, {0, 0, 1}), z1 = anim::rotate(mirrored.board[0].rotation, {0, 0, 1});
            const float same_way = z0[0] * z1[0] + z0[1] * z1[1] + z0[2] * z1[2];
            std::cout << "mirrored board's length against the original's: " << same_way << "\n";
            check(same_way > 0.95f, "mirrored, the board keeps its nose ahead");
        }
        const auto back = library.mirror(mirrored);
        float worst_turn = 0, worst_place = 0;
        std::size_t worst_at = 0;
        for (std::size_t i = 0; i < clip.frames.size(); ++i) {
            if (i % anim::skeleton_joints < 2) continue;
            worst_turn = std::max(worst_turn, angle(clip.frames[i].rotation, back.frames[i].rotation));
            if (const float d = apart(clip.frames[i].position, back.frames[i].position); d > worst_place) {
                worst_place = d;
                worst_at = i;
            }
        }
        std::cout << "worst joint " << worst_at % anim::skeleton_joints << " (" << library.joint_name(static_cast<std::uint16_t>(worst_at % anim::skeleton_joints))
                  << ") frame " << worst_at / anim::skeleton_joints << "\n";
        float twice_board = 0;
        for (std::size_t f = 0; f < clip.board.size(); ++f) twice_board = std::max(twice_board, apart(clip.board[f].position, back.board[f].position));
        std::cout << "mirrored twice: worst turn " << worst_turn << " degrees, worst move " << worst_place * 1000 << " mm, board " << twice_board * 1000
                  << " mm\n";
        // (Within 2 mm: the hands' attach joints, where the game parks a prop, carry float rounding on their long reach.)
        check(worst_turn < 0.1f && worst_place < 0.002f && twice_board < 0.0005f, "mirrored twice, it is the clip again");
        bool refused = false;
        try {
            (void)library.load("animation/dingo/seq_c_proto_onb_trick_o_kickflip_low");
        } catch (const std::exception& e) {
            refused = true;
            std::cout << "not a clip: " << e.what() << "\n";
        }
        check(refused, "an asset that is not a clip is refused");
        if (argc >= 4) {
            std::ifstream file(argv[2], std::ios::binary);
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
            const auto made = anim::clip_from_bytes(bytes);
            const auto ours = library.load(argv[3]);
            check(made.times.size() == ours.times.size(), "as many frames as the library's: " + std::to_string(made.times.size()) + " and " +
                                                               std::to_string(ours.times.size()));
            float worst_angle = 0, worst_move = 0, worst_board = 0;
            std::size_t worst_joint = 0;
            for (std::size_t f = 0; f < std::min(made.times.size(), ours.times.size()); ++f) {
                for (std::size_t j = 0; j < anim::skeleton_joints; ++j) {
                    const auto &a = made.frames[f * made.joints + j], &b = ours.frames[f * ours.joints + j];
                    const float da = angle(a.rotation, b.rotation), dm = apart(a.position, b.position);
                    if (da > worst_angle) worst_angle = da, worst_joint = j;
                    worst_move = std::max(worst_move, dm);
                }
                if (f < made.board.size()) worst_board = std::max(worst_board, apart(made.board[f].position, ours.board[f].position));
            }
            std::cout << "against the library: worst turn " << worst_angle << " degrees (joint " << worst_joint << "), worst move " << worst_move * 1000
                      << " mm, board " << worst_board * 1000 << " mm; takeoff " << made.takeoff << " / " << ours.takeoff << ", landing " << made.landing
                      << " / " << ours.landing << "\n";
            check(worst_angle < 0.2f && worst_move < 0.001f && worst_board < 0.001f, "the same joints and board as the library's clip");
            check(std::abs(made.takeoff - ours.takeoff) < 0.001f && std::abs(made.landing - ours.landing) < 0.001f, "the same takeoff and landing");
        }
    } catch (const std::exception& e) {
        std::cout << "FAILED: " << e.what() << "\n";
        return 1;
    }
    if (failures) return 1;
    std::cout << "game clips tests passed\n";
    return 0;
}
