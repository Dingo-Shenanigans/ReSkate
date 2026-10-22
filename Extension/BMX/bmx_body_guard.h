#pragma once
#include "bmx_wheel_surface.h"
#include <array>
#include <cmath>

namespace dingosdk::bmx::body_guard {
using Vec=std::array<float,3>;
using Ray=wheel_surface::RaySample;
namespace math=wheel_surface::detail;
struct Result {
    bool available{},hit{},swept_crossing{};
    Vec point{},normal{};
    float separation{},closing_speed{};
    unsigned rays{};
};
// Bounded ray/plane evidence for impact classification. These rays do not
// replace native capsule collision or claim a convex/sphere sweep. Every normal
// uses current, non-collinear hits and a fresh normal-direction witness.
template<class Cast>
Result search(Vec center,float radius,Vec velocity,Vec previous_center,Vec previous_velocity,
              bool consecutive,Cast&& cast) noexcept {
    Result out;
    if(!math::finite(center)||!math::finite(velocity)||!std::isfinite(radius)||radius<.02f||radius>.35f||
       (consecutive&&(!math::finite(previous_center)||!math::finite(previous_velocity))))return out;
    try {
        bool failed=false;
        const auto ray=[&](Vec a,Vec b,Ray& hit) {
            ++out.rays;hit=cast(a,b);
            if(!hit.available) {failed=true;return false;}
            if(!hit.hit)return true;
            const auto expected=math::add(a,math::mul(math::sub(b,a),hit.fraction));
            const auto error=math::sub(expected,hit.point);
            if(!math::finite(hit.point)||!std::isfinite(hit.fraction)||hit.fraction<0||hit.fraction>1||math::dot(error,error)>.025*.025) {
                failed=true;return false;
            }
            return true;
        };
        const auto inspect=[&](Vec up,bool swept) {
            Vec lateral{},forward{};
            const Vec reference=std::abs(up[1])<.9f?Vec{0,1,0}:Vec{1,0,0};
            if(!math::unit(math::cross(reference,up),lateral)||!math::unit(math::cross(up,lateral),forward))return;
            const auto start=swept?previous_center:math::add(center,math::mul(up,radius+.15f));
            // Cover oblique planes down to the fit's .30 alignment gate; the
            // measured normal gap below, not this ray length, authorizes contact.
            const auto end=math::sub(center,math::mul(up,radius/.3f+.06f));
            Ray hit;if(!ray(start,end,hit)||!hit.hit)return;
            std::array<contact_patch::Sample,5> patch{};patch[0]={true,hit.point};
            const std::array<Vec,4> offsets{math::mul(lateral,.025f),math::mul(lateral,-.025f),
                math::mul(forward,.025f),math::mul(forward,-.025f)};
            for(unsigned i=0;i<4;++i) {
                Ray neighbor;if(!ray(math::add(start,offsets[i]),math::add(end,offsets[i]),neighbor))return;
                patch[i+1]={neighbor.hit,neighbor.point};
            }
            contact_patch::Tuning tuning;tuning.maximum_residual=.0025f;tuning.minimum_up_alignment=.3f;
            const auto fit=contact_patch::fit(patch,up,forward,lateral,tuning);
            if(!fit.available)return;
            const auto center_side=math::dot(math::sub(center,fit.point),fit.normal);
            const auto previous_center_side=consecutive?
                math::dot(math::sub(previous_center,fit.point),fit.normal):center_side;
            const auto gap=center_side-radius;
            // A sphere may already overlap before its center crosses the face.
            // Require a new center-side crossing, not prior sphere separation;
            // the exact historical segment must still hit the actual face below.
            const bool crossed=swept&&previous_center_side>0&&center_side<0;
            if(!std::isfinite(gap)||gap>.06||gap<-(crossed?1.5:.15))return;
            Ray witness;
            // The normal witness must pass through the guard center, otherwise
            // a finite lip's infinite fitted plane could falsely authorize a
            // collision beyond its actual edge. A real center-path crossing
            // instead requires that exact historical segment to hit the face.
            const auto witness_start=crossed?previous_center:math::add(center,math::mul(fit.normal,radius+.16f));
            const auto witness_end=crossed?center:math::sub(center,math::mul(fit.normal,radius+.06f));
            if(!ray(witness_start,witness_end,witness)||!witness.hit||
               std::abs(math::dot(math::sub(witness.point,fit.point),fit.normal))>.0025)return;
            const auto closing=std::max(-math::dot(velocity,fit.normal),
                consecutive?-math::dot(previous_velocity,fit.normal):-math::dot(velocity,fit.normal));
            if(!out.hit||crossed||gap<out.separation) {
                out.hit=true;out.point=witness.point;out.normal=fit.normal;out.separation=static_cast<float>(gap);
                out.closing_speed=static_cast<float>(closing);out.swept_crossing=crossed;
            }
        };
        inspect({0,1,0},false);
        Vec movement_up{};
        const auto incoming=consecutive?previous_velocity:velocity;
        if(math::dot(incoming,incoming)>.25&&math::unit(math::mul(incoming,-1),movement_up)&&movement_up[1]<.98f)
            inspect(movement_up,false);
        if(consecutive) {
            const auto displacement=math::sub(previous_center,center);
            const auto distance2=math::dot(displacement,displacement);
            if(distance2>.0004&&distance2<=2.25&&math::unit(displacement,movement_up))inspect(movement_up,true);
        }
        if(failed) {out.available=false;out.hit=false;return out;}
        out.available=true;return out;
    } catch(...) {return {};}
}
}
