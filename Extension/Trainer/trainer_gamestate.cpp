#include "trainer_gamestate.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Extension/Skater/client_source_spawn.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>
#include <string>

namespace dingosdk::trainer {
namespace {
// Game build 20260929.
//
// A game state is an asset object in memory: its 16-byte asset id, then the object, with its
// name at + 0x18 and at + 0x28 the handle the game finds its value by (0x11dcd40): the low 12
// bits pick one of the skater's state pages, the next 11 the value's place in that page, and a
// bit map at page + 0x71c says whether the page holds it. The handles differ from level to
// level, so they are read from the objects each time; the objects are found by their asset id
// in the memory search (trainer_classes.cpp).
//
// The skater's pages hang off its AntDrivenComponent: + 0x68 is the animatable, which leads to
// the block of pages (known by its vtable) two ways, + 0xe8 -> + 0x8 and + 0x1d8 -> + 0x248;
// page n is at block + 0x60 + n * 8.
constexpr std::uintptr_t ant_driven_vtable = 0x63aaee8, states_block_vtable = 0x61a8bd8;
constexpr std::uintptr_t component_collection = 0x70, collection_count = 0x8, collection_first = 0x20, collection_step = 0x20;
constexpr std::uintptr_t driven_animatable = 0x68, block_pages = 0x60, page_map = 0x71c;
constexpr std::uintptr_t block_ways[2][2]{{0xe8, 0x8}, {0x1d8, 0x248}};
constexpr std::uintptr_t object_after_id = 0x10, object_name = 0x18, object_handle = 0x28;
constexpr std::uint32_t trick_count = 59;

template <class T> bool peek(std::uintptr_t address, T &value) noexcept {
    __try {
        std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
        return true;
    } __except (1) { return false; }
}
bool put(std::uintptr_t address, float value) noexcept {
    __try {
        *reinterpret_cast<volatile float *>(address) = value;
        return true;
    } __except (1) { return false; }
}
bool pointer(std::uintptr_t value) noexcept { return value > 0x10000 && value < 0x7fffffffffffull && !(value & 3); }

// The two states a flip trick's speed is driven by: the speed its clip plays at, which the game
// copies from the trick scripts' Float.Intent.TrickFlipSpeed once when the board is flicked
// (writing the copy in the air slows or speeds the flip; writing the scripts' number then does
// nothing, and nothing that sets the pop reads either), and the trick that is turning.
struct StateObject {
    std::uint8_t id[16];
    std::string_view name;
    std::atomic<std::uintptr_t> at{};
};
StateObject flip_speed_object{{0xb0, 0x7b, 0x6c, 0x08, 0x42, 0xe7, 0xef, 0xc7, 0x08, 0x6c, 0x7b, 0xb0, 0xe7, 0x42, 0xc7, 0xef},
                              "Animation/Dingo/Float.Anim.TrickFlipSpeed"};
// Float.Anim.TrickFlipSpeedClamped: what the six impossible clips (seq_c_proto_onb_trick_o_impossible_*) play at, where
// every other trick clip plays at TrickFlipSpeed (the game's clip data, 2026-10-11).
StateObject flip_clamped_object{{0x36, 0xac, 0x11, 0x7c, 0x47, 0xc8, 0x20, 0x44, 0x7c, 0x11, 0xac, 0x36, 0xc8, 0x47, 0x44, 0x20},
                                "Animation/Dingo/Float.Anim.TrickFlipSpeedClamped"};
StateObject flip_trick_object{{0x17, 0x54, 0x40, 0x3c, 0xa7, 0x06, 0x61, 0x3d, 0x3c, 0x40, 0x54, 0x17, 0x06, 0xa7, 0x3d, 0x61},
                              "Animation/Dingo/EnumGS.Anim.CurrentFlipTrick"};
// The underflip: CurrentFlipTrick stays the flipped trick through it (owner's round, 2026-10-18); these say when one is
// turning (Underflip speed acts while they do).
StateObject underflip_anim_object{{0xa7, 0xcb, 0xd5, 0xbb, 0x63, 0x03, 0x34, 0x4b, 0xbb, 0xd5, 0xcb, 0xa7, 0x03, 0x63, 0x4b, 0x34},
                                  "Animation/Dingo/Bool.Anim.Is.Underflip"};
StateObject underflip_state_object{{0x60, 0x0f, 0xe7, 0x0f, 0x33, 0x04, 0x06, 0xb2, 0x0f, 0xe7, 0x0f, 0x60, 0x04, 0x33, 0xb2, 0x06},
                                   "Animation/Dingo/Bool.State.Is.Underflip"};
StateObject underflip_window_object{{0xb9, 0xfe, 0x99, 0x99, 0xe7, 0x7e, 0x13, 0xbd, 0x99, 0x99, 0xfe, 0xb9, 0x7e, 0xe7, 0xbd, 0x13},
                                    "Animation/Dingo/Bool.Anim.IsInUnderflipWindow"};
// What the underflip plays at: all 48 underflip sequences (seq_c_proto_onb_trick_*_uflip) read it where every other
// trick's sequence reads TrickFlipSpeed (the game's clip data, 2026-10-19), so TrickFlipSpeed never reached them.
StateObject underflip_timescale_object{{0xba, 0xd6, 0x9c, 0xbf, 0x33, 0x37, 0x71, 0x75, 0xbf, 0x9c, 0xd6, 0xba, 0x37, 0x33, 0x75, 0x71},
                                       "Animation/Dingo/Float.Anim.UnderflipTimeScale"};
// The underflip's playback rate controllers (PlaybackRateControllerAsset): they play the underflip at TrickFlipSpeed
// (and the part into the air at an expression of it and UnderflipTimeScale), held between MinScale and MaxScale. The
// game's band is 1 to 1.2 and 0.6 to 1.2 (the game's data, 2026-10-19), so no slower speed ever reached an underflip.
// Underflip speed scales the band; the copies of each asset in memory are all kept.
struct RateBand {
    std::uint8_t id[16];
    std::string_view name;
    float stock_min, stock_max;
    std::atomic<std::uintptr_t> at[4]{};
};
constexpr std::uintptr_t rate_min_scale = 0x74, rate_max_scale = 0x78; // from the object (after its id)
RateBand underflip_bands[]{
    {{0xbf, 0x2e, 0xfa, 0xb7, 0x63, 0x8e, 0x74, 0xe3, 0xb7, 0xfa, 0x2e, 0xbf, 0x8e, 0x63, 0xe3, 0x74},
     "Animation/Dingo/cdbchooser_onboard_underflips_(pbrate)", 1.0f, 1.2f},
    {{0x35, 0x45, 0x95, 0xf4, 0x69, 0xe1, 0xe6, 0xc9, 0xf4, 0x95, 0x45, 0x35, 0xe1, 0x69, 0xc9, 0xe6},
     "Animation/Dingo/cdbchooser_onboard_underflips_toair(pbrate)", 0.6f, 1.2f},
};
std::atomic<float> band_factor{1}, band_high_factor{1};
StateObject pumping_object{{0xab, 0x5b, 0xd5, 0x21, 0x9f, 0xa4, 0xb0, 0xfb, 0x21, 0xd5, 0x5b, 0xab, 0xa4, 0x9f, 0xfb, 0xb0}, "Animation/Dingo/Bool.Intent.Pumping"};
StateObject pump_strength_object{{0x54, 0xc6, 0x59, 0x6a, 0xbb, 0xad, 0xfc, 0x05, 0x6a, 0x59, 0xc6, 0x54, 0xad, 0xbb, 0x05, 0xfc},
                                 "Animation/Dingo/Float.Intent.UserTriggeredPumpStrength"};
StateObject pump_assist_object{{0xcb, 0xdf, 0x5b, 0x53, 0x14, 0x53, 0xc8, 0x29, 0x53, 0x5b, 0xdf, 0xcb, 0x53, 0x14, 0x29, 0xc8},
                               "Animation/Dingo/Float.Anim.PumpingAssistAmount"};
// Off the board: how fast a flip in the air (jump, then RB and the left stick) turns, and how quickly it gets there.
StateObject body_flip_speed_object{{0xbc, 0x5b, 0x7b, 0x1d, 0x12, 0xd8, 0xa1, 0x2a, 0x1d, 0x7b, 0x5b, 0xbc, 0xd8, 0x12, 0x2a, 0xa1},
                                   "Animation/Dingo/Float.Intent.BodyFlipSpeedMod"};
StateObject body_flip_rate_object{{0x74, 0x34, 0x08, 0x3f, 0x5e, 0x6f, 0xb4, 0xfa, 0x3f, 0x08, 0x34, 0x74, 0x6f, 0x5e, 0xfa, 0xb4},
                                  "Animation/Dingo/Float.Intent.BodyFlipAccelerationRate"};
// Which manual the player is asking for (0: none).
StateObject manual_object{{0x77, 0x52, 0xaa, 0x61, 0x87, 0xb9, 0xfb, 0xaa, 0x61, 0xaa, 0x52, 0x77, 0xb9, 0x87, 0xaa, 0xfb}, "Animation/Dingo/Enum.Intent.Manual"};
StateObject grind_nudge_object{{0x68, 0xa1, 0x85, 0xe5, 0x51, 0x0c, 0x22, 0x2f, 0xe5, 0x85, 0xa1, 0x68, 0x0c, 0x51, 0x2f, 0x22},
                               "Animation/Dingo/Float.Intent.GrindStabilityNudge"};
StateObject grind_control_objects[]{
    {{0x7b, 0x19, 0x3e, 0x26, 0xbb, 0xd0, 0xda, 0x97, 0x26, 0x3e, 0x19, 0x7b, 0xd0, 0xbb, 0x97, 0xda}, "Animation/Dingo/Float.Intent.GrindControlLR"},
    {{0xa6, 0x4e, 0xb2, 0xa5, 0x2b, 0x0d, 0x8a, 0x6d, 0xa5, 0xb2, 0x4e, 0xa6, 0x0d, 0x2b, 0x6d, 0x8a}, "Animation/Dingo/Float.Intent.GrindControlUD"},
    {{0x63, 0x68, 0x2d, 0xaa, 0x47, 0xdf, 0x36, 0x3a, 0xaa, 0x2d, 0x68, 0x63, 0xdf, 0x47, 0x3a, 0x36}, "Animation/Dingo/Float.Intent.GrindControlHeading"},
    {{0x19, 0xa9, 0xd6, 0xa6, 0xa1, 0xfc, 0x03, 0xa1, 0xa6, 0xd6, 0xa9, 0x19, 0xfc, 0xa1, 0xa1, 0x03}, "Animation/Dingo/Float.Intent.GrindControlPitch"},
    {{0xd3, 0x1e, 0x65, 0x0c, 0xa2, 0xb6, 0xac, 0xe0, 0x0c, 0x65, 0x1e, 0xd3, 0xb6, 0xa2, 0xe0, 0xac}, "Animation/Dingo/Float.Intent.GrindHeadingControlMultiplier"},
    {{0xc9, 0xc1, 0xeb, 0xed, 0xe1, 0xd0, 0xfa, 0xd3, 0xed, 0xeb, 0xc1, 0xc9, 0xd0, 0xe1, 0xd3, 0xfa}, "Animation/Dingo/Float.Intent.GrindPitchControlMultiplier"},
    {{0xc4, 0x16, 0x23, 0xba, 0x53, 0x14, 0x47, 0x92, 0xba, 0x23, 0x16, 0xc4, 0x14, 0x53, 0x92, 0x47}, "Animation/Dingo/Float.Intent.GrindBaseHeading"},
    {{0xff, 0x4b, 0xa6, 0xad, 0xeb, 0xf7, 0x7b, 0x65, 0xad, 0xa6, 0x4b, 0xff, 0xf7, 0xeb, 0x65, 0x7b}, "Animation/Dingo/Float.Intent.GrindBasePitch"},
    {{0x85, 0xc4, 0x26, 0x2b, 0xd5, 0x4e, 0x37, 0x04, 0x2b, 0x26, 0xc4, 0x85, 0x4e, 0xd5, 0x04, 0x37}, "Animation/Dingo/Enum.GS.Anim.CurrentGrindType"},
    {{0x7f, 0xf7, 0xde, 0xa2, 0x14, 0xd2, 0x6b, 0x3b, 0xa2, 0xde, 0xf7, 0x7f, 0xd2, 0x14, 0x3b, 0x6b}, "Animation/Dingo/Enum.GS.Anim.CurrentSkateGrindType"},
    {{0xef, 0xb2, 0xb6, 0x2b, 0x9a, 0xee, 0xc0, 0x5d, 0x2b, 0xb6, 0xb2, 0xef, 0xee, 0x9a, 0x5d, 0xc0}, "Animation/Dingo/Bool.Intent.GrindControlHasActiveInput"},
};
StateObject *const objects[]{&manual_object, &flip_speed_object, &flip_trick_object, &pumping_object, &pump_strength_object, &pump_assist_object,
                             &body_flip_speed_object, &body_flip_rate_object, &flip_clamped_object, &grind_nudge_object,
                             &underflip_anim_object, &underflip_state_object, &underflip_window_object, &underflip_timescale_object};

StateObject grind_objects[]{
    {{0xda, 0x09, 0x5b, 0x51, 0xf4, 0xff, 0x36, 0x3e, 0x51, 0x5b, 0x09, 0xda, 0xff, 0xf4, 0x3e, 0x36}, "Animation/Dingo/Bool.Intent.NoPopGrindExit"},
    {{0xf2, 0xe4, 0x06, 0x75, 0x27, 0xf5, 0x50, 0x71, 0x75, 0x06, 0xe4, 0xf2, 0xf5, 0x27, 0x71, 0x50}, "Animation/Dingo/Bool.Intent.EnableGrindExitToGrind"},
    {{0x84, 0xbc, 0x0c, 0x36, 0x3f, 0x38, 0x7a, 0xe8, 0x36, 0x0c, 0xbc, 0x84, 0x38, 0x3f, 0xe8, 0x7a}, "Animation/Dingo/Bool.Intent.EnableGrindExitDoubleGrinds"},
    {{0xa6, 0x67, 0x3c, 0x9d, 0x49, 0x99, 0x66, 0x4c, 0x9d, 0x3c, 0x67, 0xa6, 0x99, 0x49, 0x4c, 0x66}, "Animation/Dingo/Bool.Intent.EnableStrongerGrindExits"},
    {{0xab, 0xcf, 0xb0, 0x33, 0x0f, 0xd3, 0xf3, 0x74, 0x33, 0xb0, 0xcf, 0xab, 0xd3, 0x0f, 0x74, 0xf3}, "Animation/Dingo/Bool.Intent.EnableCameraRelativeGrindExits"},
    {{0x66, 0x51, 0xab, 0xf7, 0x0d, 0xc7, 0x38, 0x76, 0xf7, 0xab, 0x51, 0x66, 0xc7, 0x0d, 0x76, 0x38}, "Animation/Dingo/Bool.Intent.EnableGrindDirectControl"},
    {{0xfe, 0x2a, 0x2c, 0xdf, 0x68, 0x36, 0x38, 0x34, 0xdf, 0x2c, 0x2a, 0xfe, 0x36, 0x68, 0x34, 0x38}, "Animation/Dingo/Bool.Intent.EnableGrindLinearDrive"},
    {{0x11, 0xe9, 0x62, 0x1b, 0x14, 0x3c, 0x1e, 0xca, 0x1b, 0x62, 0xe9, 0x11, 0x3c, 0x14, 0xca, 0x1e}, "Animation/Dingo/Bool.Intent.EnableEATGrindControl"},
    {{0x41, 0x6b, 0x36, 0x87, 0xd4, 0x0d, 0x92, 0x1b, 0x87, 0x36, 0x6b, 0x41, 0x0d, 0xd4, 0x1b, 0x92}, "Animation/Dingo/Bool.Intent.GrindToBoardslide"},
    {{0x5a, 0xf8, 0x53, 0xe2, 0x2f, 0x5b, 0x89, 0x02, 0xe2, 0x53, 0xf8, 0x5a, 0x5b, 0x2f, 0x02, 0x89}, "Animation/Dingo/Bool.Intent.DisableLegacyCopingGrinds"},
    {{0x8c, 0x0f, 0xfd, 0x43, 0x45, 0x81, 0xa8, 0xc4, 0x43, 0xfd, 0x0f, 0x8c, 0x81, 0x45, 0xc4, 0xa8}, "Animation/Dingo/Bool.Intent.AllowAirToGrind"},
};
// Transition skating: what the physics says about the lip, in the order of TransitionState (four flags, a number, an
// enumeration, three vectors).
StateObject transition_objects[]{
    {{0x7d, 0xbc, 0xc5, 0x80, 0xd7, 0xd9, 0xb2, 0xed, 0x80, 0xc5, 0xbc, 0x7d, 0xd9, 0xd7, 0xed, 0xb2}, "Animation/Dingo/Bool.Physics.FoundLipTrick"},
    {{0xf7, 0x40, 0x6e, 0xeb, 0x30, 0x84, 0x8a, 0x2f, 0xeb, 0x6e, 0x40, 0xf7, 0x84, 0x30, 0x2f, 0x8a}, "Animation/Dingo/Bool.Physics.IsLipTricking"},
    {{0xf4, 0xe2, 0x75, 0xd3, 0x3a, 0xb6, 0x36, 0x20, 0xd3, 0x75, 0xe2, 0xf4, 0xb6, 0x3a, 0x20, 0x36}, "Animation/Dingo/Bool.Physics.IsHandplanting"},
    {{0xa0, 0x93, 0xef, 0x64, 0xcd, 0xf1, 0xd7, 0xfc, 0x64, 0xef, 0x93, 0xa0, 0xf1, 0xcd, 0xfc, 0xd7}, "Animation/Dingo/Bool.Physics.IsTrajectoryTargetingVert"},
    {{0x4d, 0x51, 0x11, 0xcd, 0x2f, 0xad, 0x80, 0x9c, 0xcd, 0x11, 0x51, 0x4d, 0xad, 0x2f, 0x9c, 0x80}, "Animation/Dingo/Float.Intent.TransitionInput"},
    {{0x00, 0xdf, 0x0d, 0x4f, 0xfa, 0x34, 0x89, 0x81, 0x4f, 0x0d, 0xdf, 0x00, 0x34, 0xfa, 0x81, 0x89}, "Animation/Dingo/EnumGS.Physics.LipDropInType"},
    {{0x91, 0x7e, 0x62, 0x00, 0xa7, 0xe0, 0x4d, 0x9c, 0x00, 0x62, 0x7e, 0x91, 0xe0, 0xa7, 0x9c, 0x4d}, "Animation/Dingo/Vec3.Physics.LipDirection"},
    {{0x09, 0xda, 0x55, 0x9c, 0xe0, 0x97, 0x4a, 0xa8, 0x9c, 0x55, 0xda, 0x09, 0x97, 0xe0, 0xa8, 0x4a}, "Animation/Dingo/Vec3.Physics.LipNearestPoint"},
    {{0x27, 0x56, 0x8a, 0xee, 0x40, 0x00, 0x61, 0xce, 0xee, 0x8a, 0x56, 0x27, 0x00, 0x40, 0xce, 0x61}, "Animation/Dingo/Vec3.Prediction.ReckoningGroundNormal"},
};
bool put_flag(std::uintptr_t address, bool value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t *>(address) = value ? 1 : 0;
        return true;
    } __except (1) { return false; }
}

// The object behind an asset id seen at `id_at`, when it is that state's (its name says so).
bool is_named(const std::uint8_t (&id)[16], std::string_view wanted, std::uintptr_t id_at) noexcept {
    __try {
        if (std::memcmp(reinterpret_cast<const void *>(id_at), id, sizeof(id)) != 0) return false;
        const auto name = *reinterpret_cast<const std::uintptr_t *>(id_at + object_after_id + object_name);
        if (name <= id_at || name > id_at + 0x400) return false;
        return _strnicmp(reinterpret_cast<const char *>(name), wanted.data(), wanted.size()) == 0 &&
               *reinterpret_cast<const char *>(name + wanted.size()) == 0;
    } __except (1) { return false; }
}
bool is_object(const StateObject &object, std::uintptr_t id_at) noexcept { return is_named(object.id, object.name, id_at); }
// Where the skater's value of that state is; 0 when it has none.
std::uintptr_t value_address(StateObject &object, std::uintptr_t block) noexcept {
    const auto at = object.at.load(std::memory_order_relaxed);
    if (!at) return 0;
    if (!is_object(object, at - object_after_id)) {
        object.at.store(0, std::memory_order_relaxed); // unloaded with its level
        return 0;
    }
    std::uint32_t handle{}, map{};
    std::uintptr_t page{};
    if (!peek(at + object_handle, handle) || !(handle >> 24)) return 0;
    const std::uint32_t index = handle & 0xfff, place = (handle >> 12) & 0x7ff;
    if (index >= 16 || !peek(block + block_pages + index * 8, page) || !pointer(page) || !peek(page + page_map + (place >> 5) * 4, map) ||
        !(map >> (place & 31) & 1))
        return 0;
    return page + place;
}
std::uintptr_t states_block(std::uintptr_t base, std::uintptr_t entity) noexcept {
    std::uintptr_t collection{}, owner{};
    std::uint8_t count{};
    if (!entity || !peek(entity + component_collection, collection) || !pointer(collection) || !peek(collection, owner) || owner != entity ||
        !peek(collection + collection_count, count) || count > 128)
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        std::uintptr_t component{}, type{}, animatable{};
        if (!peek(collection + collection_first + i * collection_step, component) || !pointer(component) || !peek(component, type) ||
            type != base + ant_driven_vtable)
            continue;
        if (!peek(component + driven_animatable, animatable) || !pointer(animatable)) return 0;
        for (const auto &way : block_ways) {
            std::uintptr_t holder{}, block{};
            if (peek(animatable + way[0], holder) && pointer(holder) && peek(holder + way[1], block) && pointer(block) && peek(block, type) &&
                type == base + states_block_vtable)
                return block;
        }
        return 0;
    }
    return 0;
}

// EnumGS.Anim.CurrentFlipTrick, as the game names its values.
constexpr std::string_view game_trick_names[trick_count]{
    "None", "Ollie", "Kickflip", "Heelflip", "PopShuvit", "VarialKickflip", "InwardHeelflip", "FSPopShuvit", "VarialHeelflip", "Hardflip",
    "360PopShuvit", "360Flip", "360InwardHeelflip", "FS360PopShuvit", "Laserflip", "360Hardflip", "Nollie", "N_Kickflip", "N_Heelflip",
    "N_PopShuvit", "N_Hardflip", "N_VarialHeelflip", "N_FSPopShuvit", "N_VarialKickflip", "N_InwardHeelflip", "N_FS360PopShuvit",
    "N_360Hardflip", "N_Laserflip", "N_360PopShuvit", "N_360Flip", "N_360InwardHeelflip", "OlliePop", "N_OlliePop", "Wallie", "FSWallie",
    "BSWallie", "Impossible", "FrontFoot_Impossible", "N_Impossible", "N_BackFoot_Impossible", "LateFlipTrick", "HeldFlipTrick", "BonedOllie",
    "KickflipUnderflip", "HeelflipUnderflip", "BSPopShuvitUnderflip", "FSPopShuvitUnderflip", "NollieKickflipUnderflip",
    "NollieHeelflipUnderflip", "NollieBSPopShuvitUnderflip", "NollieFSPopShuvitUnderflip", "HospitalFlip", "FlowerFlip", "NollieHospitalFlip",
    "NollieFlowerFlip", "ImpossibleUnderflip", "FrontFootImpossibleUnderflip", "NollieImpossibleUnderflip", "NollieBackFootImpossibleUnderflip"};
// Index into flip_tricks for each value, -1 where a trick has no speed of its own.
constexpr std::int8_t trick_slider[trick_count]{
    -1, 16, 0, 1, 2, 6, 11, 3, 7, 10,
    4, 8, 13, 5, 9, 12, 16, 0, 1,
    2, 10, 7, 3, 6, 11, 5,
    12, 9, 4, 8, 13, 16, 16, -1, -1,
    -1, 14, 15, 17, 18, 19, -1, 16,
    -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1};
// A bit per possible first word of an object's id (hashed to 4096): most words of memory start no object, and
// checking each against every object made this the slowest part of the search.
const std::array<std::uint64_t, 64> &state_id_filter() noexcept {
    static const auto bits = [] {
        std::array<std::uint64_t, 64> result{};
        const auto mark = [&](const StateObject &object) {
            std::uint32_t first;
            std::memcpy(&first, object.id, 4);
            const auto h = (first * 0x9e3779b1u) >> 20;
            result[h >> 6] |= std::uint64_t{1} << (h & 63);
        };
        for (const auto *object : objects) mark(*object);
        for (const auto &object : grind_objects) mark(object);
        for (const auto &object : grind_control_objects) mark(object);
        for (const auto &object : transition_objects) mark(object);
        for (const auto &band : underflip_bands) {
            std::uint32_t first;
            std::memcpy(&first, band.id, 4);
            const auto h = (first * 0x9e3779b1u) >> 20;
            result[h >> 6] |= std::uint64_t{1} << (h & 63);
        }
        return result;
    }();
    return bits;
}
}

void find_state_objects(std::uintptr_t start, std::size_t size) noexcept {
    __try {
        const auto *words = reinterpret_cast<const std::uint32_t *>(start);
        const auto look = [&](StateObject &object, std::size_t i) {
            std::uint32_t first;
            std::memcpy(&first, object.id, 4);
            if (words[i] != first) return;
            // An object can be in memory more than once, and only one copy has its handle filled in (seen on the grind
            // switches, 2026-10-11): a copy with a handle replaces one without.
            const auto known = object.at.load(std::memory_order_relaxed), here = start + i * 4 + object_after_id;
            std::uint32_t old_handle{}, handle{};
            if (known == here || !is_object(object, start + i * 4)) return;
            if (known && (!peek(known + object_handle, old_handle) || old_handle >> 24)) return;
            if (known && (!peek(here + object_handle, handle) || !(handle >> 24))) return;
            object.at.store(here, std::memory_order_relaxed);
        };
        const auto &filter = state_id_filter();
        for (std::size_t i = 0; i + 0x20 < size / 4; ++i) {
            if (const auto h = (words[i] * 0x9e3779b1u) >> 20; !((filter[h >> 6] >> (h & 63)) & 1)) continue;
            for (auto *object : objects) look(*object, i);
            for (auto &object : grind_objects) look(object, i);
            for (auto &object : grind_control_objects) look(object, i);
            for (auto &object : transition_objects) look(object, i);
            for (auto &band : underflip_bands) {
                std::uint32_t first;
                std::memcpy(&first, band.id, 4);
                if (words[i] != first || !is_named(band.id, band.name, start + i * 4)) continue;
                const auto here = start + i * 4 + object_after_id;
                // The band as the game made it, or as the trainer last set it: anything else is not this layout.
                float low{}, high{};
                const float f = band_factor.load(std::memory_order_relaxed), g = band_high_factor.load(std::memory_order_relaxed);
                if (!peek(here + rate_min_scale, low) || !peek(here + rate_max_scale, high) ||
                    !((low == band.stock_min && high == band.stock_max) || (low == band.stock_min * f && high == band.stock_max * g)))
                    continue;
                for (auto &slot : band.at) {
                    auto known = slot.load(std::memory_order_relaxed);
                    if (known == here) break;
                    if (!known && slot.compare_exchange_strong(known, here, std::memory_order_relaxed)) break;
                }
            }
        }
    } __except (1) {}
}
int scale_underflip_bands(float low_factor, float high_factor) noexcept {
    band_factor.store(low_factor, std::memory_order_relaxed);
    band_high_factor.store(high_factor, std::memory_order_relaxed);
    int found = 0;
    for (auto &band : underflip_bands)
        for (auto &slot : band.at) {
            const auto at = slot.load(std::memory_order_relaxed);
            if (!at) continue;
            float low{}, high{};
            if (!peek(at + rate_min_scale, low) || !peek(at + rate_max_scale, high)) {
                slot.store(0, std::memory_order_relaxed); // freed with its level
                continue;
            }
            const float want_low = band.stock_min * low_factor, want_high = band.stock_max * high_factor;
            if (low != want_low) (void)put(at + rate_min_scale, want_low);
            if (high != want_high) (void)put(at + rate_max_scale, want_high);
            ++found;
        }
    return found;
}
bool flip_states_found() noexcept {
    return flip_speed_object.at.load(std::memory_order_relaxed) && flip_trick_object.at.load(std::memory_order_relaxed);
}
FlipState read_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    if (!flip_states_found()) return {};
    const auto block = states_block(base, entity);
    if (!block) return {};
    FlipState state;
    const auto speed_at = value_address(flip_speed_object, block), trick_at = value_address(flip_trick_object, block);
    if (!speed_at || !trick_at || !peek(trick_at, state.trick) || !peek(speed_at, state.speed)) return {};
    // Anything else there means this is not the layout this was written for.
    if (state.trick >= trick_count || !std::isfinite(state.speed) || state.speed < 0 || state.speed > 1000) return {};
    state.speed_at = speed_at;
    state.trick_at = trick_at;
    if (const auto at = value_address(flip_clamped_object, block); at && peek(at, state.clamped) && std::isfinite(state.clamped)) state.clamped_at = at;
    const auto flag = [&](StateObject &object) -> std::int8_t {
        std::uint8_t value{};
        const auto at = value_address(object, block);
        return at && peek(at, value) ? static_cast<std::int8_t>(value != 0) : std::int8_t{-1};
    };
    state.underflip_anim = flag(underflip_anim_object);
    state.underflip_state = flag(underflip_state_object);
    state.underflip_window = flag(underflip_window_object);
    if (const auto at = value_address(underflip_timescale_object, block); at && peek(at, state.underflip_timescale) &&
                                                                           std::isfinite(state.underflip_timescale))
        state.underflip_timescale_at = at;
    return state;
}
PumpState read_pump_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    PumpState state;
    if (!pumping_object.at.load(std::memory_order_relaxed) && !pump_strength_object.at.load(std::memory_order_relaxed)) return state;
    const auto block = states_block(base, entity);
    if (!block) return state;
    std::uint8_t flag{};
    float number{};
    if (const auto at = value_address(pumping_object, block); at && peek(at, flag)) {
        state.found = true;
        state.pumping = flag != 0;
    }
    if (const auto at = value_address(pump_strength_object, block); at && peek(at, number) && std::isfinite(number)) state.strength = number;
    if (const auto at = value_address(pump_assist_object, block); at && peek(at, number) && std::isfinite(number)) state.assist = number;
    return state;
}
BodyFlipState read_body_flip_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    BodyFlipState state;
    if (!body_flip_speed_object.at.load(std::memory_order_relaxed) && !body_flip_rate_object.at.load(std::memory_order_relaxed)) return state;
    const auto block = states_block(base, entity);
    if (!block) return state;
    float number{};
    if (const auto at = value_address(body_flip_speed_object, block); at && peek(at, number) && std::isfinite(number)) {
        state.speed_at = at;
        state.speed = number;
    }
    if (const auto at = value_address(body_flip_rate_object, block); at && peek(at, number) && std::isfinite(number)) {
        state.rate_at = at;
        state.rate = number;
    }
    return state;
}
GrindControl read_grind_control(std::uintptr_t base, std::uintptr_t entity) noexcept {
    GrindControl control;
    const auto block = states_block(base, entity);
    if (!block) return control;
    float *const numbers[]{&control.lr, &control.ud, &control.heading, &control.pitch, &control.heading_multiplier, &control.pitch_multiplier,
                           &control.base_heading, &control.base_pitch};
    std::uint32_t *const words[]{&control.grind_type, &control.skate_grind_type, &control.active_input};
    for (std::size_t i = 0; i < std::size(grind_control_objects); ++i) {
        const auto at = value_address(grind_control_objects[i], block);
        if (!at) continue;
        if (i < std::size(numbers)) {
            float value{};
            if (!peek(at, value) || !std::isfinite(value)) continue;
            *numbers[i] = value;
        } else if (i + 1 < std::size(grind_control_objects)) {
            std::uint32_t value{};
            if (!peek(at, value)) continue;
            *words[i - std::size(numbers)] = value;
        } else {
            std::uint8_t value{};
            if (!peek(at, value)) continue;
            control.active_input = value;
        }
        control.found |= 1u << i;
    }
    return control;
}
bool read_grind_nudge(std::uintptr_t base, std::uintptr_t entity, float &nudge) noexcept {
    if (!grind_nudge_object.at.load(std::memory_order_relaxed)) return false;
    const auto block = states_block(base, entity);
    const auto at = block ? value_address(grind_nudge_object, block) : 0;
    float value{};
    if (!at || !peek(at, value) || !std::isfinite(value)) return false;
    nudge = value;
    return true;
}
std::int32_t read_manual_intent(std::uintptr_t base, std::uintptr_t entity) noexcept {
    if (!manual_object.at.load(std::memory_order_relaxed)) return -1;
    const auto block = states_block(base, entity);
    std::uint32_t value{};
    if (!block) return -1;
    const auto at = value_address(manual_object, block);
    return at && peek(at, value) && value < 64 ? static_cast<std::int32_t>(value) : -1;
}
struct ForcedSetting {
    std::atomic<int> mode{-1};
    std::atomic<std::uint32_t> asked{};
};
ForcedSetting forced_settings[grind_settings.size() + transition_settings.size() + foot_settings.size()]; // grind, transition, then on-foot ones
constexpr std::size_t foot_first = grind_settings.size() + transition_settings.size();
// Which of forced_settings the setting asset at `asset` is (its key is the string at + 0x60); -1: none of them.
int grind_setting_of(std::uintptr_t asset) noexcept {
    std::uintptr_t name{};
    char text[48]{};
    if (!peek(asset + 0x60, name) || !pointer(name & ~std::uintptr_t{3}) || !peek(name, text)) {
        // A short string at the end of a page: byte by byte.
        if (!name) return -1;
        std::memset(text, 0, sizeof(text));
        for (std::size_t i = 0; i + 1 < sizeof(text); ++i)
            if (!peek(name + i, text[i]) || !text[i]) break;
    }
    text[sizeof(text) - 1] = 0;
    for (std::size_t i = 0; i < grind_settings.size(); ++i)
        if (_stricmp(text, std::string(grind_settings[i].setting).c_str()) == 0) return static_cast<int>(i);
    for (std::size_t i = 0; i < transition_settings.size(); ++i)
        if (_stricmp(text, std::string(transition_settings[i].setting).c_str()) == 0) return static_cast<int>(grind_settings.size() + i);
    for (std::size_t i = 0; i < foot_settings.size(); ++i)
        if (_stricmp(text, std::string(foot_settings[i].setting).c_str()) == 0) return static_cast<int>(foot_first + i);
    return -1;
}

// The game's lookup of a state's value: (state object, context with the pages) -> where the value is, 0 when the
// context has none. All of it is 0x79 bytes; these are its first.
constexpr std::uintptr_t state_lookup = 0x11dcdd0;
constexpr unsigned char state_lookup_bytes[]{0x48, 0x83, 0xec, 0x28, 0x80, 0x79, 0x2b, 0x00, 0x4c, 0x8b, 0xd2, 0x4c, 0x8b, 0xc9};
using StateLookup = std::uintptr_t (*)(std::uintptr_t object, std::uintptr_t context);
struct HeldSwitch {
    std::atomic<std::uintptr_t> object{};
    std::atomic<int> mode{-1};
    std::atomic<std::uint32_t> asked{}, answered{};
    volatile std::uint8_t value[16]{};
};
HeldSwitch held_switches[std::size(grind_objects)];
std::atomic<StateLookup> lookup_original{};
// Held switches and forced settings only count while the trainer's tick keeps saying so (keep_holds_alive): should the
// tick stop, the game has its own again within half a second.
std::atomic<std::uint64_t> holds_until{};
bool holds_alive() noexcept { return GetTickCount64() < holds_until.load(std::memory_order_relaxed); }
std::atomic<bool> lookup_attempted{}, lookup_ready{};
std::atomic<std::uintptr_t> module_base{};
// Underflip speed, held in the lookup: the game sets UnderflipTimeScale every tick, and its clips had read it before the
// trainer's tick wrote it (owner's round of 2026-10-19: written 0.1, the next ticks read 1.000, 0.973, 0.949 and the
// underflip turned at full speed). Each lookup of the local skater's value scales what the game last put there, so
// whoever reads it after the game's write gets the scaled number.
struct HeldScale {
    std::atomic<std::uintptr_t> at{}; // the local skater's value; 0: not held
    std::atomic<float> factor{1}, raw{}, scaled{};
    std::atomic<bool> fresh{};
    std::atomic<std::uintptr_t> caller[8]{}; // who asked (offsets in the game) and how often, for the log
    std::atomic<std::uint32_t> calls[8]{};
} held_timescale;
void note_timescale_caller(std::uintptr_t from) noexcept {
    const auto base = module_base.load(std::memory_order_relaxed);
    const auto offset = base && from > base ? from - base : from;
    for (std::size_t i = 0; i < std::size(held_timescale.caller); ++i) {
        auto known = held_timescale.caller[i].load(std::memory_order_relaxed);
        if (!known && held_timescale.caller[i].compare_exchange_strong(known, offset, std::memory_order_relaxed)) known = offset;
        if (known == offset) {
            held_timescale.calls[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}
void scale_timescale(std::uintptr_t at) noexcept {
    float value{};
    if (!peek(at, value) || !std::isfinite(value)) return;
    // A number the trainer did not put there is the game's.
    if (!held_timescale.fresh.load(std::memory_order_relaxed) || value != held_timescale.scaled.load(std::memory_order_relaxed))
        held_timescale.raw.store(value, std::memory_order_relaxed);
    const float out = held_timescale.raw.load(std::memory_order_relaxed) * held_timescale.factor.load(std::memory_order_relaxed);
    held_timescale.scaled.store(out, std::memory_order_relaxed);
    held_timescale.fresh.store(true, std::memory_order_relaxed);
    if (value != out) (void)put(at, out);
}
// The game's setter of a number state (a FloatAsset's virtual, `self` = object + 0x60, the value in the third argument):
// it finds the value with 0x11dcff0 and stores it. The underflip's clips take their rate as they start, in the tick the
// game sets UnderflipTimeScale, so scaling on later lookups came too late (owner's round of 2026-10-19: the one reader
// through the lookup got the scaled number, the underflip still turned at full speed). Scaled as the game sets it.
constexpr std::uintptr_t float_setter = 0x11dd7d0;
constexpr unsigned char float_setter_bytes[]{0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0xc2, 0x41, 0xb0, 0x01, 0x48, 0x8d, 0x51, 0xa0};
using FloatSetter = void (*)(std::uintptr_t self, std::uintptr_t context, float value);
std::atomic<FloatSetter> setter_original{};
std::atomic<std::uint32_t> timescale_sets{}, timescale_sets_scaled{};
// Underflip speed: the underflip's playback rate is TrickFlipSpeed (cdbchooser_onboard_underflips_(pbrate)), set afresh as
// the underflip starts and taken by its clips then. While the trainer's tick says an underflip may start (the flick's
// window has closed in the air), that set is scaled. UnderflipTimeScale is not it: held at x0.01 from the game's first
// set, the underflip turned as fast (owner's round of 2026-10-19).
std::atomic<std::uintptr_t> underflip_flip_at{};
std::atomic<float> underflip_flip_factor{1};
std::atomic<std::uint32_t> flip_sets{}, flip_sets_scaled{};
void float_setter_hook(std::uintptr_t self, std::uintptr_t context, float value) {
    const auto object = self - 0x60;
    if (object == flip_speed_object.at.load(std::memory_order_relaxed) && std::isfinite(value)) {
        const auto armed = underflip_flip_at.load(std::memory_order_relaxed);
        const auto original = lookup_original.load(std::memory_order_acquire);
        if (armed && original && original(object, context) == armed && holds_alive()) {
            flip_sets.fetch_add(1, std::memory_order_relaxed);
            value *= underflip_flip_factor.load(std::memory_order_relaxed);
            flip_sets_scaled.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (object == underflip_timescale_object.at.load(std::memory_order_relaxed) && std::isfinite(value)) {
        timescale_sets.fetch_add(1, std::memory_order_relaxed);
        const auto original = lookup_original.load(std::memory_order_acquire);
        const auto at = original ? original(object, context) : 0;
        if (at && at == held_timescale.at.load(std::memory_order_relaxed) && holds_alive()) {
            const float out = value * held_timescale.factor.load(std::memory_order_relaxed);
            held_timescale.raw.store(value, std::memory_order_relaxed);
            held_timescale.scaled.store(out, std::memory_order_relaxed);
            held_timescale.fresh.store(true, std::memory_order_relaxed);
            timescale_sets_scaled.fetch_add(1, std::memory_order_relaxed);
            value = out;
        }
    }
    setter_original.load(std::memory_order_acquire)(self, context, value);
}
// The game's lookup of a state's value for writing (context, object, create) -> where to write. Hooked only to see who
// writes TrickFlipSpeed: the underflip's fresh 1.000 did not come through the number setter (owner's round of
// 2026-10-19: armed, nothing scaled). Two tables: writes while the trainer is armed for an underflip, and the others.
constexpr std::uintptr_t state_write_lookup = 0x11dcff0;
constexpr unsigned char state_write_lookup_bytes[]{0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57};
using StateWriteLookup = std::uintptr_t (*)(std::uintptr_t context, std::uintptr_t object, bool create);
std::atomic<StateWriteLookup> write_lookup_original{};
struct CallerTable {
    std::atomic<std::uintptr_t> caller[8]{};
    std::atomic<std::uint32_t> calls[8]{};
    void note(std::uintptr_t from) noexcept {
        const auto base = module_base.load(std::memory_order_relaxed);
        const auto offset = base && from > base ? from - base : from;
        for (std::size_t i = 0; i < std::size(caller); ++i) {
            auto known = caller[i].load(std::memory_order_relaxed);
            if (!known && caller[i].compare_exchange_strong(known, offset, std::memory_order_relaxed)) known = offset;
            if (known == offset) {
                calls[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    std::string take() noexcept {
        std::string text;
        for (std::size_t i = 0; i < std::size(caller); ++i)
            if (const auto from = caller[i].load(std::memory_order_relaxed))
                if (const auto n = calls[i].exchange(0, std::memory_order_relaxed))
                    text += std::format("{}0x{:x} x{}", text.empty() ? "" : ", ", from, n);
        return text.empty() ? "none" : text;
    }
} flip_writers_armed, flip_writers;
std::uintptr_t state_write_lookup_hook(std::uintptr_t context, std::uintptr_t object, bool create) {
    if (object && object == flip_speed_object.at.load(std::memory_order_relaxed)) {
        const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        (underflip_flip_at.load(std::memory_order_relaxed) ? flip_writers_armed : flip_writers).note(from);
    }
    return write_lookup_original.load(std::memory_order_acquire)(context, object, create);
}
// The game's copy of one state into another (destination object and context, source context, source object or 0 for the
// destination's own): it finds both values and has the destination's type copy them. This is how TrickFlipSpeed is set,
// at the flick and again as an underflip starts (the only writer of it, owner's round of 2026-10-19: 0x11d9a7b is its
// call of the write lookup). Underflip speed scales the copy right after it, before anything reads it.
constexpr std::uintptr_t state_copy = 0x11d9a40;
constexpr unsigned char state_copy_bytes[]{0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x28, 0x4d, 0x85, 0xc9, 0x48, 0x8b, 0xd9};
using StateCopy = void (*)(std::uintptr_t object, std::uintptr_t context, std::uintptr_t source_context, std::uintptr_t source_object);
std::atomic<StateCopy> copy_original{};
void state_copy_hook(std::uintptr_t object, std::uintptr_t context, std::uintptr_t source_context, std::uintptr_t source_object) {
    copy_original.load(std::memory_order_acquire)(object, context, source_context, source_object);
    if (!object || object != flip_speed_object.at.load(std::memory_order_relaxed)) return;
    const auto armed = underflip_flip_at.load(std::memory_order_relaxed);
    const auto original = lookup_original.load(std::memory_order_acquire);
    if (!armed || !original || !holds_alive() || original(object, context) != armed) return;
    flip_sets.fetch_add(1, std::memory_order_relaxed);
    float value{};
    if (!peek(armed, value) || !std::isfinite(value)) return;
    if (put(armed, value * underflip_flip_factor.load(std::memory_order_relaxed))) flip_sets_scaled.fetch_add(1, std::memory_order_relaxed);
}
// A rate controller's rate for this tick: the game's per-tick update of each one (0x10de730) asks this function
// (0x10dd2f0) for its time scale clamped to the min and max it copied from its asset when it was made, and plays its
// clips at that rate. The under part of an underflip runs on controllers made before it starts (or as it starts), so
// "Underflip part only" scales the rate of every controller asked during it (measured 2026-10-19: one to three, the
// underflip's own among them; the owner saw the under part turn at Underflip speed). In the air with another gravity
// "Animation timing" (Advanced gravity) scales them too (air_animation_rate, client_noclip.cpp).
constexpr std::uintptr_t rate_compute = 0x10dd2f0;
constexpr unsigned char rate_compute_bytes[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x83, 0xb9, 0xac, 0x00, 0x00, 0x00, 0x00};
using RateCompute = float (*)(std::uintptr_t self);
std::atomic<RateCompute> rate_original{};
std::atomic<float> under_part_rate{0};
float rate_compute_hook(std::uintptr_t self) {
    const float rate = rate_original.load(std::memory_order_acquire)(self);
    const float scale = under_part_rate.load(std::memory_order_relaxed);
    return (scale > 0 && holds_alive() ? rate * scale : rate) * air_animation_rate();
}
std::uintptr_t state_lookup_hook(std::uintptr_t object, std::uintptr_t context) {
    if (object && object == underflip_timescale_object.at.load(std::memory_order_relaxed)) {
        note_timescale_caller(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        const auto original = lookup_original.load(std::memory_order_acquire);
        const auto found = original ? original(object, context) : 0;
        if (found && found == held_timescale.at.load(std::memory_order_relaxed) && holds_alive()) scale_timescale(found);
        return found;
    }
    for (auto &held : held_switches) {
        if (held.object.load(std::memory_order_relaxed) != object) continue;
        held.asked.fetch_add(1, std::memory_order_relaxed);
        const int mode = held.mode.load(std::memory_order_relaxed);
        if (mode < 0 || !holds_alive()) break;
        held.answered.fetch_add(1, std::memory_order_relaxed);
        // The game may write through the answer too (its scripts set these): the value is put back for every asker.
        held.value[0] = mode ? 1 : 0;
        return reinterpret_cast<std::uintptr_t>(&held.value[0]);
    }
    const auto original = lookup_original.load(std::memory_order_acquire);
    const auto found = original ? original(object, context) : 0;
    return found;
}
bool lookup_code_known(std::uintptr_t base) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void *>(base + state_lookup), state_lookup_bytes, sizeof(state_lookup_bytes)) == 0;
    } __except (1) { return false; }
}
bool start_state_holds(std::uintptr_t base) noexcept {
    if (!base || lookup_attempted.exchange(true)) return lookup_ready.load(std::memory_order_relaxed);
    if (!lookup_code_known(base)) return false;
    auto *target = reinterpret_cast<void *>(base + state_lookup);
    void *original{};
    if (hook_prepare(target, reinterpret_cast<void *>(&state_lookup_hook), &original) != HookOk || !original) return false;
    lookup_original.store(reinterpret_cast<StateLookup>(original), std::memory_order_release);
    module_base.store(base, std::memory_order_relaxed);
    if (hook_enable(target) != HookOk) return false;
    lookup_ready.store(true, std::memory_order_relaxed);
    // The state copy: Underflip speed acts there.
    bool copy_known{};
    __try {
        copy_known = std::memcmp(reinterpret_cast<const void *>(base + state_copy), state_copy_bytes, sizeof(state_copy_bytes)) == 0;
    } __except (1) {}
    if (copy_known) {
        void *copy{};
        if (hook_prepare(reinterpret_cast<void *>(base + state_copy), reinterpret_cast<void *>(&state_copy_hook), &copy) == HookOk && copy) {
            copy_original.store(reinterpret_cast<StateCopy>(copy), std::memory_order_release);
            (void)hook_enable(reinterpret_cast<void *>(base + state_copy));
        }
    }
    // The rate controllers' rate: "Underflip part only" acts there.
    bool rate_known{};
    __try {
        rate_known = std::memcmp(reinterpret_cast<const void *>(base + rate_compute), rate_compute_bytes, sizeof(rate_compute_bytes)) == 0;
    } __except (1) {}
    if (rate_known) {
        void *rate{};
        if (hook_prepare(reinterpret_cast<void *>(base + rate_compute), reinterpret_cast<void *>(&rate_compute_hook), &rate) == HookOk && rate) {
            rate_original.store(reinterpret_cast<RateCompute>(rate), std::memory_order_release);
            (void)hook_enable(reinterpret_cast<void *>(base + rate_compute));
        }
    }
    // Who writes TrickFlipSpeed (for the log).
    bool write_lookup_known{};
    __try {
        write_lookup_known = std::memcmp(reinterpret_cast<const void *>(base + state_write_lookup), state_write_lookup_bytes, sizeof(state_write_lookup_bytes)) == 0;
    } __except (1) {}
    if (write_lookup_known) {
        void *writer{};
        if (hook_prepare(reinterpret_cast<void *>(base + state_write_lookup), reinterpret_cast<void *>(&state_write_lookup_hook), &writer) == HookOk && writer) {
            write_lookup_original.store(reinterpret_cast<StateWriteLookup>(writer), std::memory_order_release);
            (void)hook_enable(reinterpret_cast<void *>(base + state_write_lookup));
        }
    }
    // The number setter: only Underflip speed needs it, so the holds work without it.
    bool setter_known{};
    __try {
        setter_known = std::memcmp(reinterpret_cast<const void *>(base + float_setter), float_setter_bytes, sizeof(float_setter_bytes)) == 0;
    } __except (1) {}
    if (setter_known) {
        void *setter{};
        if (hook_prepare(reinterpret_cast<void *>(base + float_setter), reinterpret_cast<void *>(&float_setter_hook), &setter) == HookOk && setter) {
            setter_original.store(reinterpret_cast<FloatSetter>(setter), std::memory_order_release);
            (void)hook_enable(reinterpret_cast<void *>(base + float_setter));
        }
    }
    return true;
}
bool hold_underflip_timescale(std::uintptr_t at, float factor) noexcept {
    const auto was = held_timescale.at.load(std::memory_order_relaxed);
    if (!at) {
        // Let go: the game's own number back where the trainer's is still there.
        float value{};
        if (was && held_timescale.fresh.load(std::memory_order_relaxed) && peek(was, value) && value == held_timescale.scaled.load(std::memory_order_relaxed))
            (void)put(was, held_timescale.raw.load(std::memory_order_relaxed));
        held_timescale.at.store(0, std::memory_order_relaxed);
        held_timescale.fresh.store(false, std::memory_order_relaxed);
        return lookup_ready.load(std::memory_order_relaxed);
    }
    if (!lookup_ready.load(std::memory_order_relaxed)) return false;
    held_timescale.factor.store(factor, std::memory_order_relaxed);
    if (was != at) {
        held_timescale.fresh.store(false, std::memory_order_relaxed);
        held_timescale.at.store(at, std::memory_order_relaxed);
    }
    scale_timescale(at); // the tick's own read: the scaled number is there before the next lookup too
    return true;
}
void scale_under_part(float factor) noexcept { under_part_rate.store(factor, std::memory_order_relaxed); }
void arm_underflip_flip_speed(std::uintptr_t at, float factor) noexcept {
    underflip_flip_factor.store(factor, std::memory_order_relaxed);
    underflip_flip_at.store(at, std::memory_order_relaxed);
}
float underflip_timescale_game() noexcept { return held_timescale.raw.load(std::memory_order_relaxed); }
std::string underflip_timescale_callers() noexcept {
    std::string text;
    for (std::size_t i = 0; i < std::size(held_timescale.caller); ++i) {
        const auto from = held_timescale.caller[i].load(std::memory_order_relaxed);
        if (!from) continue;
        text += std::format("{}0x{:x} x{}", text.empty() ? "" : ", ", from, held_timescale.calls[i].exchange(0, std::memory_order_relaxed));
    }
    text = text.empty() ? "nobody" : text;
    text += std::format("; the game set it {} times, {} scaled; TrickFlipSpeed scaled for the underflip {} times",
                        timescale_sets.exchange(0, std::memory_order_relaxed), timescale_sets_scaled.exchange(0, std::memory_order_relaxed),
                        flip_sets_scaled.exchange(0, std::memory_order_relaxed));
    flip_sets.store(0, std::memory_order_relaxed);
    text += std::format("; TrickFlipSpeed written by {} (armed for an underflip), {} (otherwise)", flip_writers_armed.take(), flip_writers.take());
    return text;
}
void keep_holds_alive() noexcept { holds_until.store(GetTickCount64() + 500, std::memory_order_relaxed); }
int holds_in_force() noexcept {
    if (!holds_alive()) return 0;
    int count = 0;
    for (const auto &forced : forced_settings) count += forced.mode.load(std::memory_order_relaxed) >= 0;
    for (const auto &held : held_switches) count += held.mode.load(std::memory_order_relaxed) >= 0;
    return count;
}
void force_grind_setting(std::size_t index, int mode) noexcept {
    if (index < grind_settings.size()) forced_settings[index].mode.store(mode, std::memory_order_relaxed);
}
std::uint32_t grind_setting_asked(std::size_t index) noexcept {
    return index < grind_settings.size() ? forced_settings[index].asked.exchange(0, std::memory_order_relaxed) : 0;
}
void force_transition_setting(std::size_t index, int mode) noexcept {
    if (index < transition_settings.size()) forced_settings[grind_settings.size() + index].mode.store(mode, std::memory_order_relaxed);
}
std::uint32_t transition_setting_asked(std::size_t index) noexcept {
    return index < transition_settings.size() ? forced_settings[grind_settings.size() + index].asked.exchange(0, std::memory_order_relaxed) : 0;
}
void force_foot_setting(std::size_t index, int mode) noexcept {
    if (index < foot_settings.size()) forced_settings[foot_first + index].mode.store(mode, std::memory_order_relaxed);
}
std::uint32_t foot_setting_asked(std::size_t index) noexcept {
    return index < foot_settings.size() ? forced_settings[foot_first + index].asked.exchange(0, std::memory_order_relaxed) : 0;
}
TransitionState read_transition_state(std::uintptr_t base, std::uintptr_t entity) noexcept {
    TransitionState state;
    const auto block = states_block(base, entity);
    if (!block) return state;
    bool *const flags[]{&state.lip_found, &state.lip_tricking, &state.handplanting, &state.targeting_vert};
    std::array<float, 3> *const vectors[]{&state.lip_direction, &state.lip_nearest, &state.ground_normal};
    for (std::size_t i = 0; i < std::size(transition_objects); ++i) {
        const auto at = value_address(transition_objects[i], block);
        if (!at) continue;
        if (i < 4) {
            std::uint8_t value{};
            if (!peek(at, value)) continue;
            *flags[i] = value != 0;
        } else if (i == 4) {
            float value{};
            if (!peek(at, value) || !std::isfinite(value)) continue;
            state.transition_input = value;
        } else if (i == 5) {
            if (!peek(at, state.drop_in_type)) continue;
        } else {
            std::array<float, 3> value{};
            if (!peek(at, value) || !std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2])) continue;
            *vectors[i - 6] = value;
        }
        state.found |= 1u << i;
    }
    return state;
}
std::optional<bool> forced_gameplay_bool(const void *reference) noexcept {
    std::uintptr_t asset{};
    if (!reference || !peek(reinterpret_cast<std::uintptr_t>(reference), asset)) return {};
    asset &= ~std::uintptr_t{4};
    if (!asset) return {};
    // Which setting an asset is, remembered per thread for a few seconds (the game asks several times a frame; an
    // address can be another asset after a level change).
    struct Entry { std::uintptr_t asset{}; int setting{-1}; std::uint64_t until{}; };
    thread_local Entry cache[64];
    auto &entry = cache[((asset >> 4) ^ (asset >> 12)) & 63];
    const auto now = GetTickCount64();
    if (entry.asset != asset || now >= entry.until) entry = {asset, grind_setting_of(asset), now + 5000};
    if (entry.setting < 0) return {};
    auto &forced = forced_settings[static_cast<std::size_t>(entry.setting)];
    forced.asked.fetch_add(1, std::memory_order_relaxed);
    const int mode = forced.mode.load(std::memory_order_relaxed);
    if (mode < 0 || !holds_alive()) return {};
    return mode != 0;
}
void grind_switch_asked(std::size_t index, std::uint32_t &asked, std::uint32_t &answered) noexcept {
    asked = answered = 0;
    if (index >= std::size(held_switches)) return;
    asked = held_switches[index].asked.exchange(0, std::memory_order_relaxed);
    answered = held_switches[index].answered.exchange(0, std::memory_order_relaxed);
}
int grind_switch_status(std::uintptr_t base, std::uintptr_t entity, std::size_t index, std::uint32_t &handle) noexcept {
    handle = 0;
    if (index >= std::size(grind_objects)) return 0;
    const auto at = grind_objects[index].at.load(std::memory_order_relaxed);
    if (!at) return 0;
    if (!peek(at + object_handle, handle) || !(handle >> 24)) return 1;
    const auto block = states_block(base, entity);
    return block && value_address(grind_objects[index], block) ? 3 : 2;
}
void hold_grind_switches(std::uintptr_t base, std::uintptr_t entity, const std::int8_t *modes, std::int8_t *live) noexcept {
    static_assert(std::size(grind_objects) == grind_switches.size());
    const auto block = states_block(base, entity);
    for (std::size_t i = 0; i < std::size(grind_objects); ++i) {
        live[i] = -1;
        std::uint8_t flag{};
        // For the lookup hook: which object, and what it is held at.
        held_switches[i].object.store(grind_objects[i].at.load(std::memory_order_relaxed), std::memory_order_relaxed);
        held_switches[i].mode.store(modes[i], std::memory_order_relaxed);
        const auto at = block ? value_address(grind_objects[i], block) : 0;
        if (!at || !peek(at, flag)) continue;
        live[i] = flag != 0;
        if (modes[i] >= 0 && (flag != 0) != (modes[i] != 0)) (void)put_flag(at, modes[i] != 0);
    }
}
bool write_state_number(std::uintptr_t at, float value) noexcept { return at && std::isfinite(value) && put(at, value); }
std::array<std::uintptr_t, 6> state_pages(std::uintptr_t base, std::uintptr_t entity) noexcept {
    std::array<std::uintptr_t, 6> pages{};
    if (const auto block = states_block(base, entity))
        for (std::size_t k = 0; k < pages.size(); ++k)
            if (!peek(block + block_pages + k * 8, pages[k]) || !pointer(pages[k])) pages[k] = 0;
    return pages;
}
bool write_flip_speed(const FlipState &state, float speed) noexcept {
    if (!state || !std::isfinite(speed)) return false;
    if (state.clamped_at) (void)put(state.clamped_at, speed);
    return put(state.speed_at, speed);
}
int flip_trick_index(std::uint32_t trick) noexcept { return trick < trick_count ? trick_slider[trick] : -1; }
int underflip_trick_index(std::uint32_t trick) noexcept {
    int slot = -1;
    switch (trick) {
    case 2: case 17: case 43: case 47: slot = 0; break; // kickflip, nollie kickflip, their underflips
    case 3: case 18: case 44: case 48: slot = 1; break; // heelflip
    case 4: case 19: case 45: case 49: slot = 2; break; // pop shuvit
    case 7: case 22: case 46: case 50: slot = 3; break; // FS pop shuvit
    case 36: case 55: slot = 4; break;                  // impossible
    case 37: case 56: slot = 5; break;                  // front foot impossible
    case 38: case 57: slot = 6; break;                  // nollie impossible
    case 39: case 58: slot = 7; break;                  // nollie back foot impossible
    default: return -1;
    }
    return static_cast<int>(first_underflip_slot) + slot;
}
std::string_view flip_trick_name(std::uint32_t trick) noexcept { return trick < trick_count ? game_trick_names[trick] : std::string_view{}; }
}
