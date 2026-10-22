#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <span>

// Estimate a small surface patch through an actual center ray hit. Missing
// auxiliary hits can reduce confidence, but never move the contact across a lip.
namespace dingosdk::bmx::contact_patch {
using Vec = std::array<float,3>;
struct Sample { bool hit{}; Vec point{}; };
struct Tuning {
    float maximum_rise{.23f},maximum_distance{.34f},maximum_residual{.018f},minimum_up_alignment{.30f};
};
enum class Issue { ready,center_clear,insufficient_patch,discontinuous,grazing,invalid };
struct Result {
    bool available{};
    Vec point{},normal{};
    unsigned used{};
    float residual{},up_alignment{};
    Issue issue{Issue::invalid};
};
namespace detail {
inline double dot(Vec a,Vec b) noexcept {double x=0;for(unsigned i=0;i<3;++i)x+=double(a[i])*b[i];return x;}
inline Vec sub(Vec a,Vec b) noexcept {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline bool finite(Vec a) noexcept {for(float x:a)if(!std::isfinite(x)||std::abs(x)>1e6)return false;return true;}
inline bool unit(Vec a) noexcept {return finite(a)&&std::abs(dot(a,a)-1)<.001;}
}
inline Result fit(std::span<const Sample> samples,Vec up,Vec forward,Vec lateral,Tuning tuning={}) noexcept {
    Result out;
    if(samples.empty()||samples.size()>9||!detail::unit(up)||!detail::unit(forward)||!detail::unit(lateral)||
       std::abs(detail::dot(up,forward))>.001||std::abs(detail::dot(up,lateral))>.001||std::abs(detail::dot(forward,lateral))>.001)
        return out;
    for(float v:{tuning.maximum_rise,tuning.maximum_distance,tuning.maximum_residual,tuning.minimum_up_alignment})
        if(!std::isfinite(v)||v<=0||v>1)return out;
    if(!samples.front().hit) {out.issue=Issue::center_clear;return out;}
    if(!detail::finite(samples.front().point))return out;
    out.point=samples.front().point;out.used=1;
    struct Local {double u{},v{},height{};};
    std::array<Local,8> locals{};unsigned count=0;
    double uu=0,uv=0,vv=0,uh=0,vh=0;
    for(std::size_t i=1;i<samples.size();++i) {
        if(!samples[i].hit)continue;
        if(!detail::finite(samples[i].point))return out;
        const auto delta=detail::sub(samples[i].point,out.point);
        const auto height=detail::dot(delta,up);
        // An adjacent floor/wall is not evidence for the center's patch.
        if(std::abs(height)>tuning.maximum_rise||detail::dot(delta,delta)>double(tuning.maximum_distance)*tuning.maximum_distance)continue;
        const auto u=detail::dot(delta,forward),v=detail::dot(delta,lateral);
        locals[count++]={u,v,height};uu+=u*u;uv+=u*v;vv+=v*v;uh+=u*height;vh+=v*height;
    }
    out.used+=count;
    const double determinant=uu*vv-uv*uv;
    if(count<2||!std::isfinite(determinant)||determinant<1e-8) {out.issue=Issue::insufficient_patch;return out;}
    const double along=(uh*vv-vh*uv)/determinant,across=(vh*uu-uh*uv)/determinant;
    double residual=0;
    for(unsigned i=0;i<count;++i)residual=std::max(residual,std::abs(locals[i].height-along*locals[i].u-across*locals[i].v));
    out.residual=static_cast<float>(residual);
    if(!std::isfinite(residual)||residual>tuning.maximum_residual) {out.issue=Issue::discontinuous;return out;}
    double square=0;
    for(unsigned i=0;i<3;++i) {out.normal[i]=static_cast<float>(up[i]-along*forward[i]-across*lateral[i]);square+=double(out.normal[i])*out.normal[i];}
    if(!std::isfinite(square)||square<1e-8)return out;
    for(auto& x:out.normal)x=static_cast<float>(x/std::sqrt(square));
    out.up_alignment=static_cast<float>(detail::dot(out.normal,up));
    if(out.up_alignment<tuning.minimum_up_alignment) {out.issue=Issue::grazing;return out;}
    out.available=true;out.issue=Issue::ready;return out;
}
}
