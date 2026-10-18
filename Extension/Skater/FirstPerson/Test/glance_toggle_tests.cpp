#include "Extension/Skater/FirstPerson/Upstream/src/glance_controls.h"
#include "Extension/Skater/FirstPerson/Upstream/src/settings.h"
#include "Extension/Skater/FirstPerson/Upstream/src/binding_conflicts.h"
#include "Extension/Skater/FirstPerson/Upstream/src/look_smoothing.h"
#include "Extension/Skater/FirstPerson/Upstream/src/marker_glance.h"
#include <iostream>
#include <stdexcept>
namespace jishsfirstpersonmod {
void log(const std::string&) noexcept {}
std::filesystem::path data_directory() { return std::filesystem::current_path()/"glance-toggle-test-data"; }
std::filesystem::path module_directory() { return data_directory(); }
}
using namespace jishsfirstpersonmod;
void check(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
int main() {
    using namespace first_person_vendor;
    FirstPersonLookControls controls{{{0x101,30},{0x102,40},{0x104,60},{0x108,75}}};
    GlanceControlsState state;
    const auto input=[](std::uint32_t buttons,unsigned device=1,bool available=true) { return ControllerInput{available,buttons,device}; };
    const auto step=[&](std::uint32_t buttons,bool blocked=false,bool toggle=true) { return state.update(controls,input(buttons),blocked,toggle); };
    step(0);check(step(0x104)[1]==-60,"left press did not toggle");
    check(step(0)[1]==-60 && step(0x100)[1]==-60,"release/modifier cleared latched glance");
    check(step(0x104)[1]==0,"modifier-held repeated direction did not clear glance");
    step(0x100);check(step(0x104)[1]==-60 && step(0x104)[1]==-60,"held chord repeated");
    check(step(0x10c)[1]==75,"new direction did not replace held old direction");
    check(step(0)[1]==75,"new direction was not held after release");
    auto ambiguous=step(0x103);check(ambiguous[0]==0 && ambiguous[1]==75,"simultaneous new directions changed selection");
    step(0);auto up=step(0x101);check(up[0]==30 && up[1]==0,"up glance did not replace horizontal glance");
    step(0);auto down=step(0x102);check(down[0]==-40 && down[1]==0,"down glance did not replace up");
    check(step(0x102)[0]==-40,"held down toggled repeatedly");
    check(step(0x102,true)==std::array<float,2>{},"blocked input retained glance");
    check(step(0x102)==std::array<float,2>{},"resume accepted held chord without neutral");
    step(0);check(step(0x101)[0]==30,"neutral did not rearm after interruption");
    check(state.update(controls,input(0,1,false),false,true)==std::array<float,2>{},"disconnect retained glance");
    check(step(0x101)==std::array<float,2>{},"reconnection resumed held glance");
    step(0);step(0x104);
    check(state.update(controls,input(0x104,2),false,true)==std::array<float,2>{},"device change retained glance");
    state.reset();step(0);step(0x104);controls[2].angle=65;
    check(step(0x104)==std::array<float,2>{},"angle edit retained selected glance");
    step(0);check(step(0x104)[1]==-65,"edited angle was not used");
    controls[2].combo=0x110;check(step(0x110)==std::array<float,2>{},"binding edit activated held input");
    step(0);check(step(0x110)[1]==-65,"edited binding failed");
    check(step(0x110,false,false)==std::array<float,2>{},"changing to hold mode retained toggle selection");
    step(0,false,false);check(step(0x110,false,false)[1]==-65,"hold mode changed");
    check(step(0,false,false)==std::array<float,2>{},"hold mode failed to release");
    state.reset();controls[2]={0x104,60};step(0,false,false);
    check(step(0x10c,false,false)[1]==0,"hold mode opposite cancellation changed");
    state.reset();step(0);step(0x104);
    MarkerGlanceMemory marker;
    marker.observe(true,true,state.selection(),1,123,1);
    check(marker.take(false)==-1,"marker restored while controls unavailable");
    state.reset();step(0);
    check(state.restore_selection(marker.take(true)),"marker direction not restored");
    check(step(0x104)[1]==-60 && step(0x104)[1]==-60,"held return chord toggled restored direction");
    step(0);check(step(0x104)[1]==0,"restored glance could not be toggled off after release");
    marker.observe(false,true,2,1,123,2);check(marker.take(true)==-1,"disabled marker memory retained direction");
    marker.observe(true,false,2,1,123,2);check(marker.take(true)==-1,"ordinary interruption remembered direction");
    marker.observe(true,true,2,1,123,2);marker.observe(true,false,-1,1,123,8);
    check(marker.take(true)==-1,"expired marker memory restored");
    marker.observe(true,true,2,1,123,2);marker.observe(true,false,-1,2,123,3);
    check(marker.take(true)==-1,"marker memory crossed controller change");
    marker.observe(true,true,2,1,123,2);marker.observe(true,false,-1,1,456,3);
    check(marker.take(true)==-1,"marker memory crossed worlds");
    marker.observe(true,true,2,1,123,2);marker.observe(false,false,-1,1,123,3);
    check(marker.take(true)==-1,"menu/focus/feature cancellation retained marker memory");
    Config config;config.toggle=0;config.look=controls;config.toggle_glances=true;
    config.reserved[0]=controls[0].combo;
    auto route=route_controller_input(config,false);state.reset();state.update(route.glance,input(0),false,true);
    check(state.update(route.glance,input(0x101),false,true)==std::array<float,2>{},"conflicting glance activated");
    check(state.update(route.glance,input(0x104),false,true)[1]==-60,"conflict blocked another glance");
    state.reset();check(step(0x104)==std::array<float,2>{},"owner/world reset accepted held input");
    check(!Config{}.toggle_glances,"default changed to toggle mode");
    const auto path=data_directory()/"settings.json";
    SettingsStore store(path,Config{});Command command;command.kind=CommandKind::toggle_glances;command.enabled=true;
    check(store.submit(command,0),"checkbox command rejected");store.flush_due(0);
    check(read_config(path).toggle_glances,"toggle preference did not persist");
    command.enabled=false;check(store.submit(command,1),"checkbox off rejected");store.flush_due(1);
    check(!read_config(path).toggle_glances,"hold preference did not persist");
    command.kind=CommandKind::remember_marker_glance;command.enabled=true;
    check(store.submit(command,2),"marker checkbox command rejected");store.flush_due(2);
    check(read_config(path).remember_marker_glance,"marker preference did not persist");
    check(!Config{}.remember_marker_glance,"marker memory enabled by default");
    std::cout<<"Toggle/hold glances, replacement, press edges, interruptions, conflicts and persistence passed\n";
}
