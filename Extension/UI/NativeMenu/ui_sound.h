#pragma once
#include <cstdint>

// The game's own menu sounds, for the pages ReSkate draws in its pause menu. The game plays
// them through its UI audio manager, which listens for one event
// (Audio/UI/Manager/Events/DGO_UI_PlayOneShot_Event) carrying which sound
// (Audio/_Systems/EnumTypeInfoAssets/DGO_Audio_UI_OneShot_Enum). This raises that event the
// way the game's own code raises events, so the sounds are the game's, at the player's own
// volume settings.
namespace dingosdk::multiplayer {
// Values of the game's enum.
enum class UiSound : std::uint32_t { cancel = 0, navigate = 3, toggle = 5, select = 7, tab = 56 };
// Any thread: the sound is played at the next client tick (the newest asked for wins).
void queue_ui_sound(std::uint32_t sound) noexcept;
// Client thread, each tick while a world is up.
void tick_ui_sounds(std::uintptr_t base, bool loading) noexcept;
} // namespace dingosdk::multiplayer
