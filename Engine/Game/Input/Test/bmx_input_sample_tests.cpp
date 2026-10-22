#include "Engine/Game/Input/bmx_input_sample.h"
#include "Engine/Game/BMX/bmx_input.h"
#include <cmath>
#include <cstdio>
#include <array>
#include <cstring>

namespace {
using namespace dingosdk::input;
using namespace dingosdk::bmx;
int failures{};
void check(bool valid, const char* message) {
    if (!valid) { std::printf("FAIL: %s\n", message); ++failures; }
}
bool near(float a, float b) { return std::abs(a - b) < 1e-6f; }
RawInput raw(BmxAxes axes, BmxReadiness gates = {true, true, false, false}, std::uint32_t device = bmx_keyboard_device) {
    RawInput result;
    result.available = bmx_input_allowed(gates);
    result.focused = gates.focused;
    result.menu_open = gates.menu_blocked;
    result.device = device;
    result.left_x = axes.left_x; result.left_y = axes.left_y;
    result.right_x = axes.right_x; result.right_y = axes.right_y;
    result.pedal = axes.pedal; result.brake = axes.brake;
    result.quick_hop = axes.quick_hop;
    result.left_trigger=axes.left_trigger;result.right_trigger=axes.right_trigger;
    result.left_shoulder=axes.left_shoulder;result.right_shoulder=axes.right_shoulder;
    return result;
}
void native_ranges_and_keyboard_layout() {
    const auto pad = bmx_pad_axes(-32768, 32767, 32767, -32768, 0x1000u);
    check(near(pad.left_x, -1) && near(pad.left_y, 1) && near(pad.right_x, 1) && near(pad.right_y, -1),
        "native Xbox/PS signed axis endpoints normalize without overflow");
    check(near(pad.pedal, 1) && near(pad.brake, 0), "South/Cross/A pedals without braking");
    const auto buttons = bmx_pad_axes(0, 0, 0, 0, 0xb080u);
    check(near(buttons.pedal, 1) && near(buttons.brake, 1) && buttons.quick_hop && buttons.dismount_requested,
        "East/Circle/B brakes, R3 requests quick hop and North/Triangle/Y requests dismount");
    const auto unrelated = bmx_pad_axes(0, 0, 0, 0, 0x037fu);
    check(near(unrelated.pedal, 0) && near(unrelated.brake, 0) && !unrelated.quick_hop && !unrelated.dismount_requested,
        "D-pad, menu, shoulder and L3 buttons do not become propulsion, braking or quick hop");
    const auto zero = bmx_pad_axes(0, 0, 0, 0, 0);
    check(near(zero.left_x, 0) && near(zero.pedal, 0), "resting native controls stay zero");
    const auto modifiers=bmx_pad_axes(0,0,0,0,0x030fu,51,255);
    check(near(modifiers.left_trigger,.2f)&&near(modifiers.right_trigger,1)&&
        modifiers.left_shoulder&&modifiers.right_shoulder&&modifiers.pedal==0&&modifiers.brake==0,
        "Xbox/native Sony modifier channels preserve analog triggers and shoulders without propulsion");
    BmxKeyboard keys;
    keys.w = keys.a = keys.k = keys.l = keys.pedal = true;
    auto axes = bmx_keyboard_axes(keys);
    check(near(axes.left_x, -1) && near(axes.left_y, 1), "W/A map to front left/forward");
    check(near(axes.right_x, 1) && near(axes.right_y, -1), "K/L map to rear right/back");
    check(near(axes.pedal, 1) && near(axes.brake, 0), "E/Q represent separate pedal and brake");
    keys.s = keys.d = keys.i = keys.j = keys.brake = true;
    axes = bmx_keyboard_axes(keys);
    check(near(axes.left_x, 0) && near(axes.left_y, 0) && near(axes.right_x, 0) && near(axes.right_y, 0),
        "opposed fallback keys cancel each stick axis");
    check(near(axes.pedal, 1) && near(axes.brake, 1), "simultaneous pedal/brake remain explicit inputs");
}
void fallback_and_pad_drive_the_same_mapper() {
    InputMapper keyboard, controller;
    constexpr InputContext ground{true};
    constexpr double seconds = 1.0 / 60;
    keyboard.update(raw({}), ground, seconds);
    controller.update(raw({}, {true, true, false, false}, 1), ground, seconds);
    BmxKeyboard keys; keys.k = true;
    for (int i = 0; i < 20; ++i) {
        keyboard.update(raw(bmx_keyboard_axes(keys)), ground, seconds);
        controller.update(raw(bmx_pad_axes(0, 0, 0, -32768, 0), {true, true, false, false}, 1), ground, seconds);
    }
    keys.k = false; keys.i = true;
    const auto from_keys = keyboard.update(raw(bmx_keyboard_axes(keys)), ground, seconds);
    const auto from_pad = controller.update(raw(bmx_pad_axes(0, 0, 0, 32767, 0), {true, true, false, false}, 1), ground, seconds);
    check(from_keys.hop_pressed && from_pad.hop_pressed && from_keys.hop_strength == from_pad.hop_strength,
        "native pad and K-to-I fallback produce the same charged hop");
    // A pad disappearing into keyboard fallback changes identity, even when its axes match.
    const auto changed = controller.update(raw(bmx_keyboard_axes(keys)), ground, seconds);
    check(!changed.active, "source switch with held controls cannot carry a pending gesture into fallback");
}
void streets_named_pad_tricks() {
    constexpr double seconds=1.0/60;
    for(bool sony:{false,true})for(unsigned move=0;move<4;++move) {
        InputMapper mapper;
        mapper.update(raw({}, {true,true,false,false},1),{true},seconds);
        mapper.update(raw({}, {true,true,false,false},1),{false},seconds);
        BmxAxes axes;
        if(!sony) {
            axes=move<2?bmx_pad_axes(0,0,0,-32768,move==0?0x100:0x200):
                bmx_pad_axes(0,0,move==2?-32768:32767,0,0,255,255);
        } else {
            // Full DualSense USB: native Y grows down; button/triggers must
            // survive the sampler's bypass even though the game sees D-pad only.
            std::array<std::uint8_t,64> bytes{};bytes[0]=1;
            bytes[1]=bytes[2]=bytes[3]=bytes[4]=128;bytes[8]=8;
            if(move<2) {bytes[4]=255;bytes[9]=move==0?1:2;}
            else {bytes[3]=move==2?0:255;bytes[5]=bytes[6]=255;}
            const auto pad=dingosdk::parse_playstation_report(dingosdk::PlayStationPad::dualsense,false,bytes.data(),bytes.size());
            check(pad.has_value(),"synthetic native Sony packet is recognized");
            if(!pad)continue;
            axes=bmx_pad_axes(pad->left_x,pad->left_y,pad->right_x,pad->right_y,pad->buttons,pad->left_trigger,pad->right_trigger);
        }
        const auto action=mapper.update(raw(axes,{true,true,false,false},1),{false},seconds);
        const auto direction=move%2==0?TrickDirection::left:TrickDirection::right;
        check(move<2?action.handlebar==direction&&action.tailwhip==TrickDirection::none:
            action.tailwhip==direction&&action.handlebar==TrickDirection::none,
            "Xbox and native Sony sampled inputs select the four authored Streets barspin/tailwhip directions");
        const auto held=mapper.update(raw(axes,{true,true,false,false},1),{false},seconds);
        check(held.handlebar==TrickDirection::none&&held.tailwhip==TrickDirection::none,
            "native held modifier+stick cannot repeatedly submit a trick");
    }
}
void menu_focus_and_capture_priority() {
    constexpr double seconds = 1.0 / 60;
    for (int reason = 0; reason < 4; ++reason) {
        InputMapper mapper;
        mapper.update(raw({}), {false}, seconds);
        auto gates = BmxReadiness{true, true, false, false};
        if (reason == 0) gates.overlay_ready = false;
        if (reason == 1) gates.focused = false;
        if (reason == 2) gates.menu_blocked = true;
        if (reason == 3) gates.capture_blocked = true;
        BmxAxes held; held.right_x = 1;held.left_trigger=held.right_trigger=1;
        check(!mapper.update(raw(held, gates), {false}, seconds).active, "each external ownership gate suppresses riding actions");
        check(!mapper.update(raw(held), {false}, seconds).active, "held input cannot rearm after gate releases");
        check(mapper.update(raw({}), {false}, seconds).active, "neutral after owner release rearms input");
        check(mapper.update(raw(held), {false}, seconds).tailwhip == TrickDirection::right,
            "fresh Streets trigger-pair plus rear direction works after ownership release");
    }
    check(bmx_menu_requested(true, 0) && bmx_menu_requested(false, 0x10) && bmx_menu_requested(false, 0x20),
        "Escape and pad Start/Back request return of native menu control");
    check(!bmx_menu_requested(false, 0x1000), "pedal/trick face button is not a menu request");
}
void independent_leases_and_bypass() {
    check(bmx_capture_live(1250, 1000, true) && bmx_capture_live(1250, 1249, true), "fresh BMX lease owns native input");
    check(!bmx_capture_live(1250, 1250, true) && !bmx_capture_live(1250, 1251, true), "stalled rider tick cannot strand an expired capture");
    check(!bmx_capture_live(0, 1000, true) && !bmx_capture_live(1250, 1000, false), "release and overlay shutdown clear capture");
    check(block_owned_input(false, false, false, false, true), "BMX independently blocks ordinary native input reads");
    check(!block_owned_input(true, false, false, false, true), "overlay's single raw sample bypasses its own capture");
    check(block_owned_input(false, false, true, false, false) && block_owned_input(false, false, false, true, false) &&
        block_owned_input(false, true, false, false, false), "releasing BMX leaves menu/freecam/prompt ownership untouched");
    check(!block_owned_input(false, false, false, false, false), "unowned input returns to ordinary native game path");
}
void native_dpad_capture_priority() {
    using Capture=BmxNativeCapture;
    struct Pad {std::uint16_t wButtons;std::uint8_t bLeftTrigger,bRightTrigger;std::int16_t sThumbLX,sThumbLY,sThumbRX,sThumbRY;};
    constexpr std::uint32_t stationary_packet=0xe5800017;
    check((stationary_packet^bmx_native_packet_tag(Capture::controls))!=(stationary_packet^bmx_native_packet_tag(Capture::all))&&
        (stationary_packet^bmx_native_packet_tag(Capture::controls))!=stationary_packet&&
        (stationary_packet^bmx_native_packet_tag(Capture::all))!=stationary_packet,
        "each ownership transition publishes a fresh packet even if a physical D-pad is held still");
    for(unsigned gates=0;gates<32;++gates) {
        const bool bypass=(gates&1)!=0,menu=(gates&2)!=0,freecam=(gates&4)!=0,prompt=(gates&8)!=0,bmx=(gates&16)!=0;
        const auto mode=bmx_native_capture(bypass,menu,freecam,prompt,bmx);
        check(block_owned_input(bypass,menu,freecam,prompt,bmx)==(mode!=Capture::none),"new pad policy preserves existing keyboard/mouse capture ownership");
        for(auto dpad:std::array<std::uint16_t,10>{0,1,9,8,10,2,6,4,5,15}) {
            Pad pad{static_cast<std::uint16_t>(0xfff0|dpad),123,245,-32768,32767,-12345,23456};
            const auto before=pad;bmx_filter_native_pad(pad,mode);
            if(bypass||(!menu&&!freecam&&!prompt&&!bmx))
                check(std::memcmp(&pad,&before,sizeof(pad))==0,"unowned/bypass XInput payload is byte-identical");
            else {
                check(pad.wButtons==(!menu&&!freecam&&!prompt?dpad:0),"BMX-only retains native D-pad cardinals/diagonals; overlapping UI/freecam/prompt clears them");
                check(!pad.bLeftTrigger&&!pad.bRightTrigger&&!pad.sThumbLX&&!pad.sThumbLY&&!pad.sThumbRX&&!pad.sThumbRY,
                    "preserving D-pad cannot leak BMX sticks, triggers or face/shoulder buttons into skating");
            }
        }
    }
    for(auto key:std::array<std::uint16_t,4>{0x5810,0x5811,0x5812,0x5813})
        check(bmx_native_dpad_keystroke(key,Capture::controls)&&!bmx_native_dpad_keystroke(key,Capture::all),"only native D-pad key down/up events survive BMX-exclusive capture");
    for(auto key:std::array<std::uint16_t,5>{0x5800,0x5814,0x5815,0x5824,0x5834})
        check(!bmx_native_dpad_keystroke(key,Capture::controls)&&bmx_native_dpad_keystroke(key,Capture::none),"menu, face and thumb diagonal keystrokes are not confused with D-pad");
    for(std::uint32_t value:{0u,4500u,9000u,13500u,18000u,22500u,27000u,31500u,35999u,36000u,65535u,0xffffffffu}) {
        check(bmx_native_pov(value,Capture::controls)==(value<36000?value:0xffffffffu),"DirectInput POV preserves valid directions/diagonals and releases malformed or centered hats");
        check(bmx_native_pov(value,Capture::all)==0xffffffffu&&bmx_native_pov(value,Capture::none)==value,"POV respects stronger owner and ordinary pass-through");
    }
}
std::uint32_t report_crc(const std::uint8_t* bytes,std::size_t size) {
    // Independent table implementation, versus the production bit-at-a-time CRC.
    std::array<std::uint32_t,256> table{};
    for(unsigned i=0;i<256;++i) {auto value=i;for(unsigned b=0;b<8;++b)value=(value&1)?0xedb88320u^(value>>1):value>>1;table[i]=value;}
    auto crc=table[(0xffffffffu^0xa1u)&255]^(0xffffffffu>>8);
    for(std::size_t i=0;i<size;++i)crc=table[(crc^bytes[i])&255]^(crc>>8);
    return ~crc;
}
void playstation_dpad_reports() {
    using Capture=BmxNativeCapture;
    struct Transport {dingosdk::PlayStationPad kind;bool bluetooth;unsigned id,size;};
    constexpr std::array<Transport,9> formats{{
        {dingosdk::PlayStationPad::dualsense,false,1,64},{dingosdk::PlayStationPad::dualshock4,false,1,64},
        {dingosdk::PlayStationPad::dualsense,true,0x31,78},{dingosdk::PlayStationPad::dualshock4,true,0x11,78},
        {dingosdk::PlayStationPad::dualsense,true,1,78},{dingosdk::PlayStationPad::dualshock4,true,1,78},
        {dingosdk::PlayStationPad::dualshock4,true,0x11,547},
        {dingosdk::PlayStationPad::dualsense,true,1,547},{dingosdk::PlayStationPad::dualshock4,true,1,547}}};
    constexpr std::array<std::uint16_t,8> expected{1,9,8,10,2,6,4,5};
    for(const auto format:formats)for(unsigned hat=0;hat<10;++hat)for(auto capture:{Capture::none,Capture::all,Capture::controls}) {
        std::array<std::uint8_t,551> memory;memory.fill(0x5a);auto* bytes=memory.data()+2;bytes[0]=static_cast<std::uint8_t>(format.id);
        const unsigned offset=format.id==0x31?2:format.id==0x11?3:1;
        const bool sense=format.kind==dingosdk::PlayStationPad::dualsense&&(format.id==0x31||!format.bluetooth);
        auto* data=bytes+offset;const unsigned buttons=sense?7:4;
        data[0]=0;data[1]=255;data[2]=40;data[3]=220;data[buttons]=static_cast<std::uint8_t>(0xf0|hat);
        data[buttons+1]=0xff;data[buttons+2]=0xff;
        if(sense){data[4]=200;data[5]=180;data[6]=0xab;}else {data[7]=200;data[8]=180;}
        const auto original=memory;
        check(bmx_filter_playstation_report(format.kind,format.bluetooth,bytes,format.size,capture),"all supported USB/BT full/simple reports remain recognized");
        check(memory[0]==0x5a&&memory[1]==0x5a&&memory[2+format.size]==0x5a&&memory[3+format.size]==0x5a,"report filtering stays inside actual received length");
        if(capture==Capture::none) {check(memory==original,"overlay bypass leaves complete Sony transport untouched");continue;}
        const auto parsed=dingosdk::parse_playstation_report(format.kind,format.bluetooth,bytes,format.size);
        const auto wanted=capture==Capture::controls&&hat<8?expected[hat]:0;
        check(parsed&&parsed->buttons==wanted&&!parsed->left_trigger&&!parsed->right_trigger,"only native Sony hat survives BMX; higher-priority capture remains fully neutral");
        check(data[0]==128&&data[1]==128&&data[2]==128&&data[3]==128,"native Sony sticks stay neutral during D-pad pass-through");
        check(sense?data[6]==0xab:(data[6]&0xfc)==0xfc,"report sequence metadata is retained");
        const auto zero=[](const std::uint8_t* p,unsigned count){return std::all_of(p,p+count,[](auto value){return value==0;});};
        const auto inactive=[&](const std::uint8_t* p){return p[0]==0x80&&zero(p+1,3);};
        if(format.id==1&&format.bluetooth) {
            check(zero(bytes+10,format.size-10),"minimal Bluetooth report padding cannot carry motion/touch or vendor inputs");
        } else if(sense) {
            check(data[10]==0&&zero(data+15,12),"all DualSense button lanes and raw gyro/accel samples are neutral");
            check(inactive(data+32)&&inactive(data+36),"both DualSense touch contacts explicitly release and clear coordinates");
            check(std::all_of(data+27,data+31,[](auto value){return value==0x5a;})&&
                std::all_of(data+52,data+55,[](auto value){return value==0x5a;}),"DualSense sensor timestamp and battery/status are preserved");
        } else {
            check(zero(data+12,12),"DualShock4 raw gyro/accel samples cannot pass through capture");
            check(data[32]==1,"DualShock4 publishes an inactive touch update to release previously active contacts");
            for(unsigned i=0;i<(format.id==0x11?4u:3u);++i) {
                const auto* touch=data+33+9*i;
                check(touch[0]==0x5a&&inactive(touch+1)&&inactive(touch+5),"all DualShock4 touch-history contacts are inactive with timestamps retained");
            }
            check(data[9]==0x5a&&data[10]==0x5a&&data[11]==0x5a&&data[29]==0x5a&&data[30]==0x5a,
                "DualShock4 sensor timestamp, temperature and battery/status are preserved");
        }
        if(format.id==0x31||format.id==0x11) {
            std::uint32_t actual{};std::memcpy(&actual,bytes+74,4);
            check(actual==report_crc(bytes,74),"full Bluetooth wire CRC stays at offset74 even in a padded547-byte read");
            check(format.size==78||zero(bytes+78,format.size-78),"padded full report tail is suppressed independently of the wire checksum");
        }
    }
    for(unsigned reason=0;reason<3;++reason) {
        std::array<std::uint8_t,12> report;report.fill(0xff);report[0]=reason==0?0x32:1;
        const auto kind=reason==1?dingosdk::PlayStationPad::none:dingosdk::PlayStationPad::dualsense;
        const auto length=reason==2?2u:12u;
        check(!bmx_filter_playstation_report(kind,false,report.data(),length,Capture::controls),"unknown/truncated reports cannot expose unfiltered native controls");
        check(std::all_of(report.begin(),report.begin()+length,[](auto byte){return byte==0;}),"unrecognized captured report payload is suppressed");
    }
    for(auto kind:{dingosdk::PlayStationPad::dualsense,dingosdk::PlayStationPad::dualshock4})
        for(unsigned length:{12u,64u,77u,79u}) {
            std::array<std::uint8_t,79> report;report.fill(0xff);report[0]=kind==dingosdk::PlayStationPad::dualsense?0x31:0x11;
            check(!bmx_filter_playstation_report(kind,true,report.data(),length,Capture::controls),"full Bluetooth primary prefixes or unknown lengths cannot bypass typed-tail validation");
            check(std::all_of(report.begin(),report.begin()+length,[](auto value){return value==0;}),"invalid full-report fragments are suppressed");
        }
}
void interaction_only_north_and_priority() {
    using Capture=BmxNativeCapture;
    struct Pad {std::uint16_t wButtons;std::uint8_t bLeftTrigger,bRightTrigger;std::int16_t sThumbLX,sThumbLY,sThumbRX,sThumbRY;};
    for(unsigned gates=0;gates<64;++gates) {
        const bool bypass=(gates&1)!=0,menu=(gates&2)!=0,freecam=(gates&4)!=0,prompt=(gates&8)!=0,
            mounted=(gates&16)!=0,interaction=(gates&32)!=0;
        const auto mode=bmx_native_capture(bypass,menu,freecam,prompt,mounted,interaction);
        const auto expected=bypass?Capture::none:menu||freecam||prompt?Capture::all:
            mounted?Capture::controls:interaction?Capture::interaction:Capture::none;
        check(mode==expected,"off-bike interaction never overrides menu/prompt/freecam/mounted ownership or overlay bypass");
        Pad pad{0xffff,71,192,-32768,32767,-12000,23000};auto wanted=pad;
        if(mode==Capture::interaction)wanted.wButtons&=0x7fff;
        else if(mode==Capture::controls){wanted={};wanted.wButtons=15;}
        else if(mode==Capture::all)wanted={};
        bmx_filter_native_pad(pad,mode);
        check(std::memcmp(&pad,&wanted,sizeof(pad))==0,"interaction removes exactly North while preserving walking axes, D-pad, triggers and all other buttons");
    }
    for(auto left:{Capture::none,Capture::all,Capture::controls,Capture::interaction})
        for(auto right:{Capture::none,Capture::all,Capture::controls,Capture::interaction})
            if(left!=right)check(bmx_native_packet_tag(left)!=bmx_native_packet_tag(right),
                "every transition between interaction/full/partial/unowned publishes a distinct XInput packet");
    for(unsigned key=0x5800;key<=0x583f;++key)
        check(bmx_native_dpad_keystroke(static_cast<std::uint16_t>(key),Capture::interaction)==(key!=0x5803),
            "XInput interaction keystrokes suppress only Y down/up and preserve every D-pad/axis/other button event");
    for(std::uint32_t pov:{0u,4500u,18000u,31500u,65535u,0xffffffffu})
        check(bmx_native_pov(pov,Capture::interaction)==pov,"interaction never changes native DirectInput POV including its centered sentinel");
    std::array<std::uint8_t,12> direct{};direct.fill(0x9a);auto before=direct;
    check(bmx_north_button(true,3)&&!bmx_north_button(false,3)&&!bmx_north_button(true,2),
        "DirectInput North capture requires a typed button instance3, not an axis or another button");
    check(bmx_clear_north_byte(direct.data(),direct.size(),7),"bounded typed North byte may be released");
    before[7]=0;check(direct==before,"DirectInput interaction touches only one typed button byte, preserving axes/padding/other inputs");
    check(!bmx_clear_north_byte(direct.data(),direct.size(),direct.size())&&!bmx_clear_north_byte(nullptr,12,0)&&direct==before,
        "invalid DirectInput byte offsets cannot change another part of the native state");
    check(bmx_capture_live(1250,1249,true)&&!bmx_capture_live(1250,1250,true)&&!bmx_capture_live(1250,1200,false),
        "interaction lease expiry and dead-overlay gates cannot leave a permanent native North capture");
}
void independent_north_reservations() {
    check(bmx_north_capture_live(0,1250,1200,true),"shortcut North lease survives cleared nearby interaction lease");
    check(bmx_north_capture_live(1300,0,1250,true),"clearing shortcut leaves nearby bike North reservation intact");
    check(bmx_north_capture_live(1250,1300,1250,true)&&!bmx_north_capture_live(1250,1300,1300,true),
        "each North reservation expires independently and both expired release native input");
    check(!bmx_north_capture_live(1300,1400,1250,false),"dead overlay releases both North reservations");
    check(bmx_native_capture(false,false,false,false,false,bmx_north_capture_live(0,1300,1250,true))==BmxNativeCapture::interaction,
        "equipment shortcut uses existing exact North-only filtering");
    check(bmx_native_capture(false,true,false,false,false,true)==BmxNativeCapture::all&&
        bmx_native_capture(true,false,false,false,false,true)==BmxNativeCapture::none,
        "menus and bypass retain priority over equipment shortcut capture");
}
void playstation_interaction_preserves_every_other_byte() {
    using Kind=dingosdk::PlayStationPad;
    struct Transport {Kind kind;bool bluetooth;unsigned id,size;};
    constexpr std::array<Transport,10> formats{{
        {Kind::dualsense,false,1,64},{Kind::dualshock4,false,1,64},
        {Kind::dualsense,true,0x31,78},{Kind::dualshock4,true,0x11,78},
        {Kind::dualsense,true,1,10},{Kind::dualshock4,true,1,10},
        {Kind::dualsense,true,1,78},{Kind::dualshock4,true,1,78},
        {Kind::dualshock4,true,0x11,547},{Kind::dualsense,true,1,547}}};
    for(const auto format:formats)for(unsigned hat=0;hat<8;++hat) {
        std::array<std::uint8_t,551> memory{};memory.fill(0x5a);auto* bytes=memory.data()+2;
        bytes[0]=static_cast<std::uint8_t>(format.id);
        const unsigned offset=format.id==0x31?2:format.id==0x11?3:1;
        const bool sense=format.kind==Kind::dualsense&&(format.id==0x31||!format.bluetooth);
        const unsigned face=offset+(sense?7:4);bytes[face]=static_cast<std::uint8_t>(0xf0|hat);
        const auto original=memory;auto expected=memory;expected[face+2]&=0x7f;
        if(format.id==0x31||format.id==0x11) {
            const auto crc=report_crc(expected.data()+2,74);std::memcpy(expected.data()+2+74,&crc,sizeof(crc));
        }
        const auto before=dingosdk::parse_playstation_report(format.kind,format.bluetooth,bytes,format.size);
        check(before.has_value(),"interaction fixture uses supported exact Sony layouts");
        check(bmx_filter_playstation_report(format.kind,format.bluetooth,bytes,format.size,BmxNativeCapture::interaction),
            "all supported Sony wire/minimal/padded reports accept North-only interaction capture");
        const auto after=dingosdk::parse_playstation_report(format.kind,format.bluetooth,bytes,format.size);
        check(memory==expected,"Sony interaction changes only North and necessary wire CRC, including untouched sensors/touch/sequence/padded547 tail");
        check(after&&before&&after->buttons==(before->buttons&~0x8000u)&&
            after->left_x==before->left_x&&after->left_y==before->left_y&&after->right_x==before->right_x&&
            after->right_y==before->right_y&&after->left_trigger==before->left_trigger&&after->right_trigger==before->right_trigger,
            "North-only Sony capture leaves native walking/triggers/D-pad and every other parsed control unchanged");
        check(original[0]==memory[0]&&original.back()==memory.back(),"partial capture cannot escape received report bounds");
    }
}
}
int main() {
    native_ranges_and_keyboard_layout();
    fallback_and_pad_drive_the_same_mapper();
    streets_named_pad_tricks();
    menu_focus_and_capture_priority();
    independent_leases_and_bypass();
    native_dpad_capture_priority();playstation_dpad_reports();
    interaction_only_north_and_priority();independent_north_reservations();playstation_interaction_preserves_every_other_byte();
    if (failures) std::printf("%d BMX input sampling check(s) failed\n", failures);
    else std::printf("BMX input sampling/ownership checks passed\n");
    return failures ? 1 : 0;
}
