#pragma once
#include "bmx_standalone_shadow.h"
#include "Engine/Game/BMX/bmx_native_coupled_policy.h"
#include <bmx/suspension.hpp>
#include <optional>

// Portable transaction adapter. This file performs no native calls, integrates
// no bodies and adopts no forces. The caller owns all native setup and queries.
namespace dingosdk::bmx::standalone_coupled {
namespace native=::dingosdk::bmx::native_coupled::policy;
using Body=::bmx::Body;
struct Observation {
    native::Epoch epoch;
    std::array<Body,2> bodies; // model orientation, actual principal moments and principal-to-model quaternion
    std::array<float,2> maximum_angular_speed{}; // actual native998 readback
    std::array<bool,2> integrates_angular_accumulator{}; // actual A68 bit readback
    std::array<chassis_control::ContactSample,2> contacts;
};
enum class Issue {idle,ready,waiting_for_producer,disabled,invalid_epoch,discontinuity,
    duplicate,invalid_body,invalid_output,output_limit,receipt_mismatch,producer_failed};
struct Evaluation {
    bool valid{},inertia_update_required{};
    Issue issue=Issue::idle;
    native::Epoch epoch;
    native::Command command;
    ::bmx::CoupledOutput output;
};
inline bool same_identity(const native::Epoch& a,const native::Epoch& b) noexcept {
    return a.owner_world==b.owner_world&&a.bodies==b.bodies&&a.token==b.token;
}
inline bool approximately_equal(double a,double b,double tolerance=1e-5) noexcept {
    return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance*std::max({1.,std::abs(a),std::abs(b)});
}
inline bool same_inertia(const Body& a,const Body& b) noexcept {
    for(unsigned i=0;i<3;++i)if(!approximately_equal(a.inertia_body[i],b.inertia_body[i]))return false;
    double dot{};for(unsigned i=0;i<4;++i)dot+=a.inertia_orientation[i]*b.inertia_orientation[i];
    return approximately_equal(std::abs(dot),1.);
}
// Explicit60Hz adaptation. The supplied100Hz reference15rad/s property is
// retained only behind BOTH actual native998/A68 observations below. Matching
// the property does not establish identical native/reference gyro integration.
inline ::bmx::CoupledProfile native60_profile() noexcept {
    auto p=::bmx::streets_observed_v2();p.movement.simulation.seconds=native::native_seconds;return p;
}
inline bool body_matches_role(const Body& b,unsigned role) noexcept {
    if(role>=2||!::bmx::detail::source::tire::valid(b)||!approximately_equal(b.mass,native::v2_roles[role].mass))return false;
    double dot{};for(unsigned i=0;i<4;++i)dot+=b.inertia_orientation[i]*native::v2_roles[role].principal_to_model[i];
    if(!approximately_equal(std::abs(dot),1.))return false;
    // Spring inertia is deliberately changed by the observed actuation. Bike
    // principal moments remain fixed in this selected-rig reduction.
    if(role==0)for(unsigned i=0;i<3;++i)if(!approximately_equal(b.inertia_body[i],native::v2_roles[0].principal_moments[i]))return false;
    return true;
}
inline bool role_command(const ::bmx::Vec3& force,const ::bmx::Vec3& torque,
    const ::bmx::Vec3& hop,const ::bmx::Vec3& angular,double mass,
    const ::bmx::Vec3& wanted_gravity,const ::bmx::Vec3& engine_gravity,double h,native::RoleCommand& out) noexcept {
    if(!standalone_shadow::finite_vector(force)||!standalone_shadow::finite_vector(torque)||
       !standalone_shadow::finite_vector(hop)||!standalone_shadow::finite_vector(angular)||
       !standalone_shadow::finite_vector(wanted_gravity)||!standalone_shadow::finite_vector(engine_gravity)||
       !std::isfinite(mass)||mass<=0||!approximately_equal(h,native::native_seconds,1e-8))return false;
    ::bmx::Vec3 f{},combined_f{},combined_t{};
    for(unsigned i=0;i<3;++i){
        // Native gravity remains enabled. Apply only the difference ONCE per
        // actual role; do not add the complete source gravity a second time.
        f[i]=force[i]+mass*(wanted_gravity[i]-engine_gravity[i]);
        combined_f[i]=f[i]+hop[i]/h;combined_t[i]=torque[i]+angular[i]/h;
    }
    if(!standalone_shadow::finite_vector(f)||!standalone_shadow::finite_vector(combined_f)||!standalone_shadow::finite_vector(combined_t)||
       standalone_shadow::norm(f)>40000||standalone_shadow::norm(combined_f)>40000||
       standalone_shadow::norm(torque)>4000||standalone_shadow::norm(combined_t)>4000||
       standalone_shadow::norm(hop)>600)return false;
    for(unsigned i=0;i<3;++i){
        out.force[i]=static_cast<float>(f[i]);out.torque[i]=static_cast<float>(torque[i]);
        out.hop_impulse[i]=static_cast<float>(hop[i]);out.angular_impulse[i]=static_cast<float>(angular[i]);
    }
    return true;
}
class Controller {
public:
    explicit Controller(::bmx::Vec3 actual_engine_gravity={0,-9.8,0}) noexcept:
        engine_gravity_(actual_engine_gravity),movement_(native60_profile()){}
    void reset() noexcept {
        movement_.reset();pending_.reset();last_={};have_epoch_=false;hop_part_=0;issue_=Issue::idle;
    }
    // Cancels controls without forgetting the high-water epoch. Pausing cannot
    // replay a previously consumed observation with a different raw input.
    void invalidate() noexcept {movement_.invalidate_sample();pending_.reset();issue_=Issue::idle;}
    Issue issue() const noexcept{return issue_;}
    bool pending() const noexcept{return pending_.has_value();}
    // Post observation contacts are deliberately not consumed here. A genuine
    // next epoch commits only the transaction; evaluate() then consumes that
    // next epoch once with its independently obtained current wheel queries.
    bool acknowledge(const native::Receipt& receipt,const Observation& actual_post) noexcept {
        if(!pending_){issue_=Issue::receipt_mismatch;return false;}
        const auto& p=*pending_;
        if(receipt.epoch!=p.evaluation.epoch||receipt.token!=p.evaluation.epoch.token||receipt.serial!=p.evaluation.epoch.serial||
           receipt.hop_part!=p.evaluation.command.hop_part){fail(Issue::receipt_mismatch);return false;}
        if(!native::valid_epoch(actual_post.epoch)||!same_identity(actual_post.epoch,p.evaluation.epoch)){
            fail(Issue::invalid_epoch);return false;
        }
        const bool same_epoch=actual_post.epoch==p.evaluation.epoch;
        const bool successor=p.evaluation.epoch.serial!=std::numeric_limits<std::uint64_t>::max()&&
            actual_post.epoch.serial==p.evaluation.epoch.serial+1&&
            actual_post.epoch.seconds==p.evaluation.epoch.seconds&&
            actual_post.epoch.time==p.evaluation.epoch.time+double(p.evaluation.epoch.seconds);
        if(!same_epoch&&!successor){fail(Issue::discontinuity);return false;}
        if(!observed_bodies_valid(actual_post)){fail(Issue::invalid_body);return false;}
        if(receipt.state==native::ReceiptState::queued){issue_=Issue::waiting_for_producer;return false;}
        if(receipt.state!=native::ReceiptState::submitted||receipt.attempted!=15||receipt.completed!=15||
           receipt.inertia_required!=2||receipt.inertia_attempted!=2||receipt.inertia_completed!=2||!receipt.inertia_before_wrenches||
           !verified_inertia(receipt.verified_principal_inertia[1],p.evaluation.command.roles[1].target_principal_inertia)){
            fail(Issue::producer_failed);return false;
        }
        if(same_epoch){issue_=Issue::waiting_for_producer;return false;}
        // Receipt proof can precede visibility of target inertia in another
        // same-epoch observation. Require actual targetI only at the genuine
        // successor, never substitute the predicted rider Body for that read.
        if(!same_inertia(actual_post.bodies[1],p.evaluation.output.rider)){
            fail(Issue::producer_failed);return false;
        }
        // Keep last_ at the pending before epoch: actual_post remains available
        // for exactly one subsequent evaluate() after this successful commit.
        movement_=p.trial;pending_.reset();issue_=Issue::ready;return true;
    }
    Evaluation evaluate(const RawInput& input,const Observation& o) noexcept {
        Evaluation e;e.epoch=o.epoch;
        if(pending_){e.issue=issue_=Issue::waiting_for_producer;return e;}
        if(!native::valid_epoch(o.epoch)){e.issue=issue_=Issue::invalid_epoch;movement_.invalidate_sample();return e;}
        if(have_epoch_&&!same_identity(last_,o.epoch)){
            movement_.reset();have_epoch_=false;hop_part_=0;
        }
        if(have_epoch_){
            if(o.epoch.serial==last_.serial&&o.epoch.time==last_.time){e.issue=issue_=Issue::duplicate;return e;}
            if(last_.serial==std::numeric_limits<std::uint64_t>::max()||o.epoch.serial!=last_.serial+1||
               std::abs(o.epoch.time-last_.time-o.epoch.seconds)>1e-7){
                if(o.epoch.serial>last_.serial&&o.epoch.time>=last_.time)last_=o.epoch;
                e.issue=issue_=Issue::discontinuity;movement_.invalidate_sample();return e;
            }
        }
        last_=o.epoch;have_epoch_=true;
        if(!observed_bodies_valid(o)){
            e.issue=issue_=Issue::invalid_body;movement_.invalidate_sample();return e;
        }
        auto trial=movement_;
        const std::array<::bmx::ContactSample,2> contacts{standalone_shadow::convert_contact(o.contacts[0]),standalone_shadow::convert_contact(o.contacts[1])};
        auto output=trial.step(standalone_shadow::convert_input(input),o.bodies[0],o.bodies[1],contacts,o.epoch.token);
        if(!output.valid||!output.converged||!body_matches_role(output.rider,1)){
            e.issue=issue_=Issue::invalid_output;movement_.invalidate_sample();return e;
        }
        const auto& wanted=trial.profile().movement.tuning.gravity;
        if(!trial.profile().coupled.chassis_gravity||!trial.profile().coupled.rider_gravity||
           !role_command(output.bike.force,output.bike.torque,output.bike.hop_impulse,{},o.bodies[0].mass,wanted,engine_gravity_,o.epoch.seconds,e.command.roles[0])||
           !role_command(output.rider_force,output.rider_torque,output.rider_hop_impulse,output.rider_angular_impulse,o.bodies[1].mass,wanted,engine_gravity_,o.epoch.seconds,e.command.roles[1])){
            e.issue=issue_=Issue::output_limit;movement_.invalidate_sample();return e;
        }
        for(unsigned i=0;i<3;++i)e.command.roles[1].target_principal_inertia[i]=static_cast<float>(output.rider.inertia_body[i]);
        const bool hop=standalone_shadow::norm(output.bike.hop_impulse)>0||standalone_shadow::norm(output.rider_hop_impulse)>0;
        if(hop){
            if(hop_part_==std::numeric_limits<std::uint64_t>::max()){e.issue=issue_=Issue::output_limit;movement_.invalidate_sample();return e;}
            e.command.hop_part=++hop_part_;
        }
        if(!native::prepare(e.command,o.epoch.seconds).valid){e.issue=issue_=Issue::output_limit;movement_.invalidate_sample();return e;}
        e.valid=true;e.issue=issue_=Issue::ready;e.output=output;
        e.inertia_update_required=!same_inertia(o.bodies[1],output.rider);
        pending_.emplace(Pending{trial,e});return e;
    }
private:
    struct Pending {::bmx::CoupledMovement trial;Evaluation evaluation;};
    ::bmx::Vec3 engine_gravity_;
    ::bmx::CoupledMovement movement_;
    std::optional<Pending> pending_;
    native::Epoch last_;
    bool have_epoch_{};
    std::uint64_t hop_part_{};
    Issue issue_=Issue::idle;
    static bool observed_bodies_valid(const Observation& o) noexcept {
        return body_matches_role(o.bodies[0],0)&&body_matches_role(o.bodies[1],1)&&
            o.integrates_angular_accumulator[0]&&o.integrates_angular_accumulator[1]&&
            o.maximum_angular_speed[0]==15.f&&o.maximum_angular_speed[1]==15.f;
    }
    static bool verified_inertia(const native::Vec& actual,const native::Vec& expected) noexcept {
        for(unsigned i=0;i<3;++i)if(!approximately_equal(actual[i],expected[i]))return false;return true;
    }
    void fail(Issue reason) noexcept {movement_.invalidate_sample();pending_.reset();issue_=reason;}
};
}
