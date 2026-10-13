#include "Server/board_check.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using dingosdk::server::BoardCheck;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
constexpr std::uint64_t step_us = 50000; // 20 poses a second
}

int main() {
    using Position = BoardCheck::Position;
    {
        // Ridden: the board is under its skater whoever is near.
        BoardCheck board;
        for (unsigned i = 0; i < 400; ++i) {
            const float x = i * 0.3f;
            const Position skater{x, 0, 0}, deck{x, -0.1f, 0};
            const std::vector<BoardCheck::Other> others{{2, {x + 1, 0, 0}}};
            check(!board.sample(i * step_us, skater, &deck, others) && !board.flagged(), "A ridden board was flagged");
        }
    }
    {
        // Left lying: another player skates past it, and one stands on it for a minute.
        BoardCheck board;
        const Position skater{0, 0, 0}, deck{20, 0, 0};
        for (unsigned i = 0; i < 400; ++i) {
            const std::vector<BoardCheck::Other> others{{2, {i * 0.3f - 20, 0, 0}}};
            check(!board.sample(i * step_us, skater, &deck, others), "A board skated past was flagged");
        }
        for (unsigned i = 400; i < 1600; ++i) {
            const std::vector<BoardCheck::Other> others{{2, {20.3f, 0, 0}}};
            check(!board.sample(i * step_us, skater, &deck, others), "A board stood beside was flagged");
        }
        check(!board.flagged(), "A board left lying was flagged");
    }
    {
        // Kept on another player as they skate: flagged once they have gone 8 m with it over 3 s,
        // and not before.
        BoardCheck board;
        const Position skater{0, 0, 0};
        unsigned flagged_at{};
        for (unsigned i = 0; i < 200 && !flagged_at; ++i) {
            const float x = 10 + i * 0.15f; // 3 m/s
            const Position deck{x, 0.5f, 0};
            const std::vector<BoardCheck::Other> others{{2, {x, 0, 0}}, {3, {-30, 0, 0}}};
            if (board.sample(i * step_us, skater, &deck, others)) flagged_at = i;
        }
        check(flagged_at && board.flagged() && board.victim() == 2, "A board kept on another player was not flagged");
        check(flagged_at * step_us >= BoardCheck::held_us && flagged_at * 0.15f >= BoardCheck::travel - 0.2f,
              "A board kept on another player was flagged too soon");
        // Back under its own skater: still flagged for a while, then cleared.
        const Position home{0, -0.1f, 0};
        const std::vector<BoardCheck::Other> far{{2, {40, 0, 0}}};
        auto now = flagged_at * step_us;
        bool cleared{};
        for (unsigned i = 0; i < 20 * 29 && !cleared; ++i) cleared = board.sample(now += step_us, skater, &home, far);
        check(!cleared && board.flagged(), "The flag cleared too soon");
        for (unsigned i = 0; i < 20 * 3 && !cleared; ++i) cleared = board.sample(now += step_us, skater, &home, far);
        check(cleared && !board.flagged(), "The flag did not clear after the board behaved");
    }
    {
        // Circling a player who stays within a few metres counts as travel too; a dropped pose
        // or two does not start it over.
        BoardCheck board;
        const Position skater{0, 0, 0};
        bool flagged{};
        for (unsigned i = 0; i < 400 && !flagged; ++i) {
            if (i % 7 == 3) continue; // a lost pose
            const float angle = i * 0.05f;
            const Position them{20 + 2 * std::cos(angle), 0, 2 * std::sin(angle)}, deck{them[0], 1, them[2]};
            const std::vector<BoardCheck::Other> others{{5, them}};
            flagged = board.sample(i * step_us, skater, &deck, others);
        }
        check(flagged, "A board kept on a circling player was not flagged");
    }
    {
        // No board out at all.
        BoardCheck board;
        const Position skater{0, 0, 0};
        for (unsigned i = 0; i < 200; ++i) {
            const std::vector<BoardCheck::Other> others{{2, {10 + i * 0.15f, 0, 0}}};
            check(!board.sample(i * step_us, skater, nullptr, others), "A player with no board out was flagged");
        }
    }
    std::cout << "Board check: ridden, left lying, kept on a player, circling, clearing and no board passed.\n";
    return 0;
}
