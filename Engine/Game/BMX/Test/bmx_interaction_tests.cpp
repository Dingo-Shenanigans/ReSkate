#include "Engine/Game/BMX/bmx_interaction.h"
#include <cstdlib>
#include <iostream>
#include <utility>
using namespace dingosdk::bmx;
void check(bool ok){if(!ok)std::exit(1);}
void plain_north(){
    BikeInteractionButton b;using E=BikeInteraction;
    check(b.update(true,true,1)==E::none);check(b.update(true,true,600)==E::none);
    check(b.update(false,true,601)==E::none);check(b.update(true,true,602)==E::none);
    check(b.update(false,true,800)==E::tap);check(b.update(false,true,801)==E::none);
    check(b.update(true,true,900)==E::none);check(b.update(true,true,1399)==E::none);
    check(b.update(true,true,1400)==E::hold);check(b.update(true,true,3000)==E::none);
    check(b.update(false,true,3001)==E::none);check(b.update(true,true,3002)==E::none);
    check(b.update(true,false,3100)==E::none);check(b.update(true,true,4000)==E::none);
    check(b.update(false,true,4001)==E::none);check(b.update(true,true,4002)==E::none);
    check(b.update(false,true,3999)==E::none);check(b.update(false,true,4003)==E::none);
}
void require(bool ok,const char* text){if(!ok){std::cerr<<text<<'\n';std::exit(1);}}
void quick_switch(){
    const BikeQuickSwitchOwner owner{1,2,3,4,5},other{1,2,3,4,6};
    BikeQuickSwitchButton q;std::uint64_t now=1;
    const auto sample=[&](bool north,bool shoulder,bool allowed=true,unsigned device=1,
                          bool observed=true,BikeQuickSwitchOwner current=BikeQuickSwitchOwner{1,2,3,4,5}) {
        return q.update(north,shoulder,allowed,device,observed,current,now++);
    };
    auto e=sample(true,true);require(!e.pressed,"preheld startup chord never switches");
    sample(true,true);sample(false,false);
    e=sample(true,true);require(e.pressed&&e.suppress_north&&e.capture_north,"simultaneous fresh L1+North switches once");
    for(unsigned i=0;i<100;++i){e=sample(true,true);require(!e.pressed&&e.suppress_north&&e.capture_north,"held chord neither repeats nor leaks tap/hold");}
    e=sample(true,false);require(e.capture_north&&e.suppress_north,"releasing modifier first keeps North owned");
    e=sample(false,false);require(e.capture_north&&!e.pressed,"release tick still filters North");
    e=sample(false,false);require(!e.capture_north,"next neutral sample ends only shortcut capture");
    e=sample(false,true,false);require(!e.pressed&&!e.capture_north,"menu/focus loss releases shortcut lease");
    sample(false,false);e=sample(false,true,true,1,false);
    require(e.capture_north&&!q.owner_sample_needed(false,true),"modifier prearms North without native owner polling");
    require(q.owner_sample_needed(true,true),"fresh chord asks for a canonical owner observation");
    e=sample(true,true);require(e.pressed,"modifier then fresh North switches without a delay");
    sample(false,false);sample(false,false);
    BikeInteractionButton ordinary;ordinary.update(false,true,now);
    e=sample(true,false);require(!e.pressed&&!e.suppress_north,"ordinary North press remains ordinary");
    ordinary.update(true,true,now);e=sample(true,true);
    require(!e.pressed&&!e.suppress_north,"adding shoulder to already-held North cannot switch");
    require(ordinary.update(true,true,now+500)==BikeInteraction::hold,"existing carry hold survives late shoulder");
    sample(false,false);sample(false,false);
    e=sample(true,true,true,1,true,other);require(!e.pressed&&e.suppress_north,"owner replacement consumes attempted chord without switching");
    e=sample(true,true,true,1,true,other);require(!e.pressed,"new owner cannot spend a held old-owner chord");
    sample(false,false,true,1,true,other);sample(false,false,true,1,true,other);
    e=sample(true,true,true,1,true,other);require(e.pressed,"new release and press may switch for fresh owner");
    sample(false,false);sample(false,false);
    e=sample(true,true,true,2);require(!e.pressed,"device replacement cannot spend existing neutral arming");
    sample(false,false,true,2);e=sample(true,true,true,2);require(e.pressed,"new device fresh release and chord work");
    sample(true,true,false,2);e=sample(true,true,true,2);require(!e.pressed,"menu-held chord is not queued on return");
    sample(false,false,true,2);e=sample(true,true,true,2,true,{});
    require(!e.pressed&&e.suppress_north,"unknown owner consumes attempted chord without ordinary Y action");
    e=sample(true,true,true,2);require(!e.pressed,"a later owner observation cannot fire unknown-owner intent");
    sample(false,false,true,2);sample(false,false,true,2);
    now=1;e=sample(true,true,true,2);require(!e.pressed,"clock rollback discards arming");
    sample(false,false,true,2);e=sample(true,true,true,2);require(e.pressed,"fresh release after rollback restores deliberate input");
    q.reset();sample(false,false);sample(false,true,true,1,false);
    e=sample(true,true,true,1,false);require(!e.pressed,"shortcut cannot execute without current owner proof");
    require(owner.valid()&&!BikeQuickSwitchOwner{}.valid(),"complete owner identity is required");
    require(quick_bike_action(true,false,false,false,false)==QuickBikeAction::skateboard,"riding shortcut selects existing skateboard lifecycle");
    require(quick_bike_action(false,false,false,true,false)==QuickBikeAction::nearby_bike,"parked/walking shortcut mounts rather than equips board");
    require(quick_bike_action(false,false,false,false,true)==QuickBikeAction::nearby_bike,"crash shortcut keeps existing recovery handshake");
    require(quick_bike_action(false,false,false,false,false)==QuickBikeAction::equip_bike,"native board/walking shortcut enters automatic dismount/mount lifecycle");
    require(quick_bike_action(true,false,true,false,false)==QuickBikeAction::none&&
        quick_bike_action(false,true,false,true,false)==QuickBikeAction::none,"pending or incomplete mounting cannot queue a later switch");
}
void combined_quarantine(){
    const BikeQuickSwitchOwner owner{1,2,3,4,5},other{1,2,3,4,6};
    // Exercise the actual composition used by runtime: the old ordinary
    // latch can be armed even when the quick source is replaced/invalidated.
    for(unsigned failure=0;failure<7;++failure)for(bool hold:{false,true}) {
        BikeQuickSwitchButton q;BikeInteractionButton ordinary;
        unsigned device=1;std::uint64_t input_clock=100,ordinary_clock=100;
        bool allowed=true;BikeQuickSwitchOwner observed=owner;
        const auto step=[&](bool north,bool shoulder) {
            const bool need=q.owner_sample_needed(north,shoulder);
            const auto e=q.update(north,shoulder,allowed,device,need,observed,input_clock++);
            const auto interaction=ordinary.update(north,allowed&&!e.suppress_north,ordinary_clock++);
            return std::pair{e,interaction};
        };
        step(false,false);
        if(failure==0)device=2;
        if(failure==1){device=2;step(false,true);}
        if(failure==2)input_clock=5; // ordinary clock deliberately stays monotonic
        if(failure==3){allowed=false;step(true,true);allowed=true;}
        if(failure==4)observed={};
        if(failure==5) {
            require(step(true,true).first.pressed,"owner-change fixture first binds a deliberate owner");
            step(false,false);step(false,false);observed=other;
        }
        if(failure==6){q.reset();} // owner-suspension reset may precede ordinary callback
        auto event=step(true,true);
        require(!event.first.pressed&&event.first.suppress_north&&event.first.capture_north&&event.second==BikeInteraction::none,
            "invalidated chord is quarantined from both equipment and ordinary North actions");
        if(hold) {
            input_clock+=600;ordinary_clock+=600;
            event=step(true,true);
            require(!event.first.pressed&&event.first.suppress_north&&event.second==BikeInteraction::none,
                "invalidated chord never becomes a500ms carry or deferred switch");
            event=step(true,false);
            require(event.first.suppress_north&&event.first.capture_north&&event.second==BikeInteraction::none,
                "releasing modifier first cannot leak invalidated North into ordinary interaction");
        }
        observed=failure==5?other:owner;
        event=step(false,false);
        require(!event.first.pressed&&event.first.capture_north&&event.second==BikeInteraction::none,
            "invalidated chord release never becomes an ordinary tap");
        event=step(false,false);require(!event.first.capture_north,"fresh neutral ends quarantined North capture");
        event=step(true,false);require(event.second==BikeInteraction::none&&!event.first.suppress_north,
            "new unmodified North starts ordinary interaction after quarantine");
        event=step(false,false);require(event.second==BikeInteraction::tap,
            "a new deliberate ordinary tap remains usable after quarantine");
    }
    // The different same-context case must remain ordinary: Y was already
    // down before LB, so it is not a fresh shortcut chord.
    BikeQuickSwitchButton q;BikeInteractionButton ordinary;
    q.update(false,false,true,1,false,{},100);ordinary.update(false,true,100);
    auto e=q.update(true,false,true,1,false,{},101);
    require(!e.suppress_north&&ordinary.update(true,true,101)==BikeInteraction::none,
        "ordinary North starts before late modifier");
    e=q.update(true,true,true,1,false,{},601);
    require(!e.pressed&&!e.suppress_north&&ordinary.update(true,!e.suppress_north,601)==BikeInteraction::hold,
        "late shoulder on held same-source North preserves ordinary carry");
}
int main(){plain_north();quick_switch();combined_quarantine();std::cout<<"BMX interaction and quick equipment shortcut tests passed\n";}

