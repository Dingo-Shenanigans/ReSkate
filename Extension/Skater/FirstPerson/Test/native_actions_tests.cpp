#include "Extension/Skater/FirstPerson/native_action_bindings.h"
#include "Extension/Skater/FirstPerson/glance_runtime.h"
#include <iostream>
#include <stdexcept>
using namespace dingosdk;using namespace dingosdk::first_person_integration;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    check(action_binds[0].key=="first_person" && action_binds[1].key=="first_person_glance_up" && action_binds[5].key=="first_person_modifier","First Person bind ordering changed");
    for(unsigned i=1;i<=5;++i) check(action_binds[i].held && action_binds[i].command.empty(),"Held action entered console toggle path");
    jishsfirstpersonmod::Config config;config.look[0].angle=40;
    ControllerBindingsModel bindings;bindings.available=true;bindings.action_combos[1]=0x101;bindings.action_combos[5]=0x100;
    ControllerInput input;input.available=true;input.device=1;input.buttons=0x100;
    auto native=glance_input(config,bindings,input,true);
    check(native.input.buttons==0,"Partial controller chord triggered glance");
    input.buttons=0x101;native=glance_input(config,bindings,input,true);
    check(native.input.buttons==1 && native.controls[0].combo==1,"Native chord was not routed to glance");
    const auto key=keyboard_binding_tag|'K';bindings.action_combos[1]=key;input.available=false;input.device=0;input.buttons=0;
    input.keys['K'/64]|=std::uint64_t{1}<<('K'%64);
    native=glance_input(config,bindings,input,true);check(native.input.available && native.input.device && native.input.buttons==1,"Keyboard-only glance failed");
    config.glances_enabled=false;native=glance_input(config,bindings,input,true);check(native.controls[0].combo==0 && native.input.buttons==0,"Glance master did not disable actions");
    config.glances_enabled=true;bindings.available=false;native=glance_input(config,bindings,input,true);check(native.controls[0].combo==0,"Unavailable profile reused binding");
    bindings.available=true;native=glance_input(config,bindings,input,false);check(!native.input.available,"Native menu/focus gate bypassed");
    check(!action_held(0,input) && !action_held(0xffffffff,input),"Invalid action held");
    std::cout<<"Native held action routing checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
