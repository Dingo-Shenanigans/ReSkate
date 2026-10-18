#pragma once
#include "overlay.h"
namespace dingosdk::overlay {
// Internal source interface, separate from the exported ControllerInput ABI.
// Uses ReSkate's existing reader, source selection, readiness and menu gates.
struct GameplayController {
    bool ready{}; // Native foreground/menu gate passed, including keyboard-only input.
    ControllerInput input;
    GamepadValues pad;
};
GameplayController read_gameplay_controller(bool allow_menu = false);
}
