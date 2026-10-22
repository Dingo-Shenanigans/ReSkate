#include "Engine/Game/BMX/mount_readiness.h"
#include "Engine/Game/BMX/bmx_mount_pose.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace dingosdk::bmx;
void check(bool value, const char* reason) {
    if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
constexpr EquipmentOwner rider{1, 2, 3, 4, 5};
}
namespace {
unsigned dismount_checks{};
void settle_check(bool ok,const char* why){++dismount_checks;check(ok,why);}
MountDismountSample settle_sample(std::uint64_t wall=1000,std::uint64_t serial=10,double time=1) {
    MountDismountSample s;s.now=s.input_sampled_ms=wall;s.deadline=16000;s.serial=serial;s.native_time=time;s.owner=rider;
    s.available=s.timing_available=s.offboard=s.standable=s.motion_supported=s.physics_available=s.quiet=true;s.physics=504;return s;
}
void arm_dismount(PendingMountDismount& gate){auto s=settle_sample();s.offboard=false;s.standable=false;s.physics=100;
    settle_check(gate.update(s)==MountDismountResult::waiting&&gate.required(),"observed on-board entry requires settling");}
void native_dismount_settle_tests(){
    {PendingMountDismount gate;auto s=settle_sample();s.timing_available=s.quiet=false;
        settle_check(gate.update(s)==MountDismountResult::ready&&!gate.required(),"already on-foot path has no added admission dwell");}
    {PendingMountDismount gate;arm_dismount(gate);
        // Exact retained260 sample clock and flags; surrounding epochs below
        // are synthetic. The later log proves unsupported state, not subtype.
        auto before=settle_sample(1000,39627,660.4666272997856-1.0/60);gate.reset();before.offboard=false;before.physics=100;
        settle_check(gate.update(before)==MountDismountResult::waiting,"synthetic board predecessor");
        auto first=settle_sample(1016,39628,660.4666272997856);
        settle_check(gate.update(first)==MountDismountResult::waiting,"a sole standable sample cannot activate BMX");
        first.now=first.input_sampled_ms=1032;first.serial++;first.native_time+=1.0/60;first.motion_supported=false;
        settle_check(gate.update(first)==MountDismountResult::waiting,"unsupported suffix clears the prospective mount");}
    {PendingMountDismount gate;arm_dismount(gate);unsigned mocked_acquisitions{};
        for(unsigned i=1;i<=17;++i){const auto decision=gate.update(settle_sample(1000+16*i,10+i,1+i/60.0));
            if(decision==MountDismountResult::ready)++mocked_acquisitions;
            settle_check(i<17?decision==MountDismountResult::waiting:decision==MountDismountResult::ready,"both clocks must reach250ms sampled dwell");}
        settle_check(mocked_acquisitions==1,"only fully settled synthetic sample reaches unchanged acquisition seam");}
    for(unsigned fault=0;fault<20;++fault){PendingMountDismount gate;arm_dismount(gate);
        settle_check(gate.update(settle_sample(1016,11,1.016))==MountDismountResult::waiting,"new streak starts without admission");
        auto bad=settle_sample(1032,12,1.032);
        switch(fault){case 0:bad.available=false;break;case 1:bad.timing_available=false;break;case 2:bad.offboard=false;break;
        case 3:bad.standable=false;break;case 4:bad.motion_supported=false;break;case 5:bad.physics_available=false;break;
        case 6:bad.physics=103;break;case 7:bad.quiet=false;break;case 8:bad.input_sampled_ms=0;break;
        case 9:bad.input_sampled_ms=1033;break;case 10:bad.input_sampled_ms=780;break;case 11:bad.serial=0;break;
        case 12:bad.native_time=std::numeric_limits<double>::quiet_NaN();break;case 13:bad.native_time=-1;break;
        case 14:bad.now=bad.input_sampled_ms=1117;break;case 15:bad.serial=24;break;case 16:bad.native_time=1.117;break;
        case 17:gate.interrupt();break;case 18:bad.physics=100;break;case 19:bad.motion_supported=bad.standable=false;break;}
        settle_check(gate.update(bad)==MountDismountResult::waiting,"readiness loss or gap cannot complete streak");
        // Continue from the last valid clock, without implying unknown native
        // states were sampled during the gap.
        const auto wall=bad.now+16;const auto serial=std::max<std::uint64_t>(bad.serial,12)+1;
        const double time=std::isfinite(bad.native_time)&&bad.native_time>=1.032?bad.native_time+.016:1.048;
        settle_check(gate.update(settle_sample(wall,serial,time))==MountDismountResult::waiting,"fresh readiness starts a new dwell after loss");
    }
    for(unsigned fault=0;fault<10;++fault){PendingMountDismount gate;arm_dismount(gate);(void)gate.update(settle_sample(1016,11,1.016));auto s=settle_sample(1032,12,1.032);
        switch(fault){case 0:++s.owner.client;break;case 1:++s.owner.entity;break;case 2:++s.owner.core;break;case 3:++s.owner.context;break;
        case 4:++s.owner.rig;break;case 5:s.now=999;break;case 6:s.serial=10;break;case 7:s.native_time=1;break;
        case 8:s.serial=11;break;case 9:s.native_time=1.016;break;}
        settle_check(gate.update(s)==MountDismountResult::cancelled,"owner or clock regression cancels original mount intent");
        settle_check(gate.update(settle_sample(2000,40,2))==MountDismountResult::cancelled,"cancelled policy cannot revive without explicit reset");}
    {PendingMountDismount gate;arm_dismount(gate);(void)gate.update(settle_sample(1016,11,1.016));
        for(unsigned i=1;i<=30;++i)settle_check(gate.update(settle_sample(1016+i*16,11,1.016))==MountDismountResult::waiting,"duplicate native epoch cannot spend wall time into readiness");}
    {PendingMountDismount gate;arm_dismount(gate);(void)gate.update(settle_sample(1016,11,1.016));
        for(unsigned i=1;i<=15;++i)settle_check(gate.update(settle_sample(1016+i*16,11+i,1.016+i*.017))==MountDismountResult::waiting,"faster native clock still waits for wall dwell");
        settle_check(gate.update(settle_sample(1266,26,1.271))==MountDismountResult::waiting,"duplicate final epoch cannot finish wall dwell");
        settle_check(gate.update(settle_sample(1282,27,1.288))==MountDismountResult::ready,"only advancing final epoch completes both dwell clocks");}
    {PendingMountDismount gate;arm_dismount(gate);auto s=settle_sample(16000,20,2);
        settle_check(gate.update(s)==MountDismountResult::cancelled,"original15s deadline has priority over readiness");gate.reset();
        settle_check(gate.update(settle_sample(20000,1,0))==MountDismountResult::cancelled,"reset cannot extend caller original expired deadline");}
    {PendingMountDismount gate;arm_dismount(gate);(void)gate.update(settle_sample(1016,11,1.016));gate.reset();
        settle_check(!gate.required()&&gate.update(settle_sample(1032,12,1.032))==MountDismountResult::ready,"fresh already-on-foot request clears prior gate");}
    std::cout<<dismount_checks<<" pending native dismount settling checks passed\n";
}
}

int main() {
    native_dismount_settle_tests();
    PendingMountOwner owner;
    constexpr std::uint64_t deadline = 16000;
    check(owner.update(1000, deadline, false, {}) == MountOwnerResult::waiting,
        "unavailable native rider may become ready before the original deadline");
    check(owner.update(1001, deadline, true, rider) == MountOwnerResult::ready,
        "first validated rider pins the request identity");
    check(mount_ground_readiness(false, false, 200, 0) == MountGroundResult::query_unavailable,
        "post-dismount collision initialization never permits acquisition");
    check(owner.update(2000, deadline, true, rider) == MountOwnerResult::ready &&
        mount_ground_readiness(true, false, 200, 0) == MountGroundResult::unsupported,
        "a clear wheel query waits without mounting or losing the owner");
    check(owner.update(3000, deadline, true, rider) == MountOwnerResult::ready &&
        mount_ground_readiness(true, true, 200, 197) == MountGroundResult::invalid_height,
        "distant support cannot mount during the pending window");
    check(owner.update(4000, deadline, true, rider) == MountOwnerResult::ready &&
        mount_ground_readiness(true, true, 200, 199.9) == MountGroundResult::ready,
        "the same request can acquire after unavailable, unsupported and distant ground recover");
    check(owner.update(deadline, deadline, true, rider) == MountOwnerResult::timed_out,
        "even valid ground cannot acquire at the original deadline");
    check(owner.update(deadline + 1, deadline, false, {}) == MountOwnerResult::timed_out,
        "missing rider and persistent invalid ground cannot extend the deadline");
    owner.reset();
    check(owner.update(1001, deadline, true, rider) == MountOwnerResult::ready,
        "fresh request can pin its rider");
    check(owner.update(1002, deadline, false, {}) == MountOwnerResult::waiting,
        "a temporary read failure does not clear the pinned rider");
    auto replacement = rider; replacement.core = 9;
    check(owner.update(1003, deadline, true, replacement) == MountOwnerResult::owner_changed,
        "a replaced native core cancels rather than inheriting pending mount");
    owner.reset();
    auto incomplete = rider; incomplete.rig = 0;
    check(owner.update(1001, deadline, true, incomplete) == MountOwnerResult::waiting,
        "incomplete ownership cannot pin or acquire");
    check(owner.update(1002, deadline, true, replacement) == MountOwnerResult::ready,
        "explicit reset permits a fresh request to bind a new complete owner");
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    check(mount_ground_readiness(true, true, nan, 200) == MountGroundResult::invalid_height &&
        mount_ground_readiness(true, true, 200, nan) == MountGroundResult::invalid_height,
        "nonfinite wheel or rider height never bypasses support verification");

    NearbyMountSearch search;
    const std::array<float,3> origin{295.787872f,274.271484f,-471.909882f};
    check(search.begin(rider,0,origin,-2.3494325f),"ordinary request pins finite rider-relative search origin and yaw");
    check(search.next(rider,0,753,12.549999251961708)==NearbyMountDecision::query&&search.candidate()==origin,
        "first query is the exact original run156 center before any nearby fallback");
    check(search.bind_support({9421466624ULL,1184208928ULL}),"first actual nonzero query world/context pin the request");
    search.finish_attempt(true); // Recorded156 initial Issue::discontinuous; no final witnesses existed.
    check(search.index()==1,"actual156 measured geometric rejection consumes only its original point");
    check(search.next(rider,0,753,12.549999251961708)==NearbyMountDecision::waiting,
        "duplicate native epoch cannot query the next candidate");
    check(search.next(rider,0,754,12.566665917634964)==NearbyMountDecision::query,
        "one later native epoch permits one closest-radius candidate");
    const auto second=search.candidate();
    check(std::abs(std::hypot(second[0]-origin[0],second[2]-origin[2])-.2f)<.0001f&&second[1]==origin[1],
        "nearby search changes only horizontal location within its configured radius");
    search.finish_attempt(false);
    check(search.next(rider,0,755,12.58333258330822)==NearbyMountDecision::query&&search.candidate()==second,
        "unavailable geometry or raced final clock retries the same point without extending radius");
    search.finish_attempt(true);search.finish_attempt(true);
    check(search.index()==2,"one attempt can consume at most one candidate");
    check(!search.bind_support({9421466625ULL,1184208928ULL})&&!search.active(),"different actual support world cancels instead of transferring the request");
    check(search.begin(rider,0,origin,0)&&search.bind_support({123,456})&&!search.bind_support({123,457})&&!search.active(),
        "same-world support context replacement also cancels the pinned request");
    for(unsigned fault=0;fault<10;++fault) {
        check(search.begin(rider,31,origin,0),"fresh bounded policy fixture begins");
        check(search.next(rider,31,100,2)==NearbyMountDecision::query,"fixture pins an initial native epoch");
        search.finish_attempt(false);auto fault_owner=rider;std::uint64_t token=31,serial=101;double time=2+1.0/60;
        if(fault==0)fault_owner.rig++;
        if(fault==1)token++;
        if(fault==2)serial=99;
        if(fault==3)time=1.9;
        if(fault==4)time=2;
        if(fault==5)serial=100;
        if(fault==6)time=2.101;
        if(fault==7)time=std::numeric_limits<double>::quiet_NaN();
        if(fault==8)serial=113;
        if(fault==9)serial=0;
        check(search.next(fault_owner,token,serial,time)==NearbyMountDecision::cancelled&&!search.active(),
            "owner/token/rewind/partial-clock/gap/nonfinite changes fail closed without another query");
    }
    check(search.begin(rider,0,origin,.7f),"finite exhaustive-search fixture begins");
    double previous_radius=-1;
    for(unsigned i=0;i<NearbyMountSearch::offsets.size();++i) {
        check(search.next(rider,0,100+i,2+i/60.0)==NearbyMountDecision::query,"at most one candidate is released per increasing native frame");
        const auto p=search.candidate();const auto radius=std::hypot(p[0]-origin[0],p[2]-origin[2]);
        check(radius<=NearbyMountSearch::maximum_radius+.0001&&radius+.0001>=previous_radius,
            "all seventeen samples stay within0.4m and never search a farther radius before a closer one");
        previous_radius=radius;search.finish_attempt(true);
    }
    check(search.exhausted()&&search.next(rider,0,117,2+17/60.0)==NearbyMountDecision::exhausted,
        "seventeen measured rejections exhaust instead of looping or allocating a body");
    constexpr std::array<std::array<float,2>,17> expected{{
        {0,0},{.2f,0},{-.2f,0},{0,.2f},{0,-.2f},
        {.1414213562f,.1414213562f},{-.1414213562f,.1414213562f},
        {.1414213562f,-.1414213562f},{-.1414213562f,-.1414213562f},
        {.4f,0},{-.4f,0},{0,.4f},{0,-.4f},
        {.2828427125f,.2828427125f},{-.2828427125f,.2828427125f},
        {.2828427125f,-.2828427125f},{-.2828427125f,-.2828427125f}}};
    check(search.begin(rider,0,{},0),"ordered offset contract fixture begins at zero yaw");
    for(unsigned i=0;i<expected.size();++i) {
        check(search.next(rider,0,100+i,2+i/60.0)==NearbyMountDecision::query,"ordered contract gets only one release per native epoch");
        check(search.candidate()==std::array<float,3>{expected[i][0],0,expected[i][1]}&&search.yaw()==0,
            "ordered local left/right/forward/back/diagonal samples exactly match the fixed search contract");
        search.finish_attempt(true);
    }
    search.cancel();check(!search.active()&&search.world()==0&&search.index()==0,
        "explicit cancellation/new request clears owner, world, clock and candidate progress");
    check(search.begin(rider,0,origin,0)&&search.next(rider,0,1,1.0/60)==NearbyMountDecision::query&&search.candidate()==origin,
        "new mount restarts at its own fresh epoch and original center");
    auto invalid=origin;invalid[0]=std::numeric_limits<float>::infinity();
    check(!search.begin(rider,0,invalid,0)&&!search.active(),"nonfinite search origin cannot authorize a query");
    {
        namespace m=mount_pose;
        const m::Request measured{{295.7878723144531f,274.271484375f,-471.9098815917969f},-2.3494324684143066f,
            9421466624ULL,1184208928ULL,753,12.549999251961708};
        const std::array<m::Sample,2> initial{{
            {true,true,true,{295.3589782714844f,274.27154541015625f,-472.335693359375f},
                {.001143542816862464f,.9999589920043945f,.008984026499092579f},
                measured.world,measured.context,measured.serial,measured.simulation_time},
            {true,true,true,{296.2268371582031f,274.2718200683594f,-471.4826354980469f},
                {-.030652664601802826f,.9994444847106934f,-.013081436976790428f},
                measured.world,measured.context,measured.serial,measured.simulation_time}}};
        const auto refused=m::prepare(measured,initial);
        check(!refused.prepared&&refused.issue==m::Issue::discontinuous,
            "unchanged full prepare rejects actual156 initial normal spread; no final native witnesses were observed");
        check(search.begin(rider,0,measured.position,measured.yaw)&&
            search.next(rider,0,measured.serial,measured.simulation_time)==NearbyMountDecision::query,
            "measured156 center is released once under a synthetic complete rider-owner fixture");
        check(search.bind_support({measured.world,measured.context}),"measured156 support world/context pin the simulated request");
        search.finish_attempt(!refused.prepared);
        check(search.next(rider,0,754,measured.simulation_time+1.0/60)==NearbyMountDecision::query,
            "measured156 rejection permits a new sampled position only at a later native epoch");
        // Explicitly synthetic flat terrain at the next candidate. The run did
        // not query this position; it is a pure current-policy regression case.
        const m::Request synthetic{search.candidate(),search.yaw(),measured.world,measured.context,754,
            measured.simulation_time+1.0/60};
        auto support=initial;
        for(unsigned i=0;i<2;++i) {
            const auto z=geometry::wheel_hub(i==0)[2];
            support[i]={true,true,true,{synthetic.position[0]+z*std::sin(synthetic.yaw),synthetic.position[1],
                synthetic.position[2]+z*std::cos(synthetic.yaw)},{0,1,0},synthetic.world,synthetic.context,
                synthetic.serial,synthetic.simulation_time};
        }
        const auto candidate=m::prepare(synthetic,support);
        check(candidate.prepared&&!candidate.ready,"synthetic nearby initial planes still do not authorize acquisition");
        unsigned mock_allocations=0,mock_body_writes=0;
        const auto acquire_gate=[&](const m::Seed& seed) {
            // This is the same pure predicate used by native acquire BEFORE
            // create_pool/transform. Counters are mock effects, not native calls.
            if(!m::matches(seed,synthetic.world,synthetic.context,synthetic.serial,synthetic.simulation_time))return;
            ++mock_allocations;++mock_body_writes;
        };
        acquire_gate(refused);acquire_gate(candidate);
        check(mock_allocations==0&&mock_body_writes==0,"measured refusal and prepared-only synthetic candidate cannot pass the pre-allocation support lease gate");
        for(unsigned i=0;i<2;++i) {
            const auto hub=chassis::rotate_unit(candidate.orientation,geometry::wheel_hub(i==0));
            for(unsigned j=0;j<3;++j)support[i].point[j]=candidate.root[j]+hub[j];
            support[i].point[1]-=static_cast<float>(geometry::wheel_radius);
        }
        auto edge=support;edge[0].verified_plane=false;
        acquire_gate(m::verify_final(candidate,edge));
        check(mock_allocations==0&&mock_body_writes==0,"an unverified final edge still cannot trigger mocked body effects");
        const auto ready=m::verify_final(candidate,support);acquire_gate(ready);
        check(ready.ready&&mock_allocations==1&&mock_body_writes==1,
            "only independent synthetic final witnesses satisfying the unchanged policy pass the same activation predicate");
        check(!m::matches(ready,synthetic.world,synthetic.context,synthetic.serial+1,synthetic.simulation_time+1.0/60),
            "ready candidate cannot carry its witness lease into a later native frame");
    }

    {
        check(!setdown_search_permitted(CarryPathObservation::unknown,false)&&
            !setdown_search_permitted(CarryPathObservation::unknown,true),
            "unknown carry path (unavailable ray/world/transform/large jump) never initiates or services even explicit setdown");
        check(setdown_search_permitted(CarryPathObservation::measured_obstacle,false)&&
            setdown_search_permitted(CarryPathObservation::clear,true)&&
            !setdown_search_permitted(CarryPathObservation::clear,false),
            "automatic fallback requires a measured obstruction while an explicit intent may service on a freshly clear carry path");
        namespace m=mount_pose;
        const m::Request measured{{297.3636169433594f,274.287353515625f,-474.3694763183594f},-2.2794594764709473f,
            9416504416ULL,1186236608ULL,2350,39.16666433215141};
        const std::array<m::Sample,2> support{{
            {true,true,true,{296.906494140625f,274.2879333496094f,-474.7622375488281f},
                {.000991592649370432f,.9999932646751404f,.0035316431894898415f},
                measured.world,measured.context,measured.serial,measured.simulation_time},
            {true,true,true,{297.8243408203125f,274.2868957519531f,-473.9713439941406f},
                {-.010031862184405327f,.9997878670692444f,-.0179878119379282f},
                measured.world,measured.context,measured.serial,measured.simulation_time}}};
        const auto rejected=m::prepare(measured,support);
        check(!rejected.prepared&&rejected.issue==m::Issue::discontinuous,
            "actual184 carried setdown normal angle0.02418rad remains rejected by unchanged0.02rad strict support gate");
        const RetainedBikeIdentity key{rider,measured.world,0x700,0,3,2};
        SetDownSearch down;
        check(down.begin(key,measured.context,measured.position,measured.yaw),"setdown pins full released bike and TLS context");
        check(down.next(key,measured.context,measured.serial,measured.simulation_time)==NearbyMountDecision::query,
            "first setdown query is exact measured184 refused center");
        SetDownWitness witness{key,measured.context,measured.serial,measured.simulation_time,true,true,true,true};
        check(!down.ready(rejected,witness),"cheap successful heights and clear approach cannot select actual184 rejected support");
        down.finish_attempt(true);
        check(down.next(key,measured.context,measured.serial,measured.simulation_time)==NearbyMountDecision::waiting,
            "setdown cannot release a second nearby candidate in the same native step");
        check(down.next(key,measured.context,measured.serial+1,measured.simulation_time+1.0/60)==NearbyMountDecision::query,
            "one later native step may release the next bounded candidate");
        // Explicitly synthetic nearby flat ground;184 did not record these rays.
        m::Request request{down.candidate(),down.yaw(),measured.world,measured.context,measured.serial+1,measured.simulation_time+1.0/60};
        auto planes=support;
        for(unsigned i=0;i<2;++i){const auto z=geometry::wheel_hub(i==0)[2];
            planes[i]={true,true,true,{request.position[0]+z*std::sin(request.yaw),request.position[1],
                request.position[2]+z*std::cos(request.yaw)},{0,1,0},request.world,request.context,request.serial,request.simulation_time};}
        const auto prepared=m::prepare(request,planes);
        witness.serial=request.serial;witness.native_time=request.simulation_time;
        check(prepared.prepared&&!down.ready(prepared,witness),"prepared-only next candidate still leaves bike carried");
        for(unsigned i=0;i<2;++i){const auto hub=chassis::rotate_unit(prepared.orientation,geometry::wheel_hub(i==0));
            for(unsigned axis=0;axis<3;++axis)planes[i].point[axis]=prepared.root[axis]+hub[axis];
            planes[i].point[1]-=static_cast<float>(geometry::wheel_radius);}
        const auto ready=m::verify_final(prepared,planes);
        check(ready.ready&&down.ready(ready,witness),"next synthetic candidate selects only after unchanged fresh final support plus approach/body join");
        for(unsigned fault=0;fault<16;++fault){auto bad=witness;
            switch(fault){case 0:bad.coherent_parked_body=false;break;case 1:bad.standing_offboard=false;break;
            case 2:bad.approach_available=false;break;case 3:bad.approach_clear=false;break;
            case 4:++bad.identity.owner.client;break;case 5:++bad.identity.owner.entity;break;case 6:++bad.identity.owner.core;break;
            case 7:++bad.identity.owner.context;break;case 8:++bad.identity.owner.rig;break;case 9:++bad.identity.world;break;
            case 10:++bad.identity.native_state;break;case 11:++bad.identity.body_index;break;case 12:++bad.identity.body_generation;break;
            case 13:++bad.identity.token;break;case 14:++bad.context;break;default:++bad.serial;break;}
            check(!down.ready(ready,bad),"unknown approach/body or any full key/context/clock change cannot clear carried state");}
        auto stale=witness;stale.native_time+=1.0/60;check(!down.ready(ready,stale),"same serial with changed native time cannot select stale support");
        auto changed=key;++changed.token;
        check(down.next(changed,measured.context,request.serial+1,request.simulation_time+1.0/60)==NearbyMountDecision::cancelled&&!down.active(),
            "new body token cancels pending setdown instead of rebinding it");
        check(!down.ready(ready,witness),"cancelled/unknown intent cannot later auto-commit a previously good support witness");
        check(down.begin(key,measured.context,measured.position,measured.yaw)&&
            down.next(key,measured.context+1,measured.serial,measured.simulation_time)==NearbyMountDecision::cancelled&&!down.active(),
            "same-world TLS context change cancels rather than issuing any candidate or clearing carry");
        check(down.begin(key,measured.context,measured.position,measured.yaw),"fresh explicit retry may begin its own bounded setdown");
        for(unsigned i=0;i<NearbyMountSearch::offsets.size();++i){
            check(down.next(key,measured.context,measured.serial+i,measured.simulation_time+i/60.0)==NearbyMountDecision::query,
                "each explicit setdown candidate requires a distinct native clock");
            down.finish_attempt(true);
        }
        check(down.exhausted()&&down.next(key,measured.context,measured.serial+17,measured.simulation_time+17/60.0)==NearbyMountDecision::exhausted,
            "seventeen strict refusals terminate the request with no selected support or forced drop");
    }

    {
        namespace m=mount_pose;
        // Actual215 initial point; no alternate nearby plane was recorded.
        const m::Request measured{{299.1027526855469f,274.36846923828125f,-470.29949951171875f},-2.0466902256011963f,
            9420206880ULL,1217763360ULL,4568,76.13332879543304};
        const std::array<m::Sample,2> support{{
            {true,true,true,{298.69287109375f,274.36053466796875f,-470.5115051269531f},
                {-.031000854447484016f,.9994335770606995f,-.013095506466925144f},
                measured.world,measured.context,measured.serial,measured.simulation_time},
            {true,true,true,{299.5150451660156f,274.3766174316406f,-470.0832214355469f},
                {-.009096065536141396f,.9997782111167908f,-.018993336707353592f},
                measured.world,measured.context,measured.serial,measured.simulation_time}}};
        const auto refused=m::prepare(measured,support);
        check(!refused.prepared&&refused.issue==m::Issue::discontinuous,
            "actual215 gentle local normal difference remains refused by unchanged strict support policy");
        const RetainedBikeIdentity key{rider,measured.world,0x700,0,3,6};
        NearbyMountSearch retained;
        check(retained.begin_retained(key,measured.context,measured.position,measured.yaw),"retained search pins complete body lifetime and TLS context");
        check(retained.next_retained(key,measured.context,rider,measured.serial,measured.simulation_time)==NearbyMountDecision::query&&
            retained.candidate()==measured.position,"first retained query is exact original215 root before nearby fallback");
        check(retained.support().world==key.world&&retained.support().context==measured.context,
            "retained support world/context are pinned before the first native query");
        retained.finish_attempt(true);
        check(retained.index()==1&&retained.next_retained(key,measured.context,rider,measured.serial,measured.simulation_time)==NearbyMountDecision::waiting,
            "measured215 refusal advances once but cannot query another candidate in the same native epoch");
        check(retained.next_retained(key,measured.context,rider,measured.serial+1,measured.simulation_time+1.0/60)==NearbyMountDecision::query,
            "fresh retained epoch may query the next existing bounded offset");
        const auto retained_second=retained.candidate();
        check(std::abs(std::hypot(retained_second[0]-measured.position[0],retained_second[2]-measured.position[2])-.2f)<.0001f&&
            retained_second[1]==measured.position[1],"retained fallback preserves original height and existing nearest sampled radius");
        retained.finish_attempt(false);
        check(retained.next_retained(key,measured.context,rider,measured.serial+2,measured.simulation_time+2.0/60)==NearbyMountDecision::query&&
            retained.candidate()==retained_second,"unavailable/raced retained query neither advances nor recycles a support witness");
        // Synthetic next point only. This is not evidence that215 had this floor.
        m::Request request{retained_second,retained.yaw(),key.world,measured.context,measured.serial+2,measured.simulation_time+2.0/60};
        auto planes=support;
        for(unsigned i=0;i<2;++i){const auto z=geometry::wheel_hub(i==0)[2];
            planes[i]={true,true,true,{request.position[0]+z*std::sin(request.yaw),request.position[1],
                request.position[2]+z*std::cos(request.yaw)},{0,1,0},request.world,request.context,request.serial,request.simulation_time};}
        const auto prepared=m::prepare(request,planes);
        check(prepared.prepared&&!prepared.ready&&!m::matches(prepared,request.world,request.context,request.serial,request.simulation_time),
            "synthetic retained fallback prepared-only plane cannot authorize activation");
        for(unsigned i=0;i<2;++i){const auto hub=chassis::rotate_unit(prepared.orientation,geometry::wheel_hub(i==0));
            for(unsigned axis=0;axis<3;++axis)planes[i].point[axis]=prepared.root[axis]+hub[axis];
            planes[i].point[1]-=static_cast<float>(geometry::wheel_radius);}
        auto bad_final=planes;bad_final[0].hit=false;
        check(!m::verify_final(prepared,bad_final).ready,"missing final actual retained wheel witness still rejects");
        const auto ready=m::verify_final(prepared,planes);
        check(ready.ready&&m::matches(ready,request.world,request.context,request.serial,request.simulation_time)&&
            !m::matches(ready,request.world,request.context,request.serial+1,request.simulation_time+1.0/60),
            "only fresh independent synthetic final support passes; lease is invalid next epoch");
        for(unsigned fault=0;fault<12;++fault){
            check(retained.begin_retained(key,measured.context,measured.position,measured.yaw),"full identity fault fixture starts");
            auto changed=key;auto context=measured.context;auto canonical=rider;
            switch(fault){case 0:++changed.owner.client;break;case 1:++changed.owner.entity;break;case 2:++changed.owner.core;break;
            case 3:++changed.owner.context;break;case 4:++changed.owner.rig;break;case 5:++changed.world;break;
            case 6:++changed.native_state;break;case 7:++changed.body_index;break;case 8:++changed.body_generation;break;
            case 9:++changed.token;break;case 10:++context;break;default:++canonical.core;break;}
            check(retained.next_retained(changed,context,canonical,measured.serial,measured.simulation_time)==NearbyMountDecision::cancelled&&!retained.active(),
                "every body lifetime/TLS/canonical owner replacement cancels; no request can rebind a retained token");
        }
        for(unsigned fault=0;fault<6;++fault){
            check(retained.begin_retained(key,measured.context,measured.position,measured.yaw)&&
                retained.next_retained(key,measured.context,rider,100,2)==NearbyMountDecision::query,"retained clock fault fixture starts");
            std::uint64_t serial=101;double time=2+1.0/60;
            switch(fault){case 0:serial=0;break;case 1:serial=99;break;case 2:time=2;break;
                case 3:serial=113;break;case 4:time=2.11;break;default:time=std::numeric_limits<double>::quiet_NaN();break;}
            check(retained.next_retained(key,measured.context,rider,serial,time)==NearbyMountDecision::cancelled&&!retained.active(),
                "retained search preserves identical zero/backward/nonfinite/gapped native clock refusal");
        }
        check(!retained.begin_retained({},measured.context,measured.position,measured.yaw)&&
            !retained.begin_retained(key,0,measured.position,measured.yaw),"missing full retained identity/context cannot seed search");
        check(retained.begin_retained(key,measured.context,measured.position,measured.yaw),"retained exhaustion fixture starts");
        double previous=0;
        for(unsigned i=0;i<NearbyMountSearch::offsets.size();++i){
            check(retained.next_retained(key,measured.context,rider,measured.serial+i,measured.simulation_time+i/60.0)==NearbyMountDecision::query,
                "every measured retained candidate consumes one fresh native epoch");
            const auto point=retained.candidate();const auto radius=std::hypot(point[0]-measured.position[0],point[2]-measured.position[2]);
            check(radius<=NearbyMountSearch::maximum_radius+.0001f&&radius+.0001>=previous&&point[1]==measured.position[1],
                "retained search stays within identical0.4m radial order and cannot recall a distant bike");
            previous=radius;retained.finish_attempt(true);
        }
        check(retained.exhausted()&&retained.next_retained(key,measured.context,rider,measured.serial+17,measured.simulation_time+17/60.0)==NearbyMountDecision::exhausted,
            "seventeen measured refusals terminate instead of repeating original root until15s");
        RetainedMountCancellation cancel{key,rider,key.world,measured.context,key.token,
            true,true,true,false,true,false,false,false,false,false,false,false,false,true};
        check(retained_mount_cancellation_allowed(cancel),"pending unacquired retained timeout/exhaustion can release SDK admission while retaining identity");
        cancel.active=true;cancel.pending=false;
        check(retained_mount_cancellation_allowed(cancel),"active-before-chassis admission retains the same guarded cancellation");
        const auto saved=cancel.identity;const auto saved_context=cancel.context;
        retained.cancel();
        check(!retained.active()&&cancel.identity==saved&&cancel.context==saved_context&&cancel.token==saved.token,
            "cancelled search drops its intent/epoch without mutating the full parked custody key");
        for(unsigned fault=0;fault<21;++fault){auto bad=cancel;
            switch(fault){case 0:bad.native_chassis=false;break;case 1:bad.parked=false;break;case 2:bad.from_parked=false;break;
            case 3:bad.active=bad.pending=false;break;case 4:bad.carried=true;break;case 5:bad.crashed=true;break;
            case 6:bad.chassis_started=true;break;case 7:bad.rehearsal=true;break;case 8:bad.private_variant=true;break;
            case 9:bad.stepping_off=true;break;case 10:bad.step_off_board=true;break;case 11:bad.board_switch=true;break;
            case 12:bad.pool_parked=false;break;case 13:bad.identity={};break;case 14:++bad.parked_owner.rig;break;
            case 15:++bad.world;break;case 16:bad.context=0;break;case 17:++bad.token;break;
            case 18:bad.identity.native_state=0;break;case 19:bad.identity.body_index=0xffffffffU;break;default:bad.identity.token=0;break;}
            check(!retained_mount_cancellation_allowed(bad),"cancellation never rescues acquired/failed/foreign/private/crashed/carried or missing retained lifetime");
        }
    }

    std::cout << "BMX pending mount readiness tests passed\n";
}
