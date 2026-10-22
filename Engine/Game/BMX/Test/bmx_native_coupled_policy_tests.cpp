#include "Engine/Game/BMX/bmx_native_coupled_policy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
namespace p=dingosdk::bmx::native_coupled::policy;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok){std::cerr<<"FAIL "<<why<<'\n';std::exit(1);}}
bool near(double a,double b,double tolerance=3e-6){return std::abs(a-b)<=tolerance;}
bool near(p::Vec a,p::Vec b,double tolerance=3e-6){for(unsigned i=0;i<3;++i)if(!near(a[i],b[i],tolerance))return false;return true;}
p::Epoch epoch(std::uint64_t serial=100){
    p::Epoch e;e.owner_world={1,2,3,4,5,6,7,8};e.bodies={p::BodyKey{6,10,3},p::BodyKey{6,11,4}};
    e.token=2;e.serial=serial;e.time=double(serial)/60;e.seconds=static_cast<float>(p::native_seconds);return e;
}
void transforms(){
    p::Quat principal,model;check(p::unit(p::v2_roles[0].principal_to_model,principal),"source chassis principal quaternion is unit");
    check(p::unit({.2f,-.3f,.1f,.92736185f},model),"test model quaternion unit");
    const auto native=p::native_orientation(model,principal);
    const auto decoded=p::model_orientation(native,principal);
    for(unsigned i=0;i<4;++i)check(near(decoded[i],model[i]),"native principal pose decodes exactly to model");
    const p::Vec point{.14f,.72f,-.11f};
    check(near(p::rotate(native,p::native_local_point(point,principal)),p::rotate(model,point)),"capsule geometry invariant under principal basis bridge");
    const auto I=p::v2_roles[0].principal_moments;const p::Vec torque{117,-93,201};
    auto principal_torque=p::rotate(p::conjugate(native),torque);
    for(unsigned i=0;i<3;++i)principal_torque[i]/=I[i];
    const auto native_acceleration=p::rotate(native,principal_torque);
    auto model_torque=p::rotate(p::conjugate(model),torque);
    model_torque=p::rotate(p::conjugate(principal),model_torque);
    for(unsigned i=0;i<3;++i)model_torque[i]/=I[i];
    const auto source_acceleration=p::rotate(model,p::rotate(principal,model_torque));
    check(near(native_acceleration,source_acceleration,2e-5),"off-axis source inertia tensor produces native-equivalent world acceleration");
    check(p::v2_roles[0].mass==30&&p::v2_roles[1].mass==80,"actual two roles use source masses");
    for(const auto& role:p::v2_roles)for(float moment:role.principal_moments){
        const float inverse=1.f/role.mass,ratio=role.mass/moment;
        check(near(1.f/(inverse*ratio),moment,2e-6),"native inverse mass and m/I readback recovers principal inertia");
    }
    check(!p::unit({0,0,0,0},model)&&!p::unit({0,0,0,2},model),"bad principal frames refused without pose rebasing");
    check(p::pose_matches({12,34,56},native,{12,34,56},native),"actual pose agrees with the reviewed proxy and overlap plan");
    auto opposite=native;for(auto& x:opposite)x=-x;
    check(p::pose_matches({12,34,56},opposite,{12,34,56},native),"quaternion sign alone does not change the actual orientation");
    check(!p::pose_matches({12.0003f,34,56},native,{12,34,56},native),"native setter returning without the requested COM cannot authorize collision admission");
    check(!p::pose_matches({12,34,56},{0,0,0,1},{12,34,56},native),"wrong actual principal orientation cannot authorize collision admission");
    check(!p::pose_matches({12,34,56},{0,0,0,2},{12,34,56},native),"nonunit actual orientation cannot authorize collision admission");
}
void epochs(){
    const auto good=epoch();check(p::valid_epoch(good),"distinct actual bodies in common native epoch");
    for(unsigned i=0;i<8;++i){auto bad=good;bad.owner_world[i]=0;check(!p::valid_epoch(bad),"complete owner/world identity required");}
    auto bad=good;bad.bodies[1].world=9;check(!p::valid_epoch(bad),"mixed native worlds refused");
    bad=good;bad.bodies[1].index=bad.bodies[0].index;check(!p::valid_epoch(bad),"same body cannot stand in for actual Spring");
    bad=good;bad.seconds=.01f;check(!p::valid_epoch(bad),"source100Hz is never fabricated from native60Hz");
    bad=good;bad.time=std::numeric_limits<double>::quiet_NaN();check(!p::valid_epoch(bad),"nonfinite clock refused");
    bad=good;bad.serial=0;check(!p::valid_epoch(bad),"missing native serial refused");
}
void caps(){
    p::Command c;c.roles[0].force={3,4,0};c.roles[0].hop_impulse={0,1,0};c.hop_part=4;
    auto prepared=p::prepare(c,p::native_seconds);check(prepared.valid&&near(prepared.force[0][1],64),"hop converted J/h once at actual native h");
    c.roles[1].angular_impulse={0,.5f,0};prepared=p::prepare(c,p::native_seconds);
    check(prepared.valid&&near(prepared.torque[1][1],30),"optional angular impulse converted L/h once");
    c.roles[0].force={40001,0,0};c.roles[0].hop_impulse={-1,0,0};check(!p::prepare(c,p::native_seconds).valid,"opposing hop cannot hide oversized raw force");
    c={};c.roles[0].force={30000,30000,0};check(!p::prepare(c,p::native_seconds).valid,"raw force norm bounded not merely components");
    c={};c.roles[0].torque={4001,0,0};c.roles[0].angular_impulse={-.5f,0,0};check(!p::prepare(c,p::native_seconds).valid,"opposing angular impulse cannot hide oversized raw torque");
    c={};c.roles[0].force={39990,0,0};c.roles[0].hop_impulse={1,0,0};c.hop_part=1;check(!p::prepare(c,p::native_seconds).valid,"combined force bounded");
    c={};c.roles[1].torque={3990,0,0};c.roles[1].angular_impulse={1,0,0};check(!p::prepare(c,p::native_seconds).valid,"combined torque bounded");
    c={};c.roles[0].hop_impulse={0,601,0};c.hop_part=1;check(!p::prepare(c,p::native_seconds).valid,"per-role linear impulse bound");
    c={};c.hop_part=1;check(!p::prepare(c,p::native_seconds).valid,"empty hop token refused");
    c={};c.roles[0].hop_impulse={0,1,0};check(!p::prepare(c,p::native_seconds).valid,"nonzero hop requires unique part token");
    c={};c.roles[1].torque[2]=std::numeric_limits<float>::infinity();check(!p::prepare(c,p::native_seconds).valid,"nonfinite wrench refused");
    check(!p::prepare({},.01).valid,"native60Hz adapter does not silently select source100Hz");
}
void once(){
    p::Once submit;submit.reset(2);p::Command c;std::vector<unsigned> calls;
    auto apply=[&](unsigned role,bool torque,const p::Vec&){calls.push_back(role*2+unsigned(torque));return true;};
    auto receipt=submit.run(epoch(),c,[]{return true;},apply);
    check(receipt.state==p::ReceiptState::submitted&&receipt.attempted==15&&receipt.completed==15&&calls==std::vector<unsigned>{0,1,2,3},"both genuine bodies get ordered force/torque once");
    receipt=submit.run(epoch(),c,[]{return true;},apply);check(receipt.state==p::ReceiptState::idle&&calls.size()==4,"duplicate native epoch never replayed");
    receipt=submit.run(epoch(99),c,[]{return true;},apply);check(receipt.state==p::ReceiptState::idle&&calls.size()==4,"rewound epoch preserves consumed high-water");
    receipt=submit.run(epoch(101),c,[]{return true;},apply);check(receipt.state==p::ReceiptState::submitted&&calls.size()==8,"next advancing native epoch accepted");
    auto replacement=epoch(102);replacement.bodies[1].generation++;
    receipt=submit.run(replacement,c,[]{return true;},apply);check(receipt.state==p::ReceiptState::terminal&&calls.size()==8,"body generation change requires full controller/backend lease reset");
    receipt=submit.run(epoch(102),c,[]{return true;},apply);check(receipt.state==p::ReceiptState::idle&&calls.size()==8,"identity failure terminal until explicit reset");
    submit.reset(3);replacement.token=3;receipt=submit.run(replacement,c,[]{return true;},apply);check(receipt.state==p::ReceiptState::submitted,"fresh token resets exact pair identity");
    submit.reset(2);auto wrong=epoch();wrong.token=3;receipt=submit.run(wrong,c,[]{return true;},apply);check(receipt.state==p::ReceiptState::idle,"wrong lease token cannot submit");
    c.roles[0].hop_impulse={0,5,0};c.roles[1].hop_impulse={0,10,0};c.hop_part=7;
    check(submit.run(epoch(),c,[]{return true;},apply).state==p::ReceiptState::submitted,"paired hop part accepted once for both bodies");
    check(submit.run(epoch(101),c,[]{return true;},apply).state==p::ReceiptState::idle,"same hop part not replayed on advancing epoch");
    c.hop_part=8;check(submit.run(epoch(101),c,[]{return true;},apply).state==p::ReceiptState::submitted,"new hop part on previously unconsumed epoch accepted");
    submit.reset(2);c={};check(submit.run(epoch(),c,[]{return true;},apply).state==p::ReceiptState::submitted,"clock test starts");
    wrong=epoch(101);wrong.time=epoch().time;check(submit.run(wrong,c,[]{return true;},apply).state==p::ReceiptState::terminal,"inconsistent advancing serial/nonadvancing native time fails terminally");
}
void faults(){
    for(unsigned failure=0;failure<4;++failure){
        p::Once submit;submit.reset(2);unsigned calls=0;
        auto receipt=submit.run(epoch(),{},[]{return true;},[&](unsigned,bool,const p::Vec&){return calls++!=failure;});
        check(receipt.state==p::ReceiptState::terminal&&receipt.attempted==((1u<<(failure+1))-1)&&receipt.completed==((1u<<failure)-1),"partial native writes expose exact attempted/completed mask");
        check(calls==failure+1&&submit.terminal(),"native failure stops immediately and permanently");
        (void)submit.run(epoch(101),{},[]{return true;},[&](unsigned,bool,const p::Vec&){++calls;return true;});
        check(calls==failure+1,"partial submission never retries even on newer epoch");
    }
    for(unsigned failure=0;failure<5;++failure){
        p::Once submit;submit.reset(2);unsigned proofs=0,calls=0;
        const auto receipt=submit.run(epoch(),{},[&]{return proofs++!=failure;},[&](unsigned,bool,const p::Vec&){++calls;return true;});
        check(receipt.state==p::ReceiptState::terminal&&calls==failure&&receipt.completed==((1u<<failure)-1),"proof fences before all4writes and afterfinalwrite");
    }
}
void inertia(){
    p::Command command;command.roles[0].target_principal_inertia=p::v2_roles[0].principal_moments;
    command.roles[1].target_principal_inertia={150,150,150};
    check(p::prepare(command,p::native_seconds).valid,"source grounded/chassis and airSpring diagonal targets admitted");
    p::Once transaction;transaction.reset(2);std::vector<unsigned> order;
    auto receipt=transaction.run(epoch(),command,[]{return true;},[&](unsigned role,const p::Vec& requested,p::Vec& actual){order.push_back(10+role);actual=requested;return true;},
        [&](unsigned role,bool torque,const p::Vec&){order.push_back(role*2+unsigned(torque));return true;});
    check(order==std::vector<unsigned>{10,11,0,1,2,3},"both inertia writes and readbacks precede every wrench");
    check(receipt.state==p::ReceiptState::submitted&&receipt.epoch==epoch()&&receipt.inertia_before_wrenches&&receipt.inertia_required==3&&receipt.inertia_attempted==3&&receipt.inertia_completed==3&&receipt.attempted==15&&receipt.completed==15,"ack binds complete exact owner/world/body/time epoch and all two-plus-four stages");
    check(receipt.verified_principal_inertia[1]==p::Vec{150,150,150},"ack carries actual verified applied inertia");
    for(unsigned failed_role=0;failed_role<2;++failed_role){
        transaction.reset(2);unsigned setters{},wrenches{};
        receipt=transaction.run(epoch(),command,[]{return true;},[&](unsigned role,const p::Vec& target,p::Vec& actual){++setters;actual=target;return role!=failed_role;},
            [&](unsigned,bool,const p::Vec&){++wrenches;return true;});
        check(receipt.state==p::ReceiptState::terminal&&wrenches==0&&receipt.attempted==0&&receipt.completed==0&&
            receipt.inertia_attempted==((1u<<(failed_role+1))-1)&&receipt.inertia_completed==((1u<<failed_role)-1),"partial inertia failure exposes exact stages and prevents all force writes");
        (void)transaction.run(epoch(101),command,[]{return true;},[&](unsigned,const p::Vec& target,p::Vec& actual){++setters;actual=target;return true;},
            [&](unsigned,bool,const p::Vec&){++wrenches;return true;});
        check(setters==failed_role+1&&wrenches==0,"consumed inertia epoch never retries after partial failure");
    }
    transaction.reset(2);unsigned wrenches{};
    receipt=transaction.run(epoch(),command,[]{return true;},[](unsigned,const p::Vec& target,p::Vec& actual){actual=target;actual[0]+=.01f;return true;},
        [&](unsigned,bool,const p::Vec&){++wrenches;return true;});
    check(receipt.state==p::ReceiptState::terminal&&wrenches==0&&!receipt.inertia_before_wrenches,"setter return without matching actual inertia cannot authorize a wrench");
    auto invalid=command;invalid.roles[1].target_principal_inertia[0]=151;check(!p::prepare(invalid,p::native_seconds).valid,"Spring target cannot exceed recovered150moment profile");
    invalid=command;invalid.roles[1].target_principal_inertia[0]=5;check(!p::prepare(invalid,p::native_seconds).valid,"Spring target cannot undershoot recovered source base");
    invalid=command;invalid.roles[0].target_principal_inertia[0]+=.1f;check(!p::prepare(invalid,p::native_seconds).valid,"unproved dynamic chassis tensor changes refused");
    invalid=command;invalid.roles[1].target_principal_inertia[0]=std::numeric_limits<float>::quiet_NaN();check(!p::prepare(invalid,p::native_seconds).valid,"nonfinite target inertia refused");
    invalid=command;invalid.roles[1].torque={4001,0,0};check(!p::prepare(invalid,p::native_seconds).valid,"inertia feature never raises existing4k native torque cap");
}
int main(){transforms();epochs();caps();once();faults();inertia();std::cout<<"PASS "<<checks<<" native coupled policy checks\n";}
