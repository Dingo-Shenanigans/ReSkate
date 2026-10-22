#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dingosdk::bmx::visual::detail {
struct ReferenceTriangle { std::array<float,3> p[3]; unsigned part; };
// RSBMX001, u32 version/record stride/count, six float32 bounds, then records
// of u32 part and nine float32 coordinates. All fields are little endian.
inline std::vector<ReferenceTriangle> decode_geometry(std::span<const std::byte> bytes) {
    constexpr std::array<unsigned char,8> magic{'R','S','B','M','X','0','0','1'};
    if(bytes.size()<44)return {};
    for(std::size_t i=0;i<magic.size();++i)if(std::to_integer<unsigned char>(bytes[i])!=magic[i])return {};
    const auto u32=[&](std::size_t at){std::uint32_t value{};for(unsigned i=0;i<4;++i)value|=std::uint32_t(std::to_integer<unsigned char>(bytes[at+i]))<<(i*8);return value;};
    const auto f32=[&](std::size_t at){return std::bit_cast<float>(u32(at));};
    const auto count=u32(16);
    if(u32(8)!=1||u32(12)!=40||!count||count>100000||bytes.size()!=44+std::size_t(count)*40)return {};
    std::array<float,6> bounds{};
    for(std::size_t i=0;i<bounds.size();++i){bounds[i]=f32(20+i*4);if(!std::isfinite(bounds[i])||std::abs(bounds[i])>10)return {};}
    for(unsigned i=0;i<3;++i)if(bounds[i]>=bounds[i+3])return {};
    std::vector<ReferenceTriangle> result;result.reserve(count);
    for(std::uint32_t i=0;i<count;++i){
        const std::size_t at=44+std::size_t(i)*40;ReferenceTriangle triangle{};triangle.part=u32(at);
        if(triangle.part>5)return {};
        for(unsigned vertex=0;vertex<3;++vertex)for(unsigned axis=0;axis<3;++axis){
            const float value=f32(at+4+(vertex*3+axis)*4);
            if(!std::isfinite(value)||std::abs(value)>10||value<bounds[axis]-.0001f||value>bounds[axis+3]+.0001f)return {};
            triangle.p[vertex][axis]=value;
        }
        result.push_back(triangle);
    }
    return result;
}
}
