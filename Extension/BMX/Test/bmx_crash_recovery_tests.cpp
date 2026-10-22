#include "Extension/BMX/bmx_crash_recovery.h"
#include <cstdlib>
#include <iostream>

namespace {
namespace c=dingosdk::bmx::crash_recovery;
void check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
c::Input input() {
    c::Input value;value.expected={0x10,0x20,0x30,0x40,0x50,0x60,7,0,0};value.current=value.expected;
    value.owner_valid=value.body_valid=value.native_standable=value.native_offboard=true;
    value.native_started=0;value.native_now=.6;value.crash_serial=100;value.current_serial=136;
    value.bike_root={627,199,1062};value.rider_ground=value.bike_root;return value;
}
void eligible_boundaries() {
    auto value=input();check(c::evaluate(value).eligible,"fresh standing offboard owner beside the settled bike is eligible at .6 native seconds");
    value.linear_velocity={1,0,0};check(c::evaluate(value).eligible,"one metre per second is inclusive");
    value.linear_velocity[0]=std::nextafter(1.f,2.f);check(c::evaluate(value).issue==c::Issue::moving,"next representable speed above one rejects");
    value=input();value.bike_root={0,0,0};value.rider_ground={1.8f,1.5f,0};
    check(c::evaluate(value).eligible,"horizontal1.8 and vertical1.5 boundaries are inclusive after rider-root correction");
    value.rider_ground[0]=std::nextafter(1.8f,2.f);check(c::evaluate(value).issue==c::Issue::too_far_horizontal,"horizontal distance cannot exceed1.8m");
    value.rider_ground[0]=0;value.rider_ground[1]=std::nextafter(1.5f,2.f);
    check(c::evaluate(value).issue==c::Issue::too_far_vertical,"vertical distance cannot exceed1.5m");
    value.rider_ground={-1.2f,-1.4f,-1.2f};check(c::evaluate(value).eligible,"signed coordinates use radial horizontal distance and absolute vertical distance");
    value.rider_ground={1.3f,0,1.3f};check(!c::evaluate(value).eligible,"per-axis proximity cannot substitute for horizontal radius");
    value=input();value.linear_velocity={.8f,.8f,0};check(c::evaluate(value).issue==c::Issue::moving,"vertical velocity contributes to total settling speed");
}
void owner_and_native_state() {
    auto value=input();value.owner_valid=false;check(c::evaluate(value).issue==c::Issue::owner_unavailable,"unavailable native owner rejects");
    value=input();value.body_valid=false;check(c::evaluate(value).issue==c::Issue::body_unavailable,"unavailable current body rejects");
    for(unsigned field=0;field<9;++field){value=input();
        switch(field){case 0:++value.current.client;break;case 1:++value.current.entity;break;case 2:++value.current.core;break;
        case 3:++value.current.context;break;case 4:++value.current.rig;break;case 5:++value.current.world;break;
        case 6:++value.current.body_token;break;case 7:++value.current.body_index;break;default:++value.current.body_generation;break;}
        check(c::evaluate(value).issue==c::Issue::owner_changed,"every rider/world/token/body-generation identity field is pinned");
    }
    value=input();value.current.world=value.expected.world=0;check(c::evaluate(value).issue==c::Issue::invalid_identity,"matching null worlds never establish ownership");
    value=input();value.current.body_token=value.expected.body_token=0;check(!c::evaluate(value).eligible,"matching zero tokens reject");
    value=input();value.current.body_index=value.expected.body_index=0xffffffffU;check(!c::evaluate(value).eligible,"invalid body index sentinel rejects");
    value=input();value.native_standable=false;check(c::evaluate(value).issue==c::Issue::not_standable,"recover only after native standable state");
    value=input();value.native_offboard=false;check(c::evaluate(value).issue==c::Issue::not_offboard,"skateboard ownership cannot recover as offboard bike");
}
void native_time() {
    auto value=input();value.native_now=std::nextafter(.6,0.);check(c::evaluate(value).issue==c::Issue::settling,"native settling duration cannot round up early");
    value=input();value.native_started=1;value.native_now=.9;check(c::evaluate(value).issue==c::Issue::invalid_native_clock,"native time reversal rejects");
    value=input();value.current_serial=value.crash_serial;check(!c::evaluate(value).eligible,"a repeated crash step cannot satisfy elapsed time");
    value=input();value.current_serial=value.crash_serial-1;check(!c::evaluate(value).eligible,"serial reversal rejects despite claimed elapsed time");
    value=input();value.crash_serial=0;check(!c::evaluate(value).eligible,"missing crash native clock rejects");
    value=input();value.native_started=-1;check(!c::evaluate(value).eligible,"negative native clock origin rejects");
    for(double invalid:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}) {
        value=input();value.native_started=invalid;check(!c::evaluate(value).eligible,"nonfinite crash native time rejects");
        value=input();value.native_now=invalid;check(!c::evaluate(value).eligible,"nonfinite current native time rejects");
    }
    value=input();value.native_started=10;value.native_now=10.625;check(c::evaluate(value).eligible,"native duration is relative to crash time, independent of wall clock");
}
void malformed_geometry() {
    for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()})
        for(unsigned vector=0;vector<3;++vector)for(unsigned axis=0;axis<3;++axis) {
        auto value=input();auto& target=vector==0?value.bike_root:vector==1?value.rider_ground:value.linear_velocity;
        target[axis]=invalid;check(c::evaluate(value).issue==c::Issue::invalid_geometry,"all position/velocity components require finite current measurements");
    }
    auto value=input();value.rider_ground={std::numeric_limits<float>::max(),0,0};
    check(c::evaluate(value).issue==c::Issue::too_far_horizontal,"large finite positions reject without overflow or acceptance");
    value=input();value.linear_velocity={std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),0};
    check(c::evaluate(value).issue==c::Issue::moving,"large finite speed rejects without overflow or acceptance");
}
}

namespace {
using Admission=c::InteractionAdmission;
using Sample=c::InteractionSample;
namespace b=dingosdk::bmx::bail;
namespace receipt=dingosdk::bmx::impact_receipt;
constexpr float native_h=1.f/60.f;
Sample interaction_sample(std::uint64_t serial,bool standing=true) {
    return {{{0x10,0x20,0x30,0x40,0x50},0x60,0x70,0,2,7},true,true,standing,
        serial,double(serial)*native_h,native_h};
}
b::Status completed_fall() {
    b::Status fall;fall.ready=fall.consumed=fall.ragdoll_observed=fall.intent_status_available=true;
    fall.entity=0x20;fall.request_id=9;fall.intent_generation=4;fall.activation_enabled=fall.intent_armed=true;
    fall.intent_prior_false=fall.intent_written=fall.intent_true_returned=fall.intent_true_readback=true;
    fall.intent_reset=fall.intent_reset_readback=true;fall.intent_true_serial=34;fall.intent_reset_serial=35;
    fall.intent_true_calls=fall.intent_reset_calls=1;fall.intent_native_calls=2;return fall;
}
Admission interaction_gate() {
    Admission gate;const auto sample=interaction_sample(34);const auto fall=completed_fall();
    gate.begin(sample.identity,receipt::capture(true,fall,fall.entity),sample.serial,sample.native_time);return gate;
}
void captured_native180_ordering() {
    auto gate=interaction_gate();auto fall=completed_fall();
    // Actual archived edges:35=.5833332985639572 still FollowGround;36/37 are
    // ragdoll. Completed ragdoll/reset facts do not exist at35. Sampling remains
    // before the product capture update, so the checker still sees old leases.
    auto pre=interaction_sample(35);pre.native_time=.5833332985639572;
    fall.ragdoll_observed=false;
    check(!gate.observe(pre,fall),"native180 serial35 transient pre-fall standing must not arm recovery capture");
    fall.ragdoll_observed=true;
    auto down=interaction_sample(36,false);down.native_time=.5999999642372131;
    check(!gate.observe(down,fall),"native180 serial36 down remains uncaptured instead of inheriting serial35 capture");
    down=interaction_sample(37,false);down.native_time=.616666629910469;
    check(!gate.observe(down,fall),"native180 serial37 remains uncaptured");
    // Actual later native180 standing edge735 is the seed; next stand is a
    // policy fixture, not a claim that the sparse archived trace observed736.
    auto stand=interaction_sample(735);stand.native_time=12.249999269843102;
    check(!gate.observe(stand,fall),"genuine post-GetUp standing needs a later distinct native step");
    check(!gate.observe(stand,fall),"duplicate standing native epoch cannot supply recovery credit");
    ++stand.serial;stand.native_time+=native_h;
    check(gate.observe(stand,fall),"two fresh consecutive post-ragdoll FollowGround steps enable explicit recovery interaction");
    check(gate.observe(stand,fall),"fresh revalidation of ready duplicate does not toggle capture between native steps");
    stand.standable=false;check(!gate.observe(stand,fall),"current known down immediately withdraws capture");
}
void interaction_identity_clock_and_reset() {
    const auto fall=completed_fall();
    for(unsigned field=0;field<10;++field) {
        auto gate=interaction_gate();auto sample=interaction_sample(735);check(!gate.observe(sample,fall),"first standing sample is only a seed");
        ++sample.serial;sample.native_time+=native_h;
        switch(field){case 0:++sample.identity.owner.client;break;case 1:++sample.identity.owner.entity;break;
        case 2:++sample.identity.owner.core;break;case 3:++sample.identity.owner.context;break;case 4:++sample.identity.owner.rig;break;
        case 5:++sample.identity.world;break;case 6:++sample.identity.native_state;break;case 7:++sample.identity.body_index;break;
        case 8:++sample.identity.body_generation;break;default:++sample.identity.token;break;}
        check(!gate.observe(sample,fall),"every full rider/world/state/body/generation/token lane is pinned for interaction");
    }
    for(unsigned fault=0;fault<8;++fault) {
        auto gate=interaction_gate();auto sample=interaction_sample(735);(void)gate.observe(sample,fall);
        ++sample.serial;sample.native_time+=native_h;
        switch(fault){case 0:sample.coherent=false;break;case 1:sample.offboard=false;break;case 2:sample.serial-=2;break;
        case 3:++sample.serial;break;case 4:sample.native_time-=.1;break;case 5:sample.seconds=0;break;
        case 6:sample.seconds=std::numeric_limits<float>::quiet_NaN();break;default:sample.native_time=std::numeric_limits<double>::infinity();break;}
        check(!gate.observe(sample,fall),"missing owner/body/standing/clock or nonconsecutive native steps cannot arm interaction");
    }
    auto gate=interaction_gate();auto sample=interaction_sample(735);(void)gate.observe(sample,fall);gate.withdraw();
    ++sample.serial;sample.native_time+=native_h;check(!gate.observe(sample,fall),"observation quarantine drains standing suffix without reusing pre-unknown credit");
    ++sample.serial;sample.native_time+=native_h;check(gate.observe(sample,fall),"same complete identity may requalify after two fresh steps");
    gate.reset();check(!gate.observe(sample,fall),"a different crash lifetime cannot reuse the old receipt or standing suffix");
}
void interaction_fall_receipt_and_custody() {
    for(unsigned fault=0;fault<20;++fault) {
        auto gate=interaction_gate();auto fall=completed_fall();auto sample=interaction_sample(735);
        switch(fault){case 0:++fall.request_id;break;case 1:++fall.intent_generation;break;case 2:++fall.entity;break;
        case 3:fall.pending=true;break;case 4:fall.expired=true;break;case 5:fall.owner_changed=true;break;
        case 6:fall.ragdoll_observed=false;break;case 7:fall.consumed=false;break;case 8:fall.protected_owner=true;break;
        case 9:fall.intent_reset=false;break;case 10:fall.intent_reset_pending=true;break;case 11:fall.intent_reset_readback=false;break;
        case 12:fall.intent_true_returned=false;break;case 13:fall.intent_true_readback=false;break;case 14:fall.intent_prior_false=false;break;
        case 15:fall.intent_reset_serial=fall.intent_true_serial;break;case 16:++fall.intent_true_calls;break;
        case 17:fall.intent_status_available=false;break;case 18:fall.intent_owner_expired=true;break;default:fall.intent_written=false;break;}
        (void)gate.observe(sample,fall);++sample.serial;sample.native_time+=native_h;
        check(!gate.observe(sample,fall),"standing never bypasses the exact current consumed fall/ragdoll/reset handshake");
    }
    auto fall=completed_fall();auto sample=interaction_sample(735);
    fall.intent_native_cleared=true;fall.intent_reset_calls=0;fall.intent_native_calls=1;
    check(Admission::completed_fall(receipt::capture(true,fall,fall.entity),fall),"native-cleared verified typed reset remains supported");
    fall=completed_fall();fall.activation_enabled=fall.intent_armed=false;fall.native_calls=1;
    check(Admission::completed_fall(receipt::capture(true,fall,fall.entity),fall),"verified already-physical leaf remains supported");
    // A second physical-leaf fall keeps the lifetime call total from request1.
    // Its newly matched request must independently become consumed + ragdoll.
    fall.request_id=10;fall.native_calls=2;fall.pending=true;fall.consumed=fall.ragdoll_observed=false;
    const auto second_receipt=receipt::capture(true,fall,fall.entity);
    Admission repeated;repeated.begin(sample.identity,second_receipt,34,double(34)*native_h);
    (void)repeated.observe(sample,fall);++sample.serial;sample.native_time+=native_h;
    check(!repeated.observe(sample,fall),"prior lifetime native calls do not consume a new pending physical fall");
    fall.pending=false;fall.consumed=true;
    check(!repeated.observe(sample,fall),"new physical fall must independently observe native ragdoll");
    fall.ragdoll_observed=true;
    check(!repeated.observe(sample,fall),"completed second physical fall still needs first fresh standing seed");
    ++sample.serial;sample.native_time+=native_h;
    check(repeated.observe(sample,fall),"second genuine matched physical fall qualifies with lifetime native_calls=2");
    auto old_request=fall;--old_request.request_id;
    check(!Admission::completed_fall(second_receipt,old_request),"old request facts cannot satisfy a new physical-leaf receipt despite lifetime calls");
    fall.native_calls=3;++fall.request_id;
    check(Admission::completed_fall(receipt::capture(true,fall,fall.entity),fall),"later genuine physical falls have no lifetime-call ceiling");
    Admission gate;auto changed=sample.identity;++changed.owner.context;++changed.owner.rig;++changed.token;
    gate.adopt_recovered(changed,700,double(700)*native_h);sample.identity=changed;
    check(!gate.observe(sample,{}),"actual custody receipt seeds new-owner recovery provenance but gives no automatic interaction");
    ++sample.serial;sample.native_time+=native_h;
    check(gate.observe(sample,{}),"new custody association requires current coherent standing suffix under the new token");
}
}

int main(){captured_native180_ordering();interaction_identity_clock_and_reset();interaction_fall_receipt_and_custody();eligible_boundaries();owner_and_native_state();native_time();malformed_geometry();
    std::cout<<"BMX explicit crash recovery ownership, native-time and proximity policy tests passed.\n";}
