#include "Engine/Game/BMX/bmx_coupled_trial_marker_policy.h"
#include <cstdio>
#include <cstdlib>
namespace m=dingosdk::bmx::trial_marker;namespace np=m::np;
unsigned checks{};void check(bool v,const char* issue){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",issue);std::exit(1);}}
m::Identity identity(){m::Identity i;i.released=true;i.process_id=1234;i.creation_filetime=134054832620000000ull;i.released_tick_ms=100000;i.nonce.fill('a');return i;}
np::Epoch epoch(){np::Epoch e;e.owner_world={101,102,103,104,105,106,107,108};e.bodies={np::BodyKey{106,11,0},np::BodyKey{106,12,2}};
    e.token=7;e.serial=100;e.time=1.5;e.seconds=static_cast<float>(np::native_seconds);return e;}
bool activate(m::Producer& p,const np::Epoch& e=epoch(),std::uint64_t begin=101000,std::uint64_t success=101002){return p.activation(begin,success,e,true,true,true,true,true,false);}
int main(){
    m::Producer absent;check(!absent.configured()&&!activate(absent)&&!absent.terminal(),"default producer creates no marker claims");
    for(unsigned fault=0;fault<8;++fault){auto i=identity();std::uint64_t generation=100001;
        if(fault==0)i.released=false;if(fault==1)i.process_id=0;if(fault==2)i.creation_filetime=0;if(fault==3)i.released_tick_ms=0;
        if(fault==4)i.nonce[0]='A';if(fault==5)i.nonce[31]='g';if(fault==6)i.nonce[1]='\0';if(fault==7)generation=0;
        m::Producer p;p.initialize(i,generation);check(!p.configured()&&!activate(p)&&!p.terminal(),"invalid gate/nativeidentity/nonce/generation never publishes markers");}
    m::Producer initialized;initialized.initialize({},0);initialized.initialize(identity(),100001);check(!initialized.configured(),"even invalid first initialization cannot later rearm scope");
    m::Producer good;good.initialize(identity(),100001);check(good.configured()&&activate(good),"actual released gate plus complete post-call proof claims activation once");
    check(good.activated().begun==101000&&good.activated().succeeded==101002&&good.activated().expiry==111000&&good.activated().epoch==epoch(),"conservative expiry binds before-call time and genuine sampled fullEpoch");
    check(!activate(good)&&!activate(good,epoch(),101010,101011),"activation cannot republish or extend its fixed budget");
    for(unsigned fault=0;fault<6;++fault){m::Producer p;p.initialize(identity(),100001);
        const bool ok=p.activation(101000,101002,epoch(),fault!=0,fault!=1,fault!=2,fault!=3,fault!=4,fault==5);
        check(!ok&&!activate(p),"failed call/entry/render/currentpair/backend or normaladoption proof burns one claim");
        check(p.terminal()&&!p.terminal(),"pre-activation failure still permits one actual terminal signal");}
    for(const auto times:{std::array<std::uint64_t,2>{99999,100000},{100000,100000},{101000,100999},{101000,111000},
        {101000,111001},{std::numeric_limits<std::uint64_t>::max()-9999,std::numeric_limits<std::uint64_t>::max()-9998}}){m::Producer p;p.initialize(identity(),100001);
        check(!activate(p,epoch(),times[0],times[1]),"generation/release rewind, failure-success order, exact expiry and addition overflow refuse");}
    for(unsigned fault=0;fault<5;++fault){auto e=epoch();if(fault==0)e.owner_world[7]=0;if(fault==1)e.bodies[1].index=e.bodies[0].index;
        if(fault==2)e.seconds=std::nextafter(e.seconds,1.f);if(fault==3)e.time=std::numeric_limits<double>::quiet_NaN();if(fault==4)e.token=0;
        m::Producer p;p.initialize(identity(),100001);check(!activate(p,e),"invalid fullpair/nativeh/clock proof refuses");}
    check(good.terminal_epoch(epoch()),"terminal retains same qualified pair");
    for(unsigned fault=0;fault<13;++fault){auto e=epoch();if(fault<8)++e.owner_world[fault];if(fault==8)++e.bodies[0].generation;
        if(fault==9)++e.bodies[1].generation;if(fault==10)++e.bodies[0].index;if(fault==11)++e.bodies[1].index;if(fault==12)++e.token;
        check(!good.terminal_epoch(e),"terminal cannot silently substitute owner/body/token identity");}
    auto later=epoch();++later.serial;later.time+=later.seconds;check(good.terminal_epoch(later),"later genuine epoch may retain trial identity");
    check(good.terminal()&&!good.terminal()&&!activate(good),"terminal claim is one-shot and closes activation");
    m::Producer early;early.initialize(identity(),100001);check(early.terminal()&&!activate(early),"terminal before activation aborts without manufactured readiness");
    m::Trace<unsigned> trace;
    for(unsigned i=0;i<700;++i)trace.append(i,true);
    check(trace.active_total==700&&trace.active_overflow&&trace.active.front()==0&&trace.active.back()==639,"first640 active rows retained completely with explicit overflow");
    for(unsigned i=0;i<52;++i)trace.append(i,false);
    check(trace.entry_total==52&&trace.entry_overflow&&trace.entry[4]==4&&trace.entry[3]==51,"entry keeps bounded48-row tail with honest truncation");
    for(unsigned i=0;i<18;++i)trace.event(i);
    check(trace.event_total==18&&trace.event_overflow&&trace.events.front()==0&&trace.events.back()==15,"event capacity cannot silently overwrite or grow");
    m::Trace<unsigned> exact;for(unsigned i=0;i<640;++i)exact.append(i,true);for(unsigned i=0;i<48;++i)exact.append(i,false);for(unsigned i=0;i<16;++i)exact.event(i);
    check(!exact.active_overflow&&!exact.entry_overflow&&!exact.event_overflow,"exact capacities are complete rather than overflow");
    std::printf("coupled trial marker one-shot identity/time/trace policy: %u checks PASS\n",checks);return 0;
}
