#include "Server/pose_check.h"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace dingosdk::server;
using dingosdk::multiplayer::Pose;
using dingosdk::multiplayer::Transform;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
Pose stock() {
    Pose pose;
    pose.skater.resize(400);
    for (auto &bone : pose.skater) bone.position = {0.f, 0.2f, 0.f};
    pose.skater[1].position = {500.f, 20.f, -300.f};
    pose.skater[3].position = {4.f, 1.f, 0.f};
    pose.board.resize(8);
    pose.board[0].position = {502.f, 20.f, -300.f};
    pose.board[2].position = {502.5f, 20.f, -300.f};
    return pose;
}
}

int main() {
    {
        const auto excess = pose_excess(stock());
        check(excess.reach <= suspicious_reach && excess.scale <= suspicious_scale && excess.board_scale <= suspicious_board_scale,
              "A stock skater was suspicious");
    }
    {
        auto pose = stock();
        pose.skater[40].scale = {0.0001f, 0.0001f, 0.0001f};
        pose.root.scale = {1.2f, 1.2f, 1.2f};
        const auto excess = pose_excess(pose);
        check(excess.scale <= suspicious_scale, "A hidden bone or a tall skater was taken for a resized one");
    }
    {
        auto pose = stock();
        pose.skater[60].scale = {6.f, 6.f, 6.f};
        check(pose_excess(pose).scale > suspicious_scale, "A 6x limb was not suspicious");
    }
    {
        auto pose = stock();
        pose.skater[60].position = {0.f, 8.f, 0.f};
        check(pose_excess(pose).reach > suspicious_reach, "A limb stretched 8 m was not suspicious");
    }
    {
        auto pose = stock();
        pose.board[5].scale = {3.f, 3.f, 3.f};
        check(pose_excess(pose).board_scale > suspicious_board_scale, "A 3x board was not suspicious");
    }
    return 0;
}
