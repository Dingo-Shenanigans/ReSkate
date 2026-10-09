#pragma once

// Skate 3's Hall Of Meat bone sounds (HOM_Set_1..5 banks, decoded to HallOfMeat/sounds/*.wav),
// played through XAudio2 when a body part reaches a new damage level.
namespace dingosdk::skate3_hom {
// strength 0..1: how far up its damage levels the part went; broken: it reached the top one.
void play_bone_sound(float strength, bool broken) noexcept;
// Skate 3's broken-bone slow-motion whoosh (hom_slo_mo bank, HallOfMeat/sounds/hom_slo_mo_*.wav).
void play_slow_motion_sound() noexcept;
} // namespace dingosdk::skate3_hom
