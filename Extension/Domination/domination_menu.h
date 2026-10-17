#pragma once

// Domination's tab on the Multiplayer page (Extension/CustomModes/custom_modes.h). The one place
// the two meet: Domination works without the menu (console), and the menu without Domination.
// Built only where Custom Multiplayer is in the tree (DINGOSDK_CUSTOM_MODES, cmake/Runtime.cmake);
// elsewhere it does nothing.
namespace dingosdk::domination {
// Startup, after domination::start: lists Domination in the menu, and has a player who joins a
// Domination game join its round.
void register_custom_mode();
}
