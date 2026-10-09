#pragma once

// Skate 3's Hall Of Meat bone sounds (HOM_Set_1..5 banks, decoded to HallOfMeat/sounds/*.wav),
// played through XAudio2 when a body part reaches a new damage level.
namespace dingosdk::hall_of_meat {
// strength 0..1: how far up its damage levels the part went; broken: it reached the top one.
void play_bone_sound(float strength, bool broken) noexcept;
} // namespace dingosdk::hall_of_meat
