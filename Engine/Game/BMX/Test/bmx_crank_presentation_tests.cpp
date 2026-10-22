#include "Engine/Game/BMX/bmx_crank_presentation.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace c=dingosdk::bmx::crank_presentation;
using dingosdk::bmx::geometry::left_pedal;
using dingosdk::bmx::geometry::bottom_bracket;
namespace {
unsigned checks{};
void check(bool ok,const char* text){++checks;if(!ok){std::cerr<<text<<'\n';std::exit(1);}}
bool near(double a,double b,double epsilon=1e-11){return std::abs(c::wrap(a-b))<epsilon;}
struct Ride {
 c::State state;
 c::Observation sample{{{1,2,3,4,5,6},7},-2.6332650184631348,0,1.0/60,1,true,false,false};
 c::Result tick(){const auto r=c::step(state,sample);state=r.state;++sample.serial;sample.time+=sample.seconds;return r;}
};
}
int main(){
 // Independent component-rotation oracle, including the smaller normalization.
 for(double factor:{1.0,.2630808/.34}){
  const std::array<double,3> a{(double(left_pedal.spindle[0])-bottom_bracket[0])*factor,
    (double(left_pedal.spindle[1])-bottom_bracket[1])*factor,(double(left_pedal.spindle[2])-bottom_bracket[2])*factor};
  const double angle=c::horizontal_angle(a);
  check(std::abs(a[1]*std::cos(angle)-a[2]*std::sin(angle))<1e-14,"measured source crank is horizontal at derived angle");
  check(near(angle,c::source_horizontal_angle()),"uniform bike normalization preserves horizontal crank angle");
 }
 for(int start=-64;start<=64;++start){
  Ride r;r.sample.raw_phase=start*.13;
  const auto first=r.tick();check(first.valid&&first.rebased&&!first.advanced&&near(r.state.phase,r.sample.raw_phase),"new owned body seeds its own raw phase without immediate leveling");
  double movement{};double locked{};
  for(int n=0;n<40;++n){const double old=r.state.phase;const auto result=r.tick();
   check(result.valid&&result.advanced&&std::abs(c::wrap(r.state.phase-old))<=c::leveling_rate*r.sample.seconds+1e-12,"coast rotation is bounded by accepted native time");
   if(n==0)locked=r.state.target;else check(near(locked,r.state.target),"coast episode keeps the same front-foot choice");
   movement+=std::abs(c::wrap(r.state.phase-old));
  }
  check(movement<=c::pi/2+1e-11&&std::abs(std::remainder(r.state.phase-c::source_horizontal_angle(),c::pi))<1e-11,"nearest half-turn target reaches level without extra revolution");
 }
 {Ride r;r.tick();for(int i=0;i<29;++i)r.tick();const double displayed=r.state.phase;
  check(near(displayed,-4.04962287,2e-6),"captured frozen cadence chooses the nearest left-foot-rear horizontal stance");
  r.sample.pedaling=true;r.sample.raw_phase+=.21;const auto result=r.tick();
  check(result.valid&&!result.state.coasting&&near(r.state.phase,displayed+.21),"pedaling resumes with raw delta rather than snap back to original source phase");
 }
 {Ride r;r.sample.raw_phase=3.13;r.sample.pedaling=true;r.tick();r.sample.raw_phase=-3.12;const auto x=r.tick();check(near(x.state.phase,-3.12),"wrapped raw cadence follows short increment across pi");}
 {Ride r;r.tick();for(int i=0;i<6;++i)r.tick();const double phase=r.state.phase;r.sample.reserved=true;
  for(int i=0;i<20;++i){r.sample.raw_phase+=.04;const double before=r.state.phase;const auto x=r.tick();check(!x.leveling&&!x.state.coasting&&near(x.state.phase,before+.04),"authored crank/released feet retain raw delta without auto-leveling");}
  check(!near(r.state.phase,phase),"reserved raw cadence still progresses");
 }
 {Ride r;r.tick();r.tick();const auto old=r.state;auto duplicate=r.sample;duplicate.serial=old.serial;duplicate.time=old.time;duplicate.raw_phase+=.8;
  const auto x=c::step(old,duplicate);check(x.valid&&!x.advanced&&x.state.phase==old.phase&&x.state.raw_phase==old.raw_phase&&x.state.serial==old.serial,"duplicate render/native polls consume neither cadence nor leveling time");
  duplicate.time+=.01;const auto y=c::step(old,duplicate);check(!y.valid&&y.state.phase==old.phase&&!y.state.observed,"contradictory same-serial clock cannot advance presentation");
 }
 {Ride r;r.tick();r.tick();const auto old=r.state;r.sample.seconds=0;r.sample.raw_phase+=.8;const auto x=r.tick();check(x.valid&&!x.advanced&&x.state.phase==old.phase&&x.state.raw_phase==old.raw_phase&&x.state.serial==old.serial,"zero native time freezes all state");}
 {Ride r;r.tick();r.tick();const auto old=r.state;r.state=c::suspend(r.state);r.sample.serial+=40;r.sample.time+=2;r.sample.raw_phase+=1;
  const auto x=r.tick();check(x.valid&&x.rebased&&!x.advanced&&near(x.state.phase,old.phase)&&near(x.state.raw_phase,r.sample.raw_phase),"resume rebases cadence after missing time while retaining same bike display");
  const auto y=r.tick();check(y.advanced&&std::abs(c::wrap(y.state.phase-old.phase))<=c::leveling_rate*r.sample.seconds+1e-12,"first real resumed successor can level only one step");
 }
 for(int field=0;field<7;++field){Ride r;r.tick();r.tick();const auto before=r.state;if(field<6)++r.sample.identity.owner_world[static_cast<unsigned>(field)];else ++r.sample.identity.body_token;
  r.sample.raw_phase=.7;const auto x=r.tick();check(x.rebased&&!x.advanced&&near(x.state.phase,.7)&&x.state.identity!=before.identity,"every current owner/world/body-token replacement reseeds new source phase");}
 for(int mode=0;mode<3;++mode){Ride r;r.tick();r.tick();const auto before=r.state;
  if(mode==0)r.sample.serial+=5;else if(mode==1){r.sample.serial=1;r.sample.time=0;}else r.sample.time+=.2;
  r.sample.raw_phase+=1;const auto x=r.tick();check(x.valid&&x.rebased&&!x.advanced&&x.state.phase==before.phase,"clock gap/backward/time mismatch never catches up stale phase");}
 for(int mode=0;mode<7;++mode){Ride r;r.tick();const auto before=r.state;auto sample=r.sample;
  if(mode==0)sample.raw_phase=std::numeric_limits<double>::quiet_NaN();
  if(mode==1)sample.seconds=std::numeric_limits<double>::infinity();
  if(mode==2)sample.time=std::numeric_limits<double>::quiet_NaN();
  if(mode==3)sample.seconds=-.01;
  if(mode==4)sample.seconds=.11;
  if(mode==5)sample.identity.body_token=0;
  if(mode==6)sample.identity.owner_world[4]=0;
  const auto x=c::step(before,sample);check(!x.valid&&x.state.phase==before.phase&&x.state.serial==before.serial,"invalid scalar or incomplete identity cannot mutate phase/history");}
 {Ride r;r.tick();r.tick();const double phase=r.state.phase;r.sample.enabled=false;r.sample.raw_phase+=.4;const auto x=r.tick();check(x.valid&&near(x.state.phase,phase)&&!x.leveling,"suppressed controls hold presentation without deferred catch-up");r.sample.enabled=true;r.sample.pedaling=true;r.sample.raw_phase+=.03;const auto y=r.tick();check(near(y.state.phase,phase+.03),"reactivation follows only fresh raw delta");}
 // Different call subdivisions with equal accepted time reach identical phase.
 {Ride a,b;a.tick();b.tick();b.sample.seconds=1.0/30;b.sample.time=b.state.time+b.sample.seconds;for(int i=0;i<12;++i)a.tick();for(int i=0;i<6;++i)b.tick();check(near(a.state.phase,b.state.phase),"leveling speed depends on native elapsed time rather than callback count");}
 std::cout<<"PASS "<<checks<<" crank presentation checks\n";
}
