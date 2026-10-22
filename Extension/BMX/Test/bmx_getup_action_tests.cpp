#include "Extension/BMX/bmx_getup_action.h"
#include "Extension/BMX/bmx_intent_pulse.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace action=dingosdk::bmx::getup_action;
namespace intent=dingosdk::bmx::intent_observer::detail;
namespace {
void require(bool condition,const char* issue) {
    if(!condition){std::cerr<<issue<<'\n';std::exit(1);}
}
action::Permission permitted() {return {true,false,true,true,true,true,true,true,true,1000,1001,1000};}
void guards() {
    const auto good=permitted();require(action::may_start(good),"Qualified lab permission should start.");
    for(unsigned guard=0;guard<9;++guard) {
        auto p=good;
        switch(guard) {
        case 0:p.eligible=false;break;case 1:p.cancelled=true;break;case 2:p.owner_same=false;break;
        case 3:p.unprotected=false;break;case 4:p.known_ragdoll=false;break;case 5:p.motion_released=false;break;
        case 6:p.bmx_released=false;break;case 7:p.interaction_released=false;break;case 8:p.body_released=false;break;
        }
        require(!action::may_start(p),"A missing independent native prerequisite must reject start.");
    }
    auto p=good;p.now=1250;require(!action::may_start(p),"Do not lengthen a 250ms start window.");
    p=good;p.now=999;require(!action::may_start(p),"Wall-clock reversal must reject start.");
    p=good;p.eligibility_observed=749;require(!action::may_start(p),"Stale client eligibility must reject start.");
    p=good;p.eligibility_observed=1002;require(!action::may_start(p),"Future client eligibility must reject start.");
    require(action::epoch({600,10},{601,10.016666}),"A later monotonic native edge belongs to the request epoch.");
    require(action::epoch({600,10},{600,10}),"The exact request edge is allowed.");
    require(!action::epoch({600,10},{599,10.016666}),"Serial rewind is not recovery.");
    require(!action::epoch({600,10},{601,9.9}),"Native-time rewind is not recovery.");
    require(!action::epoch({600,10},{600,10.016666}),"One serial cannot name two native times.");
    require(!action::epoch({600,10},{601,10}),"A new serial without advancing native time is uncertain.");
    require(!action::epoch({0,10},{601,10.016666}),"An absent request epoch must reject.");
    require(!action::epoch({600,10},{601,std::numeric_limits<double>::quiet_NaN()}),"Nonfinite clock must reject.");
    require(!action::exact({600,10},{600,10.016666}),"Transaction may not drift to another clock.");
}
void one_pulse_and_independent_cleanup() {
    intent::Pulse pulse;require(pulse.rearm(true),"Fresh action must reserve one generation.");
    bool value=false,allowed=true,owner_same=true;unsigned true_calls=0,false_calls=0;
    auto permission=permitted();
    const auto dispatch=[&](std::uint64_t serial,std::uint64_t now,bool cancel) {
        permission.now=now;permission.cancelled=cancel;
        pulse.dispatch({0x10000,0x4000000000000010ULL,serial},1000,now,action::may_start(permission),cancel,!owner_same,
            [](std::uintptr_t& property){property=0x20000;return true;},
            [&](bool cleanup){return owner_same&&(cleanup||(action::may_start(permission)&&allowed));},
            [&](auto,auto,std::uint8_t& result){result=value?1:0;return true;},
            [&](auto,auto,std::uint8_t next){value=next!=0;if(next)++true_calls;else ++false_calls;return true;});
    };
    allowed=false;dispatch(600,1001,false);require(!true_calls,"Unmeasured/false native Allowed must never enter.");
    allowed=true;dispatch(601,1017,false);require(true_calls==1&&value&&pulse.written.load(),"Fresh Allowed permits exactly one logical request.");
    dispatch(601,1018,true);require(value&&!false_calls,"Cancellation cannot reset on the write edge.");
    allowed=false;permission.eligible=false;permission.known_ragdoll=false;permission.motion_released=false;
    dispatch(602,9000,true);require(!value&&false_calls==1&&pulse.reset_false.load(),"Cancellation, elapsed start window and native recovery cannot erase cleanup duty.");
    dispatch(603,9017,false);require(true_calls==1&&false_calls==1,"Completed action must not repeat.");
}
void preserve_preexisting_and_owner_loss() {
    intent::Pulse pulse;require(pulse.rearm(true),"Fresh prior-true action must reserve.");
    unsigned writes=0;
    const auto resolve=[](std::uintptr_t& p){p=0x20000;return true;};
    const auto qualify=[](bool){return true;};
    const auto prior_true=[](auto,auto,std::uint8_t& out){out=1;return true;};
    const auto set=[&](auto,auto,std::uint8_t){++writes;return true;};
    pulse.dispatch({0x10000,99,1},1000,1001,true,false,false,resolve,qualify,prior_true,set);
    require(!writes&&pulse.phase.load()==intent::PulsePhase::preexisting_true,"Preexisting native GetUp must remain untouched.");
    intent::Pulse pending;require(pending.rearm(true),"Fresh owner-loss action must reserve.");bool value=false;
    const auto get=[&](auto,auto,std::uint8_t& out){out=value?1:0;return true;};
    const auto put=[&](auto,auto,std::uint8_t next){++writes;value=next!=0;return true;};
    pending.dispatch({0x10000,99,1},1000,1001,true,false,false,resolve,qualify,get,put);
    pending.dispatch({0x30000,100,2},1000,1017,false,true,true,resolve,qualify,get,put);
    require(value&&writes==1&&pending.phase.load()==intent::PulsePhase::owner_lost,"Replacement owner must never receive an old reset.");
    require(!pending.rearm(true),"An uncertain written generation cannot be discarded by rearm.");
}
}
int main(){guards();one_pulse_and_independent_cleanup();preserve_preexisting_and_owner_loss();std::cout<<"BMX GetUp action policy tests passed.\n";}
