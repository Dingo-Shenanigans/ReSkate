#include "Extension/BMX/bmx_fall_lifecycle.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace f=dingosdk::bmx::fall_lifecycle;
namespace {std::size_t allocations{};}
void* operator new(std::size_t n){++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
f::Identity identity(){return {{11,12,13,14,15},{21,22,23,24,25}};}
f::RequestReceipt receipt(){return {true,true,true,true,true,12,31,32};}
f::Observation observation(std::uint64_t edge=0){
    const auto owner=identity();f::Observation o;
    o.rider_identity={true,owner.rider};o.body_identity={true,owner.body};
    o.clock={true,{1000+edge,100+double(edge)/60,1.f/60}};o.coherent_epoch=true;
    o.rider.available=true;o.rider.value.root={627,199,1062};o.rider.value.offboard=true;
    o.rider.value.physics_state={true,504};
    o.body.available=true;o.body.value.com={627,199.9f,1061.95f};
    o.counters={true,{33,66,33,33}};
    o.capture.bmx_capture={true,false};o.capture.interaction_capture={true,false};
    o.capture.rider_motion_lease={true,false};o.capture.drive_active={true,false};
    o.explicit_native_input_provided={true,false};return o;
}
void frozen_is_immutable(f::History<>& h){
    const auto before=h.status();const auto size=h.size();auto other=observation(5000);++other.rider_identity.value.entity;
    require(h.observe(other)==f::Result::ignored&&!h.finish(f::Reason::caller_cancelled),"terminal recorder ignores later state and end requests");
    require(h.status().reason==before.reason&&h.status().accepted_edges==before.accepted_edges&&
        h.status().written_samples==before.written_samples&&h.size()==size,"one frozen terminal history retains its reason and evidence");
}
void receipt_binding_and_initial_validation(){
    const auto owner=identity();auto initial=observation();f::History<> good;
    require(good.begin(owner,receipt(),initial)&&good.status().receipt_binding==f::ReceiptBinding::accepted_request,
        "fresh accepted typed request is bound to full rider/body identity");
    for(unsigned which=0;which<6;++which){auto unbound=receipt();
        if(which==0)unbound.status_fresh=false;
        if(which==1)unbound.status_available=false;
        if(which==2)unbound.request_id=0;
        if(which==3)unbound.generation=0;
        if(which==4)++unbound.entity;
        if(which==5)unbound.attempted=false;
        f::History<4> h;require(h.begin(owner,unbound,initial)&&h.status().receipt_binding==f::ReceiptBinding::unbound,
            "stale/missing/rejected protection metadata can remain explicitly unbound without inventing a request");
    }
    auto rejected=receipt();rejected.accepted=false;rejected.typed=false;rejected.generation=0;
    f::History<4> h;require(h.begin(owner,rejected,initial)&&h.status().receipt_binding==f::ReceiptBinding::rejected_request,
        "fresh rejected request remains a rejected request, never an accepted fall");
    require(h.observe(observation(1))!=f::Result::frozen&&h.status().receipt.request_id==rejected.request_id,
        "later observations cannot replace the original receipt with a global success");
    for(unsigned which=0;which<5;++which){auto invalid=initial;auto binding=owner;f::Config config;
        if(which==0)binding.body.token=0;
        if(which==1)invalid.clock.available=false;
        if(which==2)++invalid.rider_identity.value.core;
        if(which==3)++invalid.body_identity.value.generation;
        if(which==4)config.sample_period=.01;
        f::History<4> bad;require(!bad.begin(binding,receipt(),invalid,config)&&
            bad.status().phase==f::Phase::frozen&&bad.status().reason==f::Reason::invalid_begin&&bad.size()==0,
            "invalid identity/clock or unbounded sample cadence fails before a history sample");
    }
}
void duplicate_and_paused_clock(){
    f::History<> h;require(h.begin(identity(),receipt(),observation()),"begin paused fixture");
    auto duplicate=observation();duplicate.rider.value.standable=true;duplicate.rider.value.ragdoll_mode={true,true};
    duplicate.counters.value.velocity_updates=999;
    require(h.observe(duplicate)==f::Result::duplicate&&h.size()==1&&!h.status().standable_observed&&
        !h.status().ragdoll_observed&&!h.status().counter_advance_observed,"duplicate native clock cannot promote changing global/default fields into fresh evidence");
    for(std::uint64_t i=1;i<=5000;++i){auto paused=observation();paused.clock.value.serial+=i;paused.clock.value.seconds=0;
        require(h.observe(paused)==f::Result::decimated,"zero-dt native edges are bounded paused observations");}
    require(h.status().phase==f::Phase::recording&&h.status().native_elapsed==0&&h.status().paused_edges==5000&&h.size()==1,
        "paused native clock never expires a window or fills a ring with wall/render ticks");
    require(!h.finish(f::Reason::window_elapsed),"caller cannot manufacture an elapsed native observation window");
    require(h.finish(f::Reason::wall_timeout)&&h.status().reason==f::Reason::wall_timeout,
        "external wall timeout reports an incomplete native window distinctly");frozen_is_immutable(h);
}
void owner_body_and_clock_termination(){
    for(unsigned which=0;which<12;++which){f::History<> h;require(h.begin(identity(),receipt(),observation()),"begin termination fixture");
        auto changed=observation(1);f::Reason reason=f::Reason::clock_discontinuity;
        if(which==0){++changed.rider_identity.value.core;reason=f::Reason::owner_changed;}
        if(which==1){++changed.body_identity.value.token;reason=f::Reason::body_changed;}
        if(which==2){++changed.body_identity.value.generation;reason=f::Reason::body_changed;}
        if(which==3){++changed.body_identity.value.world;reason=f::Reason::body_changed;}
        if(which==4){changed.rider_identity.available=false;reason=f::Reason::rider_unavailable;}
        if(which==5){changed.clock.available=false;reason=f::Reason::clock_unavailable;}
        if(which==6){changed.clock.value.serial=999;reason=f::Reason::clock_backwards;}
        if(which==7){changed.clock.value.time=99;reason=f::Reason::clock_backwards;}
        if(which==8)changed.clock.value.serial=1000;
        if(which==9){changed.clock.value.serial=1100;changed.clock.value.time=102;}
        if(which==10)changed.clock.value.time=100.2;
        if(which==11)changed.clock.value.time=std::numeric_limits<double>::quiet_NaN();
        require(h.observe(changed)==f::Result::frozen&&h.status().reason==reason&&h.size()==1&&
            h.status().accepted_edges==1,"foreign identity or discontinuous clock freezes without appending a misleading native edge");
        frozen_is_immutable(h);
    }
}
void missing_snapshot_and_retained_defaults(){
    {
        auto simultaneous=observation();simultaneous.rider.value.standable=true;simultaneous.rider.value.ragdoll_mode={true,true};
        f::History<4> same;require(same.begin(identity(),receipt(),simultaneous)&&same.status().ragdoll_observed&&
            same.status().standable_observed&&!same.status().standable_after_ragdoll,
            "simultaneous raw flags are recorded without fabricating a chronological get-up transition");
    }
    f::History<> h;auto initial=observation();initial.rider.value.ragdoll_mode={false,true};
    require(h.begin(identity(),receipt(),initial)&&!h.status().ragdoll_observed,"unavailable retained ragdoll value is not raw observation");
    for(unsigned i=1;i<=6;++i){auto missing=observation(i);missing.body_identity.available=false;missing.body.available=false;
        missing.rider.available=false;missing.rider.value.standable=true;missing.rider.value.ragdoll_mode={true,true};
        missing.snapshot_issue=f::SnapshotIssue::read_unavailable;
        h.observe(missing);}
    require(h.status().snapshot_failures==6&&!h.status().standable_observed&&!h.status().ragdoll_observed&&
        h.status().phase==f::Phase::recording,"missing snapshots remain unavailable instead of becoming get-up/fall evidence");
    bool saw_gap=false;h.each([&](const f::Sample& sample){if(sample.elapsed>.09){saw_gap=true;
        require(!sample.rider.available&&!sample.body.available&&sample.snapshot_issue==f::SnapshotIssue::read_unavailable,
            "stored decimated failure keeps raw availability and failure reason");}});require(saw_gap,"failed snapshot is visible at bounded cadence");
    auto incoherent=observation(7);incoherent.coherent_epoch=false;incoherent.rider.value.standable=true;
    incoherent.rider.value.ragdoll_mode={true,true};h.observe(incoherent);
    require(!h.status().standable_observed&&!h.status().ragdoll_observed,"different native epochs cannot qualify fresh raw state");
    auto invalid=observation(8);invalid.rider.value.root[0]=std::numeric_limits<float>::quiet_NaN();
    invalid.rider.value.standable=true;h.observe(invalid);require(!h.status().standable_observed,"invalid raw root is not a valid rider-state snapshot");
    require(h.finish(f::Reason::body_released)&&h.status().reason==f::Reason::body_released,
        "explicit F6/recovery body release ends observation without get-up completion");frozen_is_immutable(h);
}
void real_state_facts_without_input_or_fake_completion(){
    f::History<> h;require(h.begin(identity(),receipt(),observation()),"begin raw-state transition");
    bool recorded_ragdoll=false,recorded_standable=false;
    for(unsigned i=1;i<=1800;++i){auto edge=observation(i);
        edge.rider.value.ragdoll_mode={true,i<360};edge.rider.value.standable=i>=360;
        edge.rider.value.motion_supported=i>=360;
        if(i==30)++edge.counters.value.velocity_updates;
        else if(i>30)edge.counters.value.velocity_updates=34;
        const auto result=h.observe(edge);
        if(i==360)require(h.status().phase==f::Phase::recording&&h.status().standable_after_ragdoll&&
            h.status().standable_after_nonstandable,"fresh native standable transition is a fact and does not auto-complete monitoring");
        if(i<1800)require(result!=f::Result::frozen,"normal fresh state observation remains active for its native window");
    }
    require(h.status().phase==f::Phase::frozen&&h.status().reason==f::Reason::window_elapsed&&
        h.status().native_elapsed==30&&h.status().counter_advance_observed,"only actual native window expiry terminates this observation");
    h.each([&](const f::Sample& sample){
        if(sample.rider.available&&sample.rider.value.ragdoll_mode.available&&sample.rider.value.ragdoll_mode.value)recorded_ragdoll=true;
        if(sample.rider.available&&sample.rider.value.standable)recorded_standable=true;
        require(sample.explicit_native_input_provided.available&&!sample.explicit_native_input_provided.value,
            "passive standable observation cannot invent explicit native/get-up input");
    });
    require(recorded_ragdoll&&recorded_standable&&h.size()<=310&&h.status().overwritten_samples==0,
        "default fixed storage covers30native seconds near10Hz and retains first transition evidence");
    frozen_is_immutable(h);
}
void ring_capacity_allocation_and_counter_availability(){
    static_assert(std::is_trivially_copyable_v<f::History<4>>);
    f::History<5> h;f::Config config;config.window_seconds=15;const auto before_allocations=allocations;
    require(h.begin(identity(),receipt(),observation(),config),"begin fixed ring");
    for(unsigned i=1;i<=900;++i){auto edge=observation(i);
        if(i==10){edge.counters.available=false;edge.counters.value={};}
        h.observe(edge);}
    require(allocations==before_allocations,"begin/observe/freeze use no per-step allocation");
    require(h.size()==5&&h.status().phase==f::Phase::frozen&&h.status().written_samples==151&&
        h.status().overwritten_samples==146,"small ring stays bounded with explicit overwrite accounting");
    require(!h.status().counter_regression_observed&&!h.status().counter_advance_observed,
        "unavailable counter defaults neither prove cleanup nor manufacture a counter regression");
    double previous=-1;h.each([&](const f::Sample& sample){require(sample.elapsed>previous&&sample.elapsed>=14.5,
        "ring traversal is chronological after wrap");previous=sample.elapsed;});
    require(previous==15&&h.initial().elapsed==0&&h.initial().clock.time==100,"initial baseline survives ring overwrite separately");
    h.reset();require(h.status().phase==f::Phase::idle&&h.size()==0&&h.begin(identity(),receipt(),observation()),
        "only an explicit reset permits a separate later fall lifecycle");
}
void optional_detail_hint_never_skips_native_edges(){
    f::History<8> h;require(h.begin(identity(),receipt(),observation()),"begin optional detail cadence");
    const auto before=h.status().accepted_edges;
    require(!h.sample_due(observation().clock.value)&&!h.sample_due(observation(1).clock.value)&&
        h.sample_due(observation(6).clock.value)&&h.status().accepted_edges==before,
        "optional detail hint predicts cadence without consuming or advancing a native edge");
    auto brief_standable=observation(1);brief_standable.rider.value.standable=true;
    require(!h.sample_due(brief_standable.clock.value)&&h.observe(brief_standable)==f::Result::recorded&&
        h.status().standable_after_nonstandable,"a fact inside100ms is retained even when optional detail is not due");
    for(unsigned i=2;i<=6;++i){const auto edge=observation(i);
        require(h.sample_due(edge.clock.value)==(i==6),"intervening fact does not move the optional native-time sample boundary");
        h.observe(edge);}
    require(h.status().accepted_edges==7,"all native edges reach history independently of optional detail cadence");
    f::History<4> paused;require(paused.begin(identity(),receipt(),observation()),"begin paused detail hint");
    for(unsigned i=1;i<=1000;++i){auto edge=observation();edge.clock.value.serial+=i;edge.clock.value.seconds=0;
        require(!paused.sample_due(edge.clock.value)&&paused.observe(edge)==f::Result::decimated,
            "long paused serial stream still reaches clock validation while expensive details stay deferred");}
    auto resumed=observation();resumed.clock.value.serial+=1001;resumed.clock.value.time+=1./60;
    require(paused.observe(resumed)!=f::Result::frozen&&paused.status().accepted_edges==1002,
        "resume after many zero-dt edges does not invent a huge serial discontinuity");
    require(h.finish(f::Reason::body_released)&&!h.sample_due(observation(7).clock.value),"frozen history cannot schedule more optional reads");
}
void explicit_dense_edges_preserve_validation_and_default_cadence(){
    f::History<> dense,ordinary;const auto initial=observation();const auto before_allocations=allocations;
    require(!initial.record_edge&&dense.begin(identity(),receipt(),initial)&&ordinary.begin(identity(),receipt(),initial),
        "dense recording is an explicit opt-in and default begins remain unchanged");
    for(unsigned edge=1;edge<=1800;++edge){
        auto sample=observation(edge);sample.record_edge=edge<=15;
        const auto result=dense.observe(sample);
        if(edge<=15)require(result==f::Result::recorded&&dense.size()==edge+1,
            "the first fifteen fresh60Hz native edges are retained when explicitly requested");
        sample.record_edge=false;ordinary.observe(sample);
    }
    require(allocations==before_allocations,"dense opt-in history adds no allocations");
    require(dense.status().phase==f::Phase::frozen&&dense.status().reason==f::Reason::window_elapsed&&
        dense.status().native_elapsed==30&&dense.size()==314&&dense.status().written_samples==314&&
        dense.status().overwritten_samples==0,"fifteen dense edges and ordinary cadence fit the default thirty-second bank");
    require(ordinary.size()==301&&ordinary.status().written_samples==301&&ordinary.status().overwritten_samples==0,
        "omitting the flag retains the original10Hz default cadence");
    std::uint64_t seen{};dense.each([&](const f::Sample& sample){if(seen<=15)require(sample.clock.serial==1000+seen,
        "the retained early bank contains every chronological native edge");++seen;});
    require(dense.initial().clock.serial==initial.clock.value.serial&&dense.initial().clock.time==initial.clock.value.time&&
        dense.initial().rider.value.root==initial.rider.value.root&&dense.initial().body.value.com==initial.body.value.com&&
        dense.status().receipt.request_id==receipt().request_id,"forced appends never overwrite the immutable initial sample or request receipt");
    frozen_is_immutable(dense);
    {
        f::History<4> h;require(h.begin(identity(),receipt(),observation()),"begin forced duplicate fixture");
        auto duplicate=observation();duplicate.record_edge=true;duplicate.rider.value.standable=true;
        duplicate.rider.value.ragdoll_mode={true,true};duplicate.counters.value.velocity_updates=999;
        require(h.observe(duplicate)==f::Result::duplicate&&h.size()==1&&!h.status().standable_observed&&
            !h.status().ragdoll_observed&&!h.status().counter_advance_observed,
            "forced storage cannot turn a duplicate clock into fresh facts");
    }
    for(unsigned which=0;which<5;++which){
        f::History<4> h;require(h.begin(identity(),receipt(),observation()),"begin forced rejection fixture");
        auto changed=observation(1);changed.record_edge=true;f::Reason expected=f::Reason::clock_discontinuity;
        if(which==0){++changed.rider_identity.value.entity;expected=f::Reason::owner_changed;}
        if(which==1){++changed.body_identity.value.generation;expected=f::Reason::body_changed;}
        if(which==2){changed.clock.available=false;expected=f::Reason::clock_unavailable;}
        if(which==3){changed.clock.value.time=99;expected=f::Reason::clock_backwards;}
        if(which==4)changed.clock.value.time=100.2;
        require(h.observe(changed)==f::Result::frozen&&h.status().reason==expected&&h.size()==1,
            "forced storage cannot bypass owner, body or native clock guards");
    }
    {
        f::History<4> h;require(h.begin(identity(),receipt(),observation()),"begin forced incoherent snapshot fixture");
        auto sample=observation(1);sample.record_edge=true;sample.coherent_epoch=false;
        sample.rider.value.standable=true;sample.rider.value.ragdoll_mode={true,true};
        require(h.observe(sample)==f::Result::recorded&&h.status().snapshot_failures==1&&
            !h.status().standable_observed&&!h.status().ragdoll_observed,
            "forced snapshot keeps a coherence failure without promoting its raw defaults to native facts");
        bool checked=false;h.each([&](const f::Sample& stored){if(stored.clock.serial==1001){checked=true;
            require(!stored.rider.available&&!stored.body.available&&stored.snapshot_issue==f::SnapshotIssue::epoch_mismatch,
                "forced diagnostic rows retain the same unknown lanes and epoch failure reason");}});
        require(checked,"forced off-cadence snapshot failure remains visible");
    }
}
void raw_words_and_finite_coordinates_keep_separate_availability(){
    auto initial=observation();auto& rider=initial.rider.value;
    rider.context_830_words={true,{0x4399U,0x4389U,0xc3edU,0x7fc01234U}};
    rider.context_830_xyz={true,{307,275,-474}};
    // The full float4 is deliberately unavailable; its fourth word is not
    // treated as a coordinate or replaced by a fabricated finite value.
    rider.context_830.available=false;
    rider.active_160_words={false,{1,2,3,4}};
    f::History<4> history;
    require(history.begin(identity(),receipt(),initial),"begin independently qualified raw diagnostic lanes");
    const auto& stored=history.initial().rider.value;
    require(stored.context_830_words.available&&stored.context_830_words.value[3]==0x7fc01234U&&
        stored.context_830_xyz.available&&stored.context_830_xyz.value==f::Vec{307,275,-474}&&
        !stored.context_830.available&&!stored.active_160_words.available&&history.status().snapshot_failures==0,
        "readable raw bits and finite XYZ survive independently without promoting unreadable or non-coordinate lanes");
    auto incoherent=initial;incoherent.clock=observation(6).clock;incoherent.coherent_epoch=false;
    require(history.observe(incoherent)==f::Result::recorded,"retain incoherent diagnostic failure");
    history.each([](const f::Sample& row){if(row.clock.serial==1006)require(!row.rider.available,
        "raw diagnostics cannot bypass native epoch qualification");});
}
}
int main(){try{receipt_binding_and_initial_validation();duplicate_and_paused_clock();owner_body_and_clock_termination();
    missing_snapshot_and_retained_defaults();real_state_facts_without_input_or_fake_completion();ring_capacity_allocation_and_counter_availability();optional_detail_hint_never_skips_native_edges();explicit_dense_edges_preserve_validation_and_default_cadence();raw_words_and_finite_coordinates_keep_separate_availability();
    std::cout<<"Passive fall lifecycle identity, native-edge freshness, bounded storage, raw-state and no-actuation tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
