#include "Extension/Skater/FirstPerson/Upstream/src/comfort_policy.h"
#include "Extension/Skater/FirstPerson/spectator_state.h"
#include "Extension/Skater/FirstPerson/native_stick_filter.h"
#include <stdexcept>
#include <iostream>
using namespace dingosdk::first_person_integration;
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){
    jishsfirstpersonmod::BailLatch bail;
    check(bail.update({1,true,false,false,true,true,false})==true,"wipeout not latched");
    check(bail.update({1,true,false,false,true,false,true})==true,"recovery lost bail ownership");
    check(bail.update({1,true,false,true,true,false,false})==true,"one normal sample released bail");
    check(bail.update({1,true,false,true,true,false,false})==false,"native control did not release bail");
    jishsfirstpersonmod::RecoveryCameraLatch recovery;
    check(!recovery.update(true,1,true,true,false,false,false,false),"fall switched before get-up");
    check(recovery.update(true,1,true,true,true,false,false,false),"get-up did not switch camera");
    check(recovery.update(true,1,true,true,false,false,false,false),"get-up flag clearing ended return prematurely");
    check(!recovery.update(true,1,true,true,false,true,false,false),"native control failed to restore first person");
    SpectatorState view;
    check(view.observe(true,10,1,100) && !view.head,"new spectate did not start in follow view");
    check(view.toggle() && view.head,"first-person action did not select head");
    check(!view.observe(true,10,1,100) && view.head,"same session lost head selection");
    check(view.observe(true,10,2,100) && !view.head,"rejoin retained head view");
    view.toggle();check(view.observe(false,10,2,100) && !view.id && !view.toggle(),"disable retained spectator ownership");
    NativeStickHold latch;
    StickCapture policy;policy.watch_modifier=0x100;policy.keep_native_input=true;policy.xinput_sources=1;policy.published=1000;
    dingosdk::overlay::GamepadValues physical;physical.buttons=0x100;physical.right_x=21000;physical.right_y=-17000;
    // First game poll observes the modifier edge before the camera tick.
    latch.sample(policy,physical,1000);
    // Camera tick sees the physical stick centered. Its late latch must not replace the edge latch.
    policy.modifier=0x100;policy.x=policy.y=0;policy.published=1010;
    physical.right_x=physical.right_y=0;
    auto held=latch.sample(policy,physical,1010);
    check(held.apply(0,physical,1010) && physical.right_x==21000 && physical.right_y==-17000,"late camera sample lost held skating input");
    physical.buttons=0;physical.right_x=-9000;physical.right_y=8000;
    held=latch.sample(policy,physical,1020);
    check(!held.apply(0,physical,1020) && physical.right_x==-9000 && physical.right_y==8000,"modifier release did not restore physical stick immediately");
    policy.keep_native_input=false;policy.x=policy.y=0;physical.buttons=0x100;
    held=latch.sample(policy,physical,1030);held.apply(0,physical,1030);
    check(!physical.right_x && !physical.right_y,"disabled hold did not neutralize stick");
    std::cout<<"Native comfort, spectator lifecycle and input-edge latch checks passed\n";
}
