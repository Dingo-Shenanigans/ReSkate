#pragma once
#include <array>
#include <cmath>
#include <optional>
namespace dingosdk::trick_logger {
// The riding rig's local right axis stays horizontal during a front/backflip.
// Its horizontal perpendicular gives riding yaw without the forward axis's
// vertical singularity or its inverted 180-degree reversal. This uses the same
// verified body matrix as before; independent skateboard rotation is not added.
inline std::optional<double> riding_heading(const std::array<float,16>& body) {
    for(const auto value:body)if(!std::isfinite(value))return {};
    for(unsigned axis=0;axis<3;++axis) {
        const auto i=axis*4;const double norm=double(body[i])*body[i]+double(body[i+1])*body[i+1]+double(body[i+2])*body[i+2];
        if(norm<=.5 || norm>=1.5)return {};
    }
    if(std::hypot(body[0],body[2])<=.1)return {};
    return std::atan2(-double(body[2]),double(body[0]))*57.29577951308232;
}
}
