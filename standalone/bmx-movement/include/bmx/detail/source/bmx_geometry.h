#pragma once
#include <array>
#include <cmath>
#include <string_view>

namespace bmx::detail::source::geometry {
inline constexpr double proposed_model_scale_factor = 0.7737670588235294;
using Vec = std::array<float,3>;
// Supplied BMX_sf.fbx, transformed declared-unit geometry. These are fitted
// geometry anchors, not named author joints or recovered physics parameters.
inline constexpr std::string_view source_sha256 =
    "00ce4d383a31b786837a0d229dcb8d651dc7580166aff8741308c202647caa2a";
inline constexpr double source_uniform_scale = (.4047481809231889)*proposed_model_scale_factor;
inline constexpr std::array<double,3> source_origin{0,-1.6979001667381863,.016188328673421237};
inline constexpr double wheel_radius = (.34)*proposed_model_scale_factor;
inline constexpr double wheelbase = (1.203956057050485)*proposed_model_scale_factor;
inline constexpr Vec rear_axle{float((0)*proposed_model_scale_factor),float((.3399312125684516f)*proposed_model_scale_factor),float((-.6019780285252407f)*proposed_model_scale_factor)};
inline constexpr Vec front_axle{float((0)*proposed_model_scale_factor),float((.33993130346462175f)*proposed_model_scale_factor),float((.6019780285252407f)*proposed_model_scale_factor)};
inline constexpr Vec bottom_bracket{float((0)*proposed_model_scale_factor),float((.4009181147792979f)*proposed_model_scale_factor),float((-.13677059002727915f)*proposed_model_scale_factor)};
inline constexpr float crank_radius = float((.24056151769963072f)*proposed_model_scale_factor); // measured spindle-to-BB distance
inline constexpr Vec steering_pivot{float((0)*proposed_model_scale_factor),float((.9602538008020781f)*proposed_model_scale_factor),float((.3685461188163388f)*proposed_model_scale_factor)};
inline constexpr Vec steering_axis{0,.9601195908265842f,-.279589648075517f};
// Retained tuning for the combined 106kg bike+rider, not a mesh-derived COM.
inline constexpr Vec com_from_root{0,.9f,-.05f};

struct Grip { Vec center,outward; float radius; };
// Conservative outer clearance of the measured central +/-25mm knurled band
// (max .01637766m), excluding the .01728m end flange. It is not a perfect tube.
inline constexpr Grip left_grip{{float((.33457504536745686f)*proposed_model_scale_factor),float((1.2319480593290573f)*proposed_model_scale_factor),float((.3406239453545827f)*proposed_model_scale_factor)},
    {.9426919343452176f,.11310901153299041f,-.31390805728828f},float((.0164f)*proposed_model_scale_factor)};
inline constexpr Grip right_grip{{float((-.33457504173973396f)*proposed_model_scale_factor),float((1.2319480453628473f)*proposed_model_scale_factor),float((.34062398104089797f)*proposed_model_scale_factor)},
    {-.9426919147801553f,.11310899923644252f,-.31390812047453476f},float((.0164f)*proposed_model_scale_factor)};
inline constexpr const Grip& grip(bool left) noexcept { return left?left_grip:right_grip; }

// Area-weighted centroids of the actual broad upper pedal faces. Pins protrude
// 1.78mm above these planes. The eight-part pack gives each platform a separate
// transform: translate with its spindle, retain its authored level orientation.
struct Pedal { Vec contact,normal,spindle; };
inline constexpr Pedal left_pedal{{float((.139242193856f)*proposed_model_scale_factor),float((.220121163795f)*proposed_model_scale_factor),float((.011273203601f)*proposed_model_scale_factor)},{0,1,0},
    {float((.084980725506f)*proposed_model_scale_factor),float((.211285091841f)*proposed_model_scale_factor),float((.011246842761f)*proposed_model_scale_factor)}};
inline constexpr Pedal right_pedal{{float((-.139242212706f)*proposed_model_scale_factor),float((.599275463263f)*proposed_model_scale_factor),float((-.284814700726f)*proposed_model_scale_factor)},{0,1,0},
    {float((-.084980725506f)*proposed_model_scale_factor),float((.590551321670f)*proposed_model_scale_factor),float((-.284787818885f)*proposed_model_scale_factor)}};
inline constexpr const Pedal& pedal(bool left) noexcept { return left?left_pedal:right_pedal; }
inline constexpr float ankle_from_sole = .09f; // retained native-rig ankle proxy, not a shoe mesh measurement
// Actual frame triangle/plane intersections at source-normalized Z=-.04m.
// Five adjacent sections measured the same straight top-tube axis.
inline constexpr Vec carry_tube_center{float((-.000000202f)*proposed_model_scale_factor),float((.731890937f)*proposed_model_scale_factor),float((-.04f)*proposed_model_scale_factor)};
inline constexpr Vec carry_tube_axis{0,.2569094143f,.9664354882f};
inline constexpr float carry_tube_radius=float((.0209f)*proposed_model_scale_factor);

inline Vec rotate_direction(Vec value,Vec axis,float angle) noexcept {
    // Rodrigues rotation normalizes the fixed fitted axis to avoid introducing
    // model scale from float-rounded source coefficients.
    const double length=std::sqrt(double(axis[0])*axis[0]+double(axis[1])*axis[1]+double(axis[2])*axis[2]);
    const std::array<double,3> n{axis[0]/length,axis[1]/length,axis[2]/length};
    const double c=std::cos(angle),s=std::sin(angle),projection=n[0]*value[0]+n[1]*value[1]+n[2]*value[2];
    return {static_cast<float>(value[0]*c+(n[1]*value[2]-n[2]*value[1])*s+n[0]*projection*(1-c)),
        static_cast<float>(value[1]*c+(n[2]*value[0]-n[0]*value[2])*s+n[1]*projection*(1-c)),
        static_cast<float>(value[2]*c+(n[0]*value[1]-n[1]*value[0])*s+n[2]*projection*(1-c))};
}
inline Vec rotate_steering(Vec point,float angle) noexcept {
    if(angle==0)return point;
    for(unsigned i=0;i<3;++i)point[i]-=steering_pivot[i];
    point=rotate_direction(point,steering_axis,angle);
    for(unsigned i=0;i<3;++i)point[i]+=steering_pivot[i];
    return point;
}
inline Vec wheel_hub(bool front,float steering_angle=0) noexcept {
    return front?rotate_steering(front_axle,steering_angle):rear_axle;
}
inline Vec crank_point(Vec point,float phase) noexcept {
    if(phase==0)return point;
    for(unsigned i=0;i<3;++i)point[i]-=bottom_bracket[i];
    point=rotate_direction(point,{1,0,0},phase);
    for(unsigned i=0;i<3;++i)point[i]+=bottom_bracket[i];
    return point;
}
inline Vec pedal_point(bool left,float phase) noexcept {
    const auto& platform=pedal(left);
    const auto rotated=crank_point(platform.spindle,phase);
    Vec point=platform.contact;
    for(unsigned i=0;i<3;++i)point[i]+=rotated[i]-platform.spindle[i];
    return point;
}
inline float parked_root_clearance(float pitch) noexcept {
    // Caller requires a finite mild slope from fresh local floor samples.
    // Plane-normal hub distance is root_clearance*cos(pitch)+local_axle_y.
    const double axle_y=(double(front_axle[1])+rear_axle[1])*.5;
    return static_cast<float>((wheel_radius-axle_y)/std::cos(pitch));
}
} // namespace bmx::detail::source::geometry
