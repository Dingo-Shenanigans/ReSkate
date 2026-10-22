#include "Engine/Game/BMX/bmx_human_native_return.h"
#include <cstdlib>
#include <iostream>
#include <string_view>
namespace h=dingosdk::bmx::human_native_handoff;
namespace n=dingosdk::bmx::native_input_handoff;
namespace b=dingosdk::input::native_board_bridge;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok){std::cerr<<"FAIL "<<why<<'\n';std::exit(1);}}
n::Sample sample(std::uint64_t wall=1000,unsigned physics=100){
 n::Sample s;s.now=s.sampled_ms=wall;s.serial=wall-990;s.native_time=double(wall)/1000;s.owner={1,2,3,4,5,6};
 s.body={7,8,9,10,11};s.counters={20,21,22,23,24,1,1};s.sdk_device=4;s.physics=physics;
 s.configuration=s.gates=s.canonical=s.clock_coherent=s.pool_released=s.sdk_idle=s.motion_released=s.publication_inactive=s.parts_zero=true;
 s.capture_released=s.sdk_quiet=s.gesture_released=wall>1000;s.offboard=s.standable=s.motion_supported=physics==504;return s;
}
h::AdmissionContext context(const n::Sample& s){return {s.now,s.sampled_ms,s.serial,s.native_time,s.owner,s.sdk_device,true,true};}
h::Guard guard(const n::Sample& s){return {s.now,s.sampled_ms,s.owner,s.sdk_device,true,true,s.gesture_released};}
void note(h::Intent& i,std::uint64_t now,unsigned buttons=0,unsigned slot=2,bool success=true){b::Pad p;p.buttons=std::uint16_t(buttons);i.note(true,slot,success,now,3,p);}
struct State {h::Intent source;h::Return reverse;
 State(bool chord=true,unsigned physics=100){note(source,980);if(chord)note(source,1000,0x8100);auto s=sample(1000,physics);check(reverse.begin(s,context(s),source),"begin original board return");}
 void observe(std::uint64_t now,unsigned physics=100){note(source,now);auto s=sample(now,physics);reverse.observe(s,guard(s),false,source);}
 void queue(){observe(1016);check(!reverse.pulse().used,"first advancing release sample waits");observe(1032);check(reverse.pulse().live(),"second sample queues single reverse pulse");}
 b::Result poll(std::uint64_t now,unsigned physics=100,bool identity=true,bool focus=true,bool ready=true){note(source,now);return reverse.poll(now,2,identity,focus,ready,physics,{},source);}
};
void sequence(){
 for(unsigned initial:{100u,103u}){State s(true,initial);s.queue();check(!s.poll(1032,initial).north,"actual neutral baseline");check(s.poll(1052,initial).north,"one actual North rise");
  check(!s.poll(1068,504).north&&s.reverse.pulse().release_started==1068,"real target starts actual release");
  check(!s.poll(1087,504).north&&!s.reverse.complete(),"19ms release cannot complete");s.poll(1088,504);
  check(s.reverse.phase()==h::ReturnPhase::observing&&!s.reverse.complete(),"report completion is not native consumption");
  s.observe(1100,504);check(s.reverse.complete()&&!s.source.active()&&s.reverse.first504().physics==504,"supported actual504 completes transport only");
  check(s.reverse.baseline().physics==initial&&s.reverse.deadline()==16000,"real board state and original deadline retained");
 }
 {State s;s.queue();s.poll(1032);s.poll(1052);s.poll(1132);s.poll(1212);s.poll(1232);check(s.reverse.phase()==h::ReturnPhase::observing,"wall-forced release complete");
  s.observe(1240);check(!s.reverse.complete(),"input complete without504 remains unproved");s.observe(16000);check(!s.reverse.active()&&!s.reverse.complete(),"original15s expiry never becomes completion");}
}
void waits(){
 for(unsigned prior:{0u,0x100u}){h::Intent i;h::Return r;note(i,1000,prior);auto s=sample();check(r.begin(s,context(s),i),"SDK-before-original can wait neutral/LB");
  check(r.phase()==h::ReturnPhase::admission_wait&&!i.active()&&!r.pulse().used,"pending causes no pulse or active intent");
  note(i,1010,0x8100);r.note(2,true,1010,i);s=sample(1016);s.gesture_released=s.sdk_quiet=s.capture_released=false;
  r.observe(s,guard(s),true,i);check(i.active()&&r.phase()==h::ReturnPhase::release_wait&&r.deadline()==16000,"strict unchanged original policy admits without renewed deadline");}
 for(unsigned mutation=0;mutation<6;++mutation){h::Intent i;h::Return r;note(i,1000,0x100);auto s=sample();check(r.begin(s,context(s),i),"waiting negative setup");
  if(mutation==0){note(i,1010);r.note(2,true,1010,i);note(i,1011,0x8100);r.note(2,true,1011,i);}
  else if(mutation==1){note(i,1010,0,1);r.note(1,true,1010,i);}
  else if(mutation==2){note(i,1010,0,2,false);r.note(2,false,1010,i);}
  else {note(i,1010,0x8100);r.note(2,true,1010,i);}
  s=sample(mutation==3?1251:1016);s.gesture_released=s.sdk_quiet=s.capture_released=false;auto g=guard(s);
  if(mutation==4)g.owner[2]++;if(mutation==5)g.allowed=false;
  r.observe(s,g,mutation!=2,i);check(!r.active()&&!r.complete()&&!r.pulse().used,"pending release/source/deadline/context failure is terminal");}
}
void negatives(){
 for(unsigned mutation=0;mutation<23;++mutation){State state;auto s=sample(1016);note(state.source,1016);auto g=guard(s);
  switch(mutation){case 0:s.counters[0]++;break;case 1:s.body.token++;break;case 2:s.owner[2]++;break;
  case 3:s.configuration=false;break;case 4:s.gates=false;break;case 5:s.canonical=false;break;case 6:s.protected_owner=true;break;
  case 7:s.clock_coherent=false;break;case 8:s.pool_released=false;break;case 9:s.sdk_idle=false;break;case 10:s.motion_released=false;break;
  case 11:s.publication_inactive=false;break;case 12:s.parts_zero=false;break;case 13:s.serial=0;break;
  case 14:s.native_time=std::numeric_limits<double>::quiet_NaN();break;case 15:s.native_time=.9;break;case 16:s.sampled_ms=2000;break;
  case 17:s.sampled_ms=700;break;case 18:s.physics=504;break;case 19:s.physics=200;break;case 20:g.sdk_gesture_only=false;break;
  case 21:s.sdk_device=1;g.device=1;break;case 22:s.serial=10;break;}
  state.reverse.observe(s,g,false,state.source);check(!state.reverse.active()&&!state.reverse.complete(),"negative native/cleanup/source proof aborts without fallback");
 }
 for(unsigned mutation=0;mutation<9;++mutation){State s;s.queue();
  if(mutation==0)s.poll(1032,504);else if(mutation==1)s.poll(1032,100,false);else if(mutation==2)s.poll(1032,100,true,false);
  else if(mutation==3)s.poll(1032,100,true,true,false);else if(mutation==4){s.poll(1032);s.poll(1152);s.poll(1272);s.poll(1282);}
  else if(mutation==5){s.poll(1032);s.poll(1153);}else if(mutation==6){s.poll(1032);s.poll(1031);}
  else if(mutation==7){note(s.source,1032,1);s.reverse.note(2,true,1032,s.source);s.poll(1033);}
  else {s.reverse.cancel(s.source,"explicit_reset");s.poll(1032);}
  check(!s.reverse.active()&&!s.reverse.complete(),"callback/clock/guard/reset failure never completes");
 }
 for(unsigned wrong:{0u,99u,200u,201u,504u}){h::Intent i;h::Return r;note(i,980);note(i,1000,0x8100);auto s=sample(1000,wrong);check(!r.begin(s,context(s),i)&&!i.active(),"admission requires actual board100/103");}
}
void quantized(){
 for(unsigned initial:{100u,103u})for(unsigned offset=0;offset<17;++offset){State s(true,initial);s.queue();
  bool last=false;unsigned rises{};for(unsigned elapsed=offset;elapsed<=260;elapsed+=16){auto r=s.poll(1032+elapsed,initial);
   if(r.rewrite){rises+=r.north&&!last;last=r.north;}if(!s.reverse.pulse().live())break;}
  check(s.reverse.pulse().phase==b::Phase::complete&&rises==1&&!last,"60Hz all start phases one rise and actual20ms release under250ms");
 }
 {b::Pulse p;check(p.queue(1000,0,b::Direction::board_dismount),"reverse queue");p.poll(1000,0,true,true,true,true,100);p.poll(1020,0,true,true,true,true,100);
  p.poll(1140,0,true,true,true,true,100);p.poll(1240,0,true,true,true,true,504);p.poll(1250,0,true,true,true,true,504);
  check(p.phase==b::Phase::aborted&&std::string_view(p.issue)=="transaction_expired","release240 cannot overrun original250 cap");}
}
int main(){sequence();waits();negatives();quantized();std::cout<<checks<<" reverse policy assertions passed\n";}
