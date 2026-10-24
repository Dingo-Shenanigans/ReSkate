#pragma once
#include <array>
#include <cmath>
#include <string>
#include <string_view>

// The pose a native flag placement chose, and the assets a plugin mode's flag uses.
namespace dingosdk::multiplayer::native_flag {
using Facing = std::array<float,9>;
inline constexpr Facing default_facing{1,0,0,0,1,0,0,0,1};
inline bool valid_facing(const Facing& facing) {
    for(float n:facing)if(!std::isfinite(n) || std::abs(n)>1.01f)return false;
    for(unsigned row=0;row<3;++row) {
        float length{};for(unsigned i=0;i<3;++i)length+=facing[row*3+i]*facing[row*3+i];
        if(std::abs(length-1.f)>.03f)return false;
        for(unsigned other=0;other<row;++other) {
            float dot{};for(unsigned i=0;i<3;++i)dot+=facing[row*3+i]*facing[other*3+i];
            if(std::abs(dot)>.03f)return false;
        }
    }
    const auto determinant=facing[0]*(facing[4]*facing[8]-facing[5]*facing[7])-
        facing[1]*(facing[3]*facing[8]-facing[5]*facing[6])+facing[2]*(facing[3]*facing[7]-facing[4]*facing[6]);
    return determinant>.97f;
}
inline std::array<float,16> spawn_transform(const std::array<float,3>& position,const Facing& facing) {
    return {facing[0],facing[1],facing[2],0,facing[3],facing[4],facing[5],0,
        facing[6],facing[7],facing[8],0,position[0],position[1],position[2],1};
}
inline Facing transform_facing(const std::array<float,16>& transform) {
    return {transform[0],transform[1],transform[2],transform[4],transform[5],transform[6],transform[8],transform[9],transform[10]};
}
inline float facing_yaw(const Facing& facing) {return std::atan2(facing[6],facing[8])*180.f/3.14159265f;}
inline Facing yaw_facing(float degrees) {
    const float radians=degrees*3.14159265f/180.f,co=std::cos(radians),si=std::sin(radians);
    return {co,0,-si,0,1,0,si,0,co};
}
// CreateThrowdownQueueParams.SpawnPoint is XYZ plus a yaw in radians.
// TeleportLocation is only a Vec3; treating it as a transform discards the
// facing chosen in the native placement editor and leaves an identity spawn.
inline std::array<float,16> native_spawn_transform(const std::array<float,4>& spawn) {
    const float co=std::cos(spawn[3]),si=std::sin(spawn[3]);
    return spawn_transform({spawn[0],spawn[1],spawn[2]},Facing{co,0,-si,0,1,0,si,0,co});
}

// A mode's `flag` (custom_modes Mode::flag) is a short name ("duel": InstanceId ReSkate.duel,
// preview World/ReSkate/Throwdowns/Flags/duel_Placement) or the preview prefab's path, whose last
// segment less "_Placement" names the InstanceId. Empty: the stock Spot Battle flag and preview.
struct FlagAssets {
    std::string instance, preview; // both empty: stock
};
inline FlagAssets flag_assets(std::string_view flag) {
    if(flag.empty())return {};
    if(flag.find('/')==std::string_view::npos)
        return {"ReSkate."+std::string(flag),"World/ReSkate/Throwdowns/Flags/"+std::string(flag)+"_Placement"};
    auto name=flag.substr(flag.rfind('/')+1);
    constexpr std::string_view suffix="_Placement";
    if(name.size()>suffix.size() && name.ends_with(suffix))name.remove_suffix(suffix.size());
    return {name.empty()?std::string{}:"ReSkate."+std::string(name),std::string(flag)};
}
}
