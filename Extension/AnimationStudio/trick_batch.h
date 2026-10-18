#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Record all tricks: the skater goes to a practice stage (trick_stage.h) and does every ground flip trick
// with the game's own stick gestures (trick_gestures.h) on the scripted pad, once with a slow, a middle
// and a quick flick (a low, mid and high pop). Each landed take is kept as the trick and pop it was,
// with the board as the game drew it, and the trick list shows them. Solo play only. After PR #230's
// "Record all my tricks" (Norms).
namespace dingosdk::anim_studio::batch {
// False with `why` on a multiplayer server or while one runs. Any thread.
bool start(std::string& why);
// Esc or Back: stops after the take under way. Any thread.
void stop() noexcept;
[[nodiscard]] bool active() noexcept;
struct Progress {
    bool active{}, stopping{};
    std::size_t place{}, total{}, kept{};
    std::string trick, status;
};
[[nodiscard]] Progress progress();
// What the last run did, once, for the studio to tell when it opens again ("" none).
[[nodiscard]] std::string take_summary();
// Client thread, after the native client tick, with the local skater's animation component (0 none).
void tick(std::uintptr_t base, std::uintptr_t client, std::uintptr_t component, bool ready) noexcept;
} // namespace dingosdk::anim_studio::batch
