#include "Engine/Game/BMX/bmx_owner_observation.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
namespace o=dingosdk::bmx::owner_observation;
void require(bool condition,const char* text) {if(!condition){std::cerr<<text<<'\n';std::exit(1);}}
struct ThrowingCopy {
    unsigned* copies{};
    explicit ThrowingCopy(unsigned& count):copies(&count){}
    ThrowingCopy(const ThrowingCopy& source):copies(source.copies){if(++*copies==2)throw std::runtime_error("second copy");}
};
int main() {
    const o::Owner owner{1,2,3,4,5};const o::WorldKey world{10,20,30};
    const o::Observation live{true,true,owner,world};
    // Exhaustive state admission: observing an unknown rider cannot turn an
    // active, partial, failed or producer-failed pool into a retryable lifetime.
    for(unsigned bits=0;bits<128;++bits) {
        const o::RetainedState pool{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8),
            bool(bits&16),bool(bits&32),bool(bits&64)};
        const bool expected=bits==5||bits==9; // ready+parked or ready+detached
        require(o::may_suspend(pool)==expected,"only ready healthy released parked/detached states may suspend");
    }
    require(o::evaluate(owner,world,live)==o::Result::matched,"complete same rider and full world match");
    {
        using namespace dingosdk::bmx;
        std::atomic<int> request{7};BikeInteractionButton north;EquipmentToggleKey toggle,interact,carry;BikeQuickSwitchButton quick;
        const BikeQuickSwitchOwner quick_owner{1,2,3,4,5};
        (void)quick.update(false,false,true,1,false,{},1);
        require(quick.update(true,true,true,1,true,quick_owner,2).pressed,"quick shortcut was deliberate before suspension");
        require(north.update(false,true,1)==BikeInteraction::none,"neutral arms interaction before suspension");
        require(north.update(true,true,2)==BikeInteraction::none,"press is pending before unknown observation");
        for(auto* key:{&toggle,&interact,&carry})require(!key->update(false,true),"neutral arms key before suspension");
        require(o::reset_interactions(request,north,toggle,interact,carry,&quick)==7&&request.load()==0,"first unknown cancels queued interact");
        request.store(8); // arrives after the final waiting drain
        require(o::reset_interactions(request,north,toggle,interact,carry,&quick)==8&&request.load()==0,"fresh quiescent tick cancels late queued carry");
        request.store(4);
        require(o::reset_interactions(request,north,toggle,interact,carry,&quick)==4&&request.load()==0,"queued toggle cannot escape quarantine");
        require(!quick.update(true,true,true,1,true,quick_owner,10000).pressed,
            "owner suspension discards a held equipment shortcut and its arming");
        (void)quick.update(false,false,true,1,true,quick_owner,10001);
        require(quick.update(true,true,true,1,true,quick_owner,10002).pressed,
            "only fresh post-suspension release and chord may switch");
        require(north.update(true,true,10000)==BikeInteraction::none,"held button cannot spend unknown time into a hold");
        require(north.update(false,true,10001)==BikeInteraction::none,"releasing a pre-quarantine press cannot become a tap");
        for(auto* key:{&toggle,&interact,&carry}) {
            require(!key->update(true,true),"held key after quarantine cannot dispatch");
            require(!key->update(false,true),"fresh neutral rearms without dispatch");
            require(key->update(true,true),"a new deliberate press after fresh neutral dispatches once");
            require(!key->update(true,true),"remaining held cannot dispatch twice");
        }
        require(north.update(true,true,10002)==BikeInteraction::none,"new deliberate button press starts after neutral");
        require(north.update(false,true,10100)==BikeInteraction::tap,"only a new post-neutral press may become a tap");
    }
    for(unsigned field=0;field<5;++field) {
        auto changed=live;
        if(field==0)++changed.owner.client;else if(field==1)++changed.owner.entity;
        else if(field==2)++changed.owner.core;else if(field==3)++changed.owner.context;else ++changed.owner.rig;
        require(o::evaluate(owner,world,changed)==o::Result::changed,"each complete owner replacement is terminal");
        changed.world_available=false;
        require(o::evaluate(owner,world,changed)==o::Result::changed,"world read failure cannot conceal a proved owner replacement");
        changed.rider_available=false;
        require(o::evaluate(owner,world,changed)==o::Result::unavailable,"values from an unavailable read are not identity proof");
    }
    for(unsigned field=0;field<5;++field) {
        auto missing=live;
        if(field==0)missing.owner.client=0;else if(field==1)missing.owner.entity=0;
        else if(field==2)missing.owner.core=0;else if(field==3)missing.owner.context=0;else missing.owner.rig=0;
        require(o::evaluate(owner,world,missing)==o::Result::unavailable,"every incomplete canonical owner stays unknown");
    }
    for(unsigned field=0;field<3;++field) {
        auto changed=live;
        if(field==0)++changed.world.world;else if(field==1)++changed.world.state;else ++changed.world.context;
        require(o::evaluate(owner,world,changed)==o::Result::changed,"each full world component independently fences reuse");
        changed.world_available=false;
        require(o::evaluate(owner,world,changed)==o::Result::unavailable,"unavailable world bytes cannot establish change or reuse");
    }
    const auto before=live;
    for(unsigned count=0;count<10000;++count) {
        auto absent=live;absent.rider_available=false;
        require(o::evaluate(owner,world,absent)==o::Result::unavailable,"repeated unknown never becomes matched by elapsed attempts");
    }
    require(live.owner==before.owner&&live.world==before.world,"policy cannot adopt a new owner or mutate retained metadata");
    require(o::evaluate(owner,world,live)==o::Result::matched,"fresh same owner may qualify after arbitrarily many unavailable reads");
    require(o::evaluate({},world,live)==o::Result::unavailable,"missing retained owner never authorizes adoption");
    require(o::evaluate(owner,{},live)==o::Result::unavailable,"missing retained world never authorizes adoption");

    {
        const o::Owner recreated{1,2,3,40,50};
        o::Observation fresh{true,true,recreated,world};
        require(o::classify_recreation(owner,world,fresh)==o::Recreation::candidate,"only complete same-actor context+rig replacement is candidate");
        for(unsigned field=0;field<3;++field){auto wrong=fresh;
            if(field==0)++wrong.owner.client;else if(field==1)++wrong.owner.entity;else ++wrong.owner.core;
            require(o::classify_recreation(owner,world,wrong)==o::Recreation::terminal,"different actor remains terminal");}
        for(unsigned field=0;field<3;++field){auto wrong=fresh;
            if(field==0)++wrong.world.world;else if(field==1)++wrong.world.state;else ++wrong.world.context;
            require(o::classify_recreation(owner,world,wrong)==o::Recreation::terminal,"each full-world lane fences handoff");}
        auto one=fresh;one.owner.context=owner.context;
        require(o::classify_recreation(owner,world,one)==o::Recreation::terminal,"rig-only replacement is outside contract");
        one=fresh;one.owner.rig=owner.rig;
        require(o::classify_recreation(owner,world,one)==o::Recreation::terminal,"context-only replacement is outside contract");
        one=fresh;one.world_available=false;
        require(o::classify_recreation(owner,world,one)==o::Recreation::unknown,"missing current world is never custody proof");
        one=fresh;one.rider_available=false;
        require(o::classify_recreation(owner,world,one)==o::Recreation::unknown,"missing canonical owner is never recreated proof");
        require(o::classify_recreation(owner,world,live)==o::Recreation::unchanged,"same owner does not spend a transfer token");
        const o::ReleasedKey key{owner,world,365,50331648,1};
        o::ReleaseWitness witness{key,true,false,true};
        require(o::witness_matches(witness,key,false,true),"exact detached retained identity matches witness");
        for(unsigned field=0;field<9;++field){auto wrong=key;
            if(field==0)++wrong.owner.context;else if(field==1)++wrong.owner.rig;else if(field==2)++wrong.world.world;
            else if(field==3)++wrong.world.state;else if(field==4)++wrong.world.context;else if(field==5)++wrong.body_index;
            else if(field==6)++wrong.body_generation;else if(field==7)++wrong.token;else ++wrong.owner.core;
            require(!o::witness_matches(witness,wrong,false,true),"no witness lane may be borrowed or rebound");}
        require(!o::witness_matches(witness,key,true,false),"parked cannot borrow detached witness");
        for(unsigned bits=0;bits<2048;++bits){o::RuntimeRelease r;
            r.bike_crashed=true;r.active=bool(bits&1);r.pending=bool(bits&2);r.chassis_started=bool(bits&4);
            r.carried=bool(bits&8);r.stepping_off=bool(bits&16);r.step_off_intent=bool(bits&32);
            r.board_switch=bool(bits&64);r.rehearsal=bool(bits&128);r.mounting=bool(bits&256);
            r.bike_parked=bool(bits&512);if(bits&1024)r.bike_crashed=false;
            const bool expected=bits==0||bits==1536;
            require(o::runtime_released(r)==expected,"paused active, carry, partial mount and transitions cannot witness custody");}
        o::RecreationSuffix suffix;
        require(!suffix.observe(recreated,10,1),"first complete custody observation cannot adopt");
        require(!suffix.observe(recreated,10,1),"same epoch cannot be counted twice");
        require(!suffix.observe(recreated,11,1.1),"duplicate epoch reset requires a new first proof");
        require(suffix.observe(recreated,12,1.2),"later complete observation permits caller's final proof");
        suffix.reset();require(!suffix.observe(recreated,20,2),"reset discards previous suffix");
        auto another=recreated;++another.rig;++another.context;
        require(!suffix.observe(another,21,2.1),"changed candidate requires a new full suffix");
        require(!suffix.observe(another,20,2),"regressed serial cannot qualify");
        o::MotionEpoch epoch;const auto copied=epoch.value();
        require(epoch.valid(copied),"copied motion lease initially current");
        require(epoch.revoke()&&!epoch.valid(copied),"revocation rejects a lease copied before busy acquisition");
        const auto fresh_epoch=epoch.value();require(epoch.valid(fresh_epoch),"new generation can be explicitly submitted later");
        require(epoch.revoke()&&!epoch.valid(fresh_epoch),"second revocation cannot revive previous generation");
        require(!epoch.valid(0),"uninitialized revision never authorizes motion");
        o::MotionEpoch exhausted{std::numeric_limits<std::uint64_t>::max()};
        require(!exhausted.revoke()&&!exhausted.valid(exhausted.value()),"revision overflow fails closed instead of reusing an old lease");
        for(unsigned bits=0;bits<8;++bits)
            require(o::custody_gameplay(bool(bits&1),bool(bits&2),bool(bits&4))==(bits==3),"loading/menu must stop before native custody reads");
        const auto last=std::numeric_limits<std::uint64_t>::max();
        require(!o::custody_token_headroom(0),"uninitialized token cannot enter custody");
        require(o::custody_token_headroom(1),"ordinary custody reserves its later mount");
        require(o::custody_token_headroom(last-3),"two free tokens and preceding values qualify");
        require(o::custody_token_headroom(last-2),"last safe custody token permits distinct T+1 and T+2");
        require(!o::custody_token_headroom(last-1),"custody cannot consume the final token needed by explicit mount");
        require(!o::custody_token_headroom(last),"exhausted token cannot qualify");
        const auto custody_token=(last-2)+1;
        const auto mounted_token=custody_token+1;
        require(custody_token!=last-2&&mounted_token!=custody_token&&mounted_token==last,"accepted boundary never wraps or aliases either association");
        const o::CustodyReadiness ready{true,true,true,20,2,.016666666f};
        require(o::custody_readiness_matches(ready,ready),"same final ready epoch accepted");
        for(unsigned field=0;field<6;++field){auto changed=ready;
            if(field==0)changed.timing=false;else if(field==1)changed.offboard=false;else if(field==2)changed.standable=false;
            else if(field==3)++changed.serial;else if(field==4)changed.time+=.01;else changed.step=.02f;
            require(!o::custody_readiness_matches(ready,changed),"final eligibility cannot borrow earlier readiness");}
        o::ReleasedUnwind unwind;unwind.begin_tick();bool suspended=true;
        unwind.stop();suspended=false;
        require(!suspended&&unwind.stopped(),"clearing suspension during nested commit cannot release the outer local request");
        unwind.begin_tick();require(!unwind.stopped(),"only the next tick clears the handoff unwind");
        unsigned copies{};ThrowingCopy source{copies};bool committed=false;
        try{const auto prepared=o::prepare_association(source);(void)prepared;committed=true;}
        catch(const std::runtime_error&){}
        require(copies==2&&!committed,"throwing second NativeFrame-style copy cannot reach association commit");
    }

    std::cout<<"BMX unavailable observation identity policy tests passed.\n";
}
