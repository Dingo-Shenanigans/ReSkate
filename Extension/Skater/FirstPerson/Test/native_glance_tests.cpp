#include "Extension/Skater/FirstPerson/glance_runtime.h"
#include <iostream>
#include <stdexcept>
using namespace dingosdk::first_person_integration;
namespace fpmod=jishsfirstpersonmod;
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){try{
    fpmod::Config config;config.toggle_glances=config.remember_marker_glance=true;config.look[0]={1,45};
    ControlsContext context;context.verified=context.telemetry_valid=context.eligible=true;context.on_foot=true;context.world_context=100;
    first_person_vendor::ControllerInput input{true,0,1,first_person_vendor::ControllerStyle::xbox};
    GlanceRuntime runtime;
    const auto tick=[&](unsigned buttons,bool blocked=false,bool requested=true){input.buttons=buttons;context.tick+=16;return runtime.step(config,config.look,input,blocked,requested,context);};
    const auto select=[&]{tick(0);check(tick(1)[0]==45,"Could not select toggled glance");tick(0);};
    const auto teleport=[&]{context.teleport=true;context.verified=false;check(tick(0,true)[0]==0,"Teleport did not clear displayed glance");runtime.disarm_pose();};
    const auto land=[&]{context.teleport=false;context.verified=true;return tick(0);};
    select();teleport();check(land()[0]==45,"Native teleport/pose disarm lost remembered glance");
    tick(0);tick(1);tick(0);select();teleport();context.world_context=200;check(land()[0]==0,"Glance survived world change");
    select();teleport();input.device=2;check(land()[0]==0,"Glance survived device change");
    select();teleport();context.tick+=6000;tick(0,true);tick(0,true);check(land()[0]==0,"Expired glance restored");
    select();input.device=3;teleport();check(land()[0]==0,"Device change at teleport entry restored another device glance");
    select();teleport();context.telemetry_valid=false;tick(0,true);context.telemetry_valid=true;check(land()[0]==0,"Menu/unknown UI did not clear memory");
    select();teleport();tick(0,true,false);check(land()[0]==0,"Disabling first person did not clear memory");
    select();teleport();context.teleport=false;context.verified=true;context.bail=true;tick(0,true);context.bail=false;check(tick(0)[0]==0,"Bail did not clear memory");
    select();teleport();config.look[0].angle=50;check(land()[0]==0,"Changed controls restored stale glance");
    std::cout<<"Native marker-glance lifecycle checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
