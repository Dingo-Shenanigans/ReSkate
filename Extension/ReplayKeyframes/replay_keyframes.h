#pragma once
#include <cstdint>
#include <string>

// Replay keyframes: a smoothness slider for the replay editor's camera. The game plays framing keys
// (the free camera's among them) as cubic Hermite segments whose tangents are only as smooth as the
// keys allow: it brakes where a channel turns or slows sharply, and the acceleration jumps at every
// key. Smoothness σ (replay_curve.h) gives every segment other tangents, up to those of the natural
// cubic spline through all keys at σ = 1; every key is still met exactly.
//
// The game builds the segments whenever a key changes (Engine/Game/Build/<build>/replay_keyframes.h),
// and each is rewritten as it is built, so that the game's own player plays σ's tangents. A change of
// setting builds the segments of the session the camera plays again (the game's own rebuild, on the
// game thread). Only the free camera's Behaviour (the evaluator a key's camera mode plays through) is
// smoothed; `keysmooth status` counts segments by Behaviour and Subject.
// The editor's preview of framing keys (the values it shows at the playhead) is the game's, and at
// σ > 0 it plays smoothed keys without its braking.
//
// Smoothing is off until the player turns it on (`keysmooth on`, or Keyframe smoothing in the menus):
// then σ is the amount (1 unless changed). The settings are kept between game runs (replay_settings.h).
namespace dingosdk::replay_keyframes {
// Checks this build's functions, loads the settings and hooks the framing track; once. False when it can't.
bool start(std::uintptr_t image_base) noexcept;
bool available() noexcept;
// Any thread.
bool on() noexcept;
float smoothness() noexcept; // the amount when on
// Game thread: the new setting, saved, with the framing keys of the session in play rebuilt; what was done.
std::string set_on(bool value);
std::string set_smoothness(float value);
// The settings, and how many framing segments were built since the last status.
std::string status();
} // namespace dingosdk::replay_keyframes
