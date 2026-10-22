#pragma once
#include "bmx_input.h"
#include "bmx_model.h"

namespace dingosdk::bmx {
// Only these actions currently have a physical model. Manuals and barspin/tailwhip requests
// remain presentation/control diagnostics until their balance, animation and catch rules exist.
inline Command riding_command(const Actions& actions) noexcept {
    Command result;
    result.input_enabled = actions.active;
    if (!actions.active) return result;
    result.steer = actions.steer;
    result.pedal = actions.pedal;
    result.brake = actions.brake;
    result.hop = actions.hop_pressed;
    result.hop_strength = actions.hop_strength;
    return result;
}
} // namespace dingosdk::bmx
