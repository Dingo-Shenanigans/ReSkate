#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Presentation-only adaptation tuning, not recovered Streets posture values.
// The bridge supplies accepted native time and actual measured wheel support.
// No force, support, input, trick phase or scoring observation is changed here.
namespace bmx::detail::source::rider_posture {
using Vec=std::array<float,3>;
inline constexpr float radians_per_degree=.01745329251994329577f;
inline constexpr float maximum_counterpitch=25*radians_per_degree;
inline constexpr float counterpitch_rate=90*radians_per_degree;
struct State {float uphill{},counterpitch{};std::uint64_t serial{};};
struct Observation {
    double seconds{};std::uint64_t serial{};
    std::array<bool,2> supported{},normal_available{};
    std::array<Vec,2> normal{};
    Vec forward{0,0,1};
};
struct Result {
    bool valid{},advanced{},measured_plane{};
    State state;
    float measured_grade{},uphill_target{},counterpitch_target{};
};
inline float dot(Vec a,Vec b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool unit(Vec v) noexcept {
    for(float x:v)if(!std::isfinite(x))return false;
    const float n=dot(v,v);return n>.99f&&n<1.01f;
}
inline float smooth(float low,float high,float value) noexcept {
    const float t=std::clamp((value-low)/(high-low),0.f,1.f);return t*t*(3-2*t);
}
inline float slew(float previous,float target,double maximum) noexcept {
    return static_cast<float>(previous+std::clamp(double(target)-previous,-maximum,maximum));
}
inline Result step(State previous,const Observation& sample) noexcept {
    Result out;out.state=previous;
    if(!std::isfinite(previous.uphill)||previous.uphill<0||previous.uphill>1||
       !std::isfinite(previous.counterpitch)||previous.counterpitch<0||previous.counterpitch>maximum_counterpitch||
       !std::isfinite(sample.seconds)||sample.seconds<0||sample.seconds>.1||!sample.serial||
       !unit(sample.forward)||sample.serial<previous.serial)return out;
    out.valid=true;
    // Both the bridge and this pure policy refuse duplicate/paused integration.
    if(sample.seconds==0||sample.serial==previous.serial)return out;
    const float horizontal=std::hypot(sample.forward[0],sample.forward[2]);
    if(horizontal>.0001f&&sample.supported[0]&&sample.supported[1]&&
       sample.normal_available[0]&&sample.normal_available[1]&&
       unit(sample.normal[0])&&unit(sample.normal[1])&&
       sample.normal[0][1]>.7f&&sample.normal[1][1]>.7f&&
       dot(sample.normal[0],sample.normal[1])>=.9659258263f) {
        Vec normal{};for(unsigned i=0;i<3;++i)normal[i]=sample.normal[0][i]+sample.normal[1][i];
        const float length=std::sqrt(dot(normal,normal));for(auto& v:normal)v/=length;
        const Vec heading{sample.forward[0]/horizontal,0,sample.forward[2]/horizontal};
        out.measured_grade=std::atan2(-dot(normal,heading),normal[1]);out.measured_plane=true;
        out.uphill_target=smooth(3*radians_per_degree,20*radians_per_degree,
            std::clamp(out.measured_grade,0.f,30*radians_per_degree));
    }
    // Missing/disagreeing normals and one-wheel support only fade a prior cue.
    out.state.uphill=std::clamp(slew(previous.uphill,out.uphill_target,4*sample.seconds),0.f,1.f);
    const float phi=horizontal>.0001f?std::atan2(sample.forward[1],horizontal):0.f;
    out.counterpitch_target=out.state.uphill*std::clamp(std::max(phi,0.f),0.f,maximum_counterpitch);
    if(!out.measured_plane||out.uphill_target==0)out.counterpitch_target=std::min(out.counterpitch_target,previous.counterpitch);
    out.state.counterpitch=std::clamp(slew(previous.counterpitch,out.counterpitch_target,
        counterpitch_rate*sample.seconds),0.f,maximum_counterpitch);
    out.state.serial=sample.serial;out.advanced=true;return out;
}
} // namespace bmx::detail::source::rider_posture
