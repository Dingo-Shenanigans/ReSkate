#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "playstation_report.h"

// Portable policy shared by native BMX sampling/capture and its tests. Device polling,
// foreground detection and capture-hook installation stay in the overlay adapter.
namespace dingosdk::input {
inline constexpr std::uint32_t bmx_keyboard_device = 0x10000000u;
inline constexpr std::uint64_t bmx_capture_lease_ms = 250;
struct BmxReadiness {
    bool overlay_ready{}, focused{}, menu_blocked{}, capture_blocked{};
};
inline bool bmx_input_allowed(BmxReadiness gates) noexcept {
    return gates.overlay_ready && gates.focused && !gates.menu_blocked && !gates.capture_blocked;
}
inline bool bmx_capture_live(std::uint64_t until, std::uint64_t now, bool overlay_alive) noexcept {
    return overlay_alive && until > now;
}
inline bool block_owned_input(bool bypass, bool menu, bool freecam, bool prompt, bool bmx) noexcept {
    return !bypass && (menu || freecam || prompt || bmx);
}
// Two independent callers reserve the same North-only filtering mode. Clearing
// a nearby-bike lease cannot clear an equipment chord still held by the player.
inline bool bmx_north_capture_live(std::uint64_t interaction_until,std::uint64_t quick_until,
                                  std::uint64_t now,bool overlay_alive) noexcept {
    return bmx_capture_live(interaction_until,now,overlay_alive)||
        bmx_capture_live(quick_until,now,overlay_alive);
}
enum class BmxNativeCapture { none, all, controls, interaction };
inline BmxNativeCapture bmx_native_capture(bool bypass, bool menu, bool freecam, bool prompt, bool bmx,bool interaction=false) noexcept {
    if(bypass)return BmxNativeCapture::none;
    if(menu||freecam||prompt)return BmxNativeCapture::all;
    return bmx?BmxNativeCapture::controls:interaction?BmxNativeCapture::interaction:BmxNativeCapture::none;
}
inline std::uint32_t bmx_native_packet_tag(BmxNativeCapture capture) noexcept {
    // A stationary physical packet must still publish the transition from
    // BMX D-pad pass-through to a fully neutral menu/freecam/prompt state.
    return capture==BmxNativeCapture::all?0x80000000u:capture==BmxNativeCapture::controls?0x40000000u:
        capture==BmxNativeCapture::interaction?0x20000000u:0;
}
// Keep the game's existing D-pad actions; never translate them into BMX input.
template<class Pad> inline void bmx_filter_native_pad(Pad& pad,BmxNativeCapture capture) noexcept {
    if(capture==BmxNativeCapture::none)return;
    if(capture==BmxNativeCapture::interaction){pad.wButtons=static_cast<std::uint16_t>(pad.wButtons&~0x8000u);return;}
    const auto buttons=static_cast<std::uint16_t>(capture==BmxNativeCapture::controls?pad.wButtons&0x0fu:0);
    pad={};pad.wButtons=buttons;
}
inline bool bmx_native_dpad_keystroke(std::uint16_t key,BmxNativeCapture capture) noexcept {
    return capture==BmxNativeCapture::none||(capture==BmxNativeCapture::interaction&&key!=0x5803)||
        (capture==BmxNativeCapture::controls&&key>=0x5810&&key<=0x5813);
}
inline std::uint32_t bmx_native_pov(std::uint32_t value,BmxNativeCapture capture) noexcept {
    if(capture==BmxNativeCapture::none||capture==BmxNativeCapture::interaction)return value;
    return capture==BmxNativeCapture::controls&&value<36000?value:0xffffffffu;
}
inline bool bmx_filter_playstation_report(PlayStationPad kind,bool bluetooth,std::uint8_t* bytes,
                                        std::size_t size,BmxNativeCapture capture) noexcept {
    if(!parse_playstation_report(kind,bluetooth,bytes,size)) {
        if(capture!=BmxNativeCapture::none&&bytes)std::fill_n(bytes,size,std::uint8_t{});
        return false;
    }
    if(capture==BmxNativeCapture::none)return true;
    const bool sense_full=kind==PlayStationPad::dualsense&&(bytes[0]==0x31||(bytes[0]==1&&!bluetooth));
    const bool simple=bytes[0]==1&&bluetooth;
    const bool bt_full=bytes[0]==0x31||bytes[0]==0x11;
    // The shared sampler needs only a primary-field prefix. Pass-through needs
    // the entire known input layout, including motion/touch and the wire CRC.
    // DS4's 78-byte BT report can be padded to the existing Windows 547-byte
    // longest-report length; that padding is not part of its wire checksum.
    const bool complete=simple?(size==10||size==78||size==547):
        bt_full?(size==78||(kind==PlayStationPad::dualshock4&&size==547)):size==64;
    if(!complete) {std::fill_n(bytes,size,std::uint8_t{});return false;}
    const unsigned offset=bytes[0]==0x31?2:bytes[0]==0x11?3:1;
    auto* data=bytes+offset;
    const auto refresh_crc=[&] {
        if(!bt_full)return;
        std::uint32_t crc=0xffffffffu;
        const auto feed=[&](std::uint8_t byte) {
            crc^=byte;
            for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
        };
        feed(0xa1);for(std::size_t i=0;i<74;++i)feed(bytes[i]);
        crc=~crc;std::memcpy(bytes+74,&crc,sizeof(crc));
    };
    if(capture==BmxNativeCapture::interaction) {
        // North is bit7 in the shared hat/face byte. Preserve every other
        // control, sensor, touch, status and padding byte of the native report.
        data[sense_full?7:4]&=0x7f;
        refresh_crc();return true;
    }
    auto hat=static_cast<std::uint8_t>(data[sense_full?7:4]&0x0f);
    if(capture!=BmxNativeCapture::controls||hat>7)hat=8;
    std::fill_n(data,4,std::uint8_t{128});
    if(sense_full) {
        data[4]=data[5]=0; // retain transport sequence at+6
        data[7]=hat;data[8]=data[9]=data[10]=0;
    } else {
        data[4]=hat;data[5]=0;data[6]&=0xfc; // retain DS4 transport sequence bits
        data[7]=data[8]=0;
    }
    // Sony's full input structures: gyro/accel are signed 16-bit raw samples;
    // contact bit7 means inactive. Preserve timestamps, sequence and battery
    // status, but never forward physical motion or touch while captured.
    // https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c
    const auto release_touch=[](std::uint8_t* point) {
        point[0]=0x80;std::fill_n(point+1,3,std::uint8_t{});
    };
    if(simple) {
        std::fill(bytes+10,bytes+size,std::uint8_t{});
    } else if(sense_full) {
        std::fill_n(data+15,12,std::uint8_t{});
        release_touch(data+32);release_touch(data+36);
    } else {
        std::fill_n(data+12,12,std::uint8_t{});
        data[32]=1; // publish an inactive contact packet, not a missing update
        for(unsigned i=0;i<(bt_full?4u:3u);++i) {
            auto* touch=data+33+9*i; // retain each touch packet's timestamp
            release_touch(touch+1);release_touch(touch+5);
        }
    }
    if(bt_full) {
        if(size>78)std::fill(bytes+78,bytes+size,std::uint8_t{});
        refresh_crc();
    }
    return true;
}
inline bool bmx_north_button(bool button,unsigned instance) noexcept {return button&&instance==3;}
inline bool bmx_clear_north_byte(void* data,std::size_t size,std::size_t offset) noexcept {
    if(!data||offset>=size)return false;
    static_cast<std::uint8_t*>(data)[offset]=0;return true;
}
inline bool bmx_menu_requested(bool escape, std::uint16_t buttons) noexcept {
    return escape || (buttons & 0x30u) != 0; // XInput Start/Back, including native PS equivalents
}
struct BmxAxes {
    float left_x{}, left_y{}, right_x{}, right_y{}, pedal{}, brake{};
    bool quick_hop{}, dismount_requested{};
    float left_trigger{},right_trigger{};
    bool left_shoulder{},right_shoulder{};
};
inline BmxAxes bmx_pad_axes(std::int16_t left_x, std::int16_t left_y,
    std::int16_t right_x, std::int16_t right_y, std::uint16_t buttons,
    std::uint8_t left_trigger=0,std::uint8_t right_trigger=0) noexcept {
    const auto axis = [](std::int16_t value) { return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f); };
    // The locally inspected BMX action map binds Accelerate/Brake to South/East,
    // Quick Hop to R3 and Dismount to North. PS reports use these same XInput bits.
    // Triggers belong to lean/lateral actions; they do not pedal or brake this bike.
    return {axis(left_x), axis(left_y), axis(right_x), axis(right_y),
        (buttons & 0x1000u) ? 1.0f : 0.0f, (buttons & 0x2000u) ? 1.0f : 0.0f,
        (buttons & 0x0080u) != 0, (buttons & 0x8000u) != 0,
        left_trigger/255.0f,right_trigger/255.0f,(buttons&0x0100u)!=0,(buttons&0x0200u)!=0};
}
struct BmxKeyboard {
    bool w{}, a{}, s{}, d{}, i{}, j{}, k{}, l{}, pedal{}, brake{};
    bool left_shoulder{},right_shoulder{},left_trigger{},right_trigger{};
};
inline BmxAxes bmx_keyboard_axes(BmxKeyboard keys) noexcept {
    return {static_cast<float>(keys.d) - static_cast<float>(keys.a),
        static_cast<float>(keys.w) - static_cast<float>(keys.s),
        static_cast<float>(keys.l) - static_cast<float>(keys.j),
        static_cast<float>(keys.i) - static_cast<float>(keys.k),
        static_cast<float>(keys.pedal), static_cast<float>(keys.brake),false,false,
        static_cast<float>(keys.left_trigger),static_cast<float>(keys.right_trigger),
        keys.left_shoulder,keys.right_shoulder};
}
} // namespace dingosdk::input
