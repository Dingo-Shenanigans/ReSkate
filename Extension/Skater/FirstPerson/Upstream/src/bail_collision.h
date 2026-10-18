#pragma once
#include "Extension/Skater/FirstPerson/Upstream/vendor/Engine/Game/Skater/first_person_spring.h"
#include <optional>
#include <algorithm>
#include <cmath>
namespace jishsfirstpersonmod {
using CollisionPoint=first_person_vendor::first_person::Vec3;
struct CollisionSweep { bool valid{};float fraction{1}; };
// A native sweep, rather than a ground-height clamp. Initial overlap is outside
// this experiment's scope: the previous displayed position is the seed.
class BailCollision {
    CollisionPoint safe_{},stall_anchor_{};double time_{},stall_since_{};
    bool ready_{},blocked_{},stall_tracking_{};
public:
    static constexpr float radius=.10f,clearance=.01f;
    void reset() { *this={}; }
    bool blocked() const { return blocked_; }
    static bool owns_camera(bool bail,bool recovery) { return bail || recovery; }
    template<class Query> CollisionPoint resolve(CollisionPoint desired,CollisionPoint seed,double now,Query query,
                                                bool recovering=false,const CollisionPoint* normal_target=nullptr,double return_remaining=-1) {
        using namespace first_person_vendor::first_person;
        const auto scaled=[](CollisionPoint point,float factor) { for(auto& value:point) value*=factor;return point; };
        if(!std::isfinite(now)) { reset();return desired; }
        if(ready_ && (now<time_ || now-time_>.25 || length(subtract(desired,safe_))>5)) reset();
        if(!ready_) { safe_=seed;time_=now;ready_=true; }
        const float dt=static_cast<float>(std::clamp(now-time_,0.,.05));time_=now;
        const auto sweep=[&](CollisionPoint from,CollisionPoint to)->std::optional<CollisionPoint> {
            const auto delta=subtract(to,from);const float distance=length(delta);
            if(distance<.00001f) return from;
            const auto result=query(from,to,radius+clearance);
            if(!result.valid || !std::isfinite(result.fraction) || result.fraction<0 || result.fraction>1) return {};
            const float fraction=result.fraction<1 ? std::max(0.f,result.fraction-.002f/distance):1.f;
            return add(from,scaled(delta,fraction));
        };
        const auto recover_stall=[&]() {
            if(!recovering || !normal_target || length(subtract(*normal_target,safe_))<.10f) {
                stall_tracking_=false;return safe_;
            }
            if(!stall_tracking_ || length(subtract(safe_,stall_anchor_))>.002f) {
                stall_tracking_=true;stall_anchor_=safe_;stall_since_=now;return safe_;
            }
            if(now-stall_since_<.050) return safe_;
            // The ordinary controlled camera target is assumed clear. Query
            // toward the trapped position, stopping on the target's clear side.
            // Never interpolate through the embedded part of that segment.
            if(const auto escape=sweep(*normal_target,safe_)) {
                safe_=length(subtract(*escape,safe_))>.002f ? *escape:*normal_target;
                stall_tracking_=false;blocked_=true;
            }
            return safe_;
        };
        const auto direct=sweep(safe_,desired);
        if(!direct) { blocked_=true;stall_tracking_=false;return safe_; }
        const bool obstructed=length(subtract(*direct,desired))>.0001f;
        if(obstructed) {
            safe_=*direct;
            // Axis sweeps allow movement along floors/walls without needing an
            // unverified native hit-normal layout. Every intermediate move is swept.
            for(unsigned axis=0;axis<3;++axis) {
                auto slide=safe_;slide[axis]=desired[axis];
                if(const auto checked=sweep(safe_,slide)) safe_=*checked;
            }
            blocked_=true;return recover_stall();
        }
        auto next=desired;
        if(!blocked_) { safe_=desired;stall_tracking_=false;return safe_; }
        if(blocked_) {
            // Tighten the tail as the 100ms position handoff approaches, so a
            // successful return does not leave a lag for the release to snap.
            const float tau=recovering && return_remaining>=0 ? std::min(.03f,std::max(.001f,float(return_remaining)*.3f)):
                recovering ? .03f:.08f;
            const float alpha=dt>0 ? 1-std::exp(-dt/tau):0;
            next=add(safe_,scaled(subtract(desired,safe_),alpha));
        }
        if(const auto checked=sweep(safe_,next)) safe_=*checked;
        if(length(subtract(safe_,desired))<.001f) blocked_=false;
        return recover_stall();
    }
};
void initialize_bail_collision(std::uintptr_t base) noexcept;
void observe_collision_engine_thread() noexcept;
bool bail_collision_available() noexcept;
std::uintptr_t bail_collision_world() noexcept;
CollisionSweep native_bail_sweep(std::uintptr_t world,CollisionPoint from,CollisionPoint to,float radius) noexcept;
}
