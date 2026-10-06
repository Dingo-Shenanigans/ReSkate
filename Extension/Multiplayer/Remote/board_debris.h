#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>

// Extra, independent skateboard entities for effects (board-break pieces). They are made the way
// the remote player's board is: a second board blueprint, physics off, appearance copied from the
// local board, moved by placing the entity. Client update thread only.
namespace dingosdk::multiplayer {
// What the render hook did with a piece's pose (diagnostic).
struct PieceStatus {
    std::uint32_t calls{}, written{}, no_layout{}, wrong_buffer{}, wrong_count{}, failed{};
    std::uint32_t count{};
    bool same_buffer{};
    std::array<float, 3> deck{}; // the deck bone's position read back after the last write
};
struct BoardDebris {
    std::uintptr_t entity{}, parent{}, context{}, local_parent{};
    explicit operator bool() const noexcept { return entity != 0; }
};
// Creates one piece at `at` (scale applies). On failure `why` says what, and nothing is left behind.
bool debris_create(std::uintptr_t base, std::uintptr_t client, const Transform &at, BoardDebris &out,
                   std::string &why) noexcept;
// Moves a piece. False (and `why`) if the entity is no longer what was created; the piece is then gone.
bool debris_place(std::uintptr_t base, const BoardDebris &piece, const Transform &at, std::string &why) noexcept;
// The bone pose a piece's skeleton shows (pose indices 0..; only the deck, truck and wheel bones are
// written). Keyed by the piece's animation holder; cleared by debris_destroy.
void debris_set_pose(std::uintptr_t holder, std::span<const Transform> bones);
PieceStatus debris_pose_status(std::uintptr_t holder);
void debris_clear_pose(std::uintptr_t holder) noexcept;
// The piece's animation holder, or 0.
std::uintptr_t debris_holder(const BoardDebris &piece) noexcept;
// Destroys a piece if it is still the entity that was created, and clears it.
void debris_destroy(std::uintptr_t base, BoardDebris &piece) noexcept;
} // namespace dingosdk::multiplayer
