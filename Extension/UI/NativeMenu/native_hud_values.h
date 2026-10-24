#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <format>
#include <string>
#include <array>
#include <optional>

namespace dingosdk::multiplayer::menu_data {
// The supported desktop gameplay screen lays out inside a 90% safe frame.
// Our anchors belong to that frame, so a small positive anchor alone still
// leaves the outer 5% margin visible. Convert only our screen-edge anchors;
// stock screens, centered notices and the stable HUD presenters stay intact.
inline constexpr float native_hud_screen_anchor(float screen) noexcept {
    return (screen-.05f)/.90f;
}
inline constexpr float native_hud_side_center=.60f;
// Rime's start/end offsets are opposing insets. Equal values resize a
// rectangle; opposite values translate it without changing its bounds.
inline constexpr std::array<float,2> native_hud_translation(float offset) noexcept {
    return {offset,-offset};
}
inline constexpr float native_hud_shortcut_width=672.f;
inline constexpr float native_hud_shortcut_edge=.998f;
// Approximate brush-font advance, including padding; round to an authored
// native frame. UTF-8 continuation bytes do not inflate a gamertag's width.
inline unsigned native_hud_name_width(std::string_view name) {
    unsigned advance=32;
    for(const unsigned char ch:name) {
        if((ch&0xc0)==0x80)continue;
        advance+=(ch=='I' || ch=='i' || ch=='l' || ch==' ' || ch=='.')?12:
            (ch=='W' || ch=='M' || ch=='w' || ch=='m' || ch>=0x80)?36:26;
    }
    return std::clamp((advance+31u)/32u*32u,224u,1280u);
}
struct HudHeaderGeometry {
    unsigned frame_width,content_width,title_width,icon=64,content_height=80,padding=48;
    float badge_y_shift{},title_y_shift{};
    static constexpr unsigned height=112,text_height=80,gap=24;
};
inline std::string native_hud_title_text(std::string_view title) {
    std::string result(title);for(auto& character:result)if(character>='a' && character<='z')character=char(character-'a'+'A');return result;
}
inline HudHeaderGeometry native_hud_header_geometry(std::string_view title) {
    // Brush title advances are distinct from H52 player-name cells. Reserve a
    // small font allowance, then fit the authored frame to this title's row.
    // Short names must not inherit a scoreboard-wide minimum background.
    unsigned advance=12;
    for(const unsigned char ch:native_hud_title_text(title)) {
        if((ch&0xc0)==0x80)continue;
        if(ch==' ' || ch=='.')advance+=16;
        else if(ch=='-' || ch=='t' || ch=='f' || ch=='r')advance+=24;
        else if(ch=='I' || ch=='i' || ch=='l' || ch=='j')advance+=18;
        else if(ch=='W' || ch=='M')advance+=58;
        else if(ch=='w' || ch=='m' || ch>=0x80)advance+=48;
        else if(ch>='a' && ch<='z')advance+=34;
        else if(ch=='E' || ch=='F' || ch=='T' || ch=='P' || ch=='S')advance+=40;
        else advance+=44;
    }
    const auto scaled=(advance*39u+31u)/32u; // Native D78 brush title, with a conservative width allowance.
    const auto text=std::clamp((scaled+7u)/8u*8u,144u,1600u);
    const unsigned icon=64u;
    const auto content=text+icon+HudHeaderGeometry::gap;
    const auto frame=(std::max(320u,content+96u)+31u)/32u*32u;
    return {frame,content,text,icon,80u};
}
// A header with a document's icon size and frame width (0: the measured default). A larger icon's
// paint baseline sits higher than the 64 px one's; a fixed frame keeps 120 px either side of its
// icon and title, the brush title fitted inside it (a 704 px frame holds a 344 px title beside a
// 96 px icon, as a captured mode header did).
inline HudHeaderGeometry native_hud_header_geometry(std::string_view title,unsigned icon_size,unsigned frame_width) {
    auto g=native_hud_header_geometry(title);
    if(!icon_size && !frame_width)return g;
    if(icon_size) {
        g.icon=icon_size;g.content_height=std::max(80u,icon_size);
        if(icon_size>64u) {g.badge_y_shift=-6.f;g.title_y_shift=-2.f;}
    }
    if(frame_width) {
        const auto used=240u+g.icon+HudHeaderGeometry::gap;
        if(frame_width>used)g.title_width=std::min(g.title_width,frame_width-used);
    }
    g.content_width=g.icon+HudHeaderGeometry::gap+g.title_width;
    g.frame_width=frame_width?std::max(frame_width,g.content_width):(std::max(320u,g.content_width+96u)+31u)/32u*32u;
    g.padding=(g.frame_width-g.content_width)/2u;
    return g;
}
// A clock as the native HUD shows it: mm:ss.mmm.
inline std::string native_hud_clock_text(std::uint64_t milliseconds) {
    return std::format("{:02}:{:02}.{:03}",milliseconds/60000,(milliseconds/1000)%60,milliseconds%1000);
}
// A published clock keeps advancing from the time it was sampled, so a plugin
// that publishes less often than a frame still shows a running clock. A clock
// counting up never passes its cap; one counting down stops at zero.
inline std::uint64_t native_hud_clock_elapsed(std::uint64_t observed,std::uint64_t sampled,
        std::uint64_t now,bool running,std::uint64_t cap) {
    const auto extra=running && sampled && now>sampled?now-sampled:0;
    if(!cap)return observed+extra;
    return observed>=cap?cap:observed+std::min(extra,cap-observed);
}
inline std::uint64_t native_hud_clock_remaining(std::uint64_t observed,std::uint64_t sampled,
        std::uint64_t now,bool running) noexcept {
    const auto extra=running && sampled && now>sampled?now-sampled:0;
    return extra>=observed?0:observed-extra;
}
// The native compass strip: ahead is its centre, +/-90 degrees either end of
// its 1184 units; a point behind stays at the nearer end. `camera` is the world
// matrix (rows: right, up, back, position).
inline constexpr float native_hud_compass_icon=96.f;
inline constexpr float native_hud_compass_center_y=209.f;
inline std::optional<float> native_hud_compass_offset(const std::array<float,3>& target,const std::array<float,16>& camera) {
    const float dx=target[0]-camera[12],dz=target[2]-camera[14];
    const float right=dx*camera[0]+dz*camera[2],forward=-dx*camera[8]-dz*camera[10];
    if(!std::isfinite(right) || !std::isfinite(forward) || std::hypot(camera[8],camera[10])<.001f)return {};
    constexpr float reach=(1184.f-native_hud_compass_icon)*.5f;
    return std::clamp(std::atan2(right,forward)/(3.14159265f*.5f),-1.f,1.f)*reach;
}
// A toast's opacity: in over 180 ms, held, out over 300 ms.
inline float native_hud_toast_alpha(std::uint64_t now,std::uint64_t shown,std::uint64_t duration) noexcept {
    if(!shown || now<shown)return 0.f;
    const auto age=now-shown,hold=std::max<std::uint64_t>(duration,480);
    if(age<180)return static_cast<float>(age)/180.f;
    if(age<hold-300)return 1.f;
    return age<hold?static_cast<float>(hold-age)/300.f:0.f;
}
inline unsigned native_hud_value_width(std::string_view text) {
    unsigned advance=40;
    for(const unsigned char ch:text) {
        if((ch&0xc0)==0x80)continue;
        advance+=(ch==':' || ch=='.' || ch==' ')?12:(ch=='W' || ch=='M' || ch>=0x80)?36:26;
    }
    return std::clamp((advance+31u)/32u*32u,224u,640u);
}
// CountdownViewModel stores whole seconds as Uint64. The match clock stores
// milliseconds. Round up so 2.9 seconds displays 3, and GO occurs only at zero.
constexpr std::uint64_t native_hud_seconds(std::uint64_t milliseconds) {
    return milliseconds / 1000 + (milliseconds % 1000 != 0);
}
inline unsigned native_slider_choice(float normalized,unsigned minimum,unsigned maximum,unsigned step) {
    if(!std::isfinite(normalized))return minimum;
    const auto steps=(maximum-minimum)/step;
    const auto index=static_cast<unsigned>(std::round(std::clamp(normalized,0.f,1.f)*steps));
    return minimum+std::min(index,steps)*step;
}
inline float native_slider_position(unsigned choice,unsigned minimum,unsigned maximum) {
    if(maximum<=minimum)return 0.f;
    return float(std::clamp(choice,minimum,maximum)-minimum)/float(maximum-minimum);
}
} // namespace dingosdk::multiplayer::menu_data
