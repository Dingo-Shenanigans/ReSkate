#pragma once

namespace dingosdk::overlay::detail {
// Slots 0..3 belong to ImGui, card/map pictures and mod icons. Allocate each
// renderer after the previous range so studio art cannot overwrite plugin SRVs.
inline constexpr unsigned plugin_image_first_slot = 4, plugin_image_slots = 16;
inline constexpr unsigned screen_look_slot = plugin_image_first_slot + plugin_image_slots, screen_look_slots = 3;
inline constexpr unsigned studio_pack_slot = screen_look_slot + screen_look_slots;
inline constexpr unsigned studio_frame_slot = studio_pack_slot + 1;
inline constexpr unsigned studio_wall_slot = studio_frame_slot + 1;
inline constexpr unsigned studio_sheet_slot = studio_wall_slot + 1;
inline constexpr unsigned studio_cover_slot = studio_sheet_slot + 1;
inline constexpr unsigned store_icon_first_slot = studio_cover_slot + 1, store_icon_slots = 64;
inline constexpr unsigned overlay_descriptor_count = store_icon_first_slot + store_icon_slots;
static_assert(studio_pack_slot >= plugin_image_first_slot + plugin_image_slots);
static_assert(studio_pack_slot >= screen_look_slot + screen_look_slots);
static_assert(store_icon_first_slot > studio_cover_slot);
static_assert(overlay_descriptor_count == 92);
} // namespace dingosdk::overlay::detail
