#include "Extension/BMX/bmx_intent_snapshot.h"
#include "Extension/BMX/bmx_intent_pulse.h"
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
namespace d=dingosdk::bmx::intent_observer::detail;
namespace {
constexpr std::uintptr_t base=0x140000000,event_address=0x200000,node_address=0x210000,
    data_address=0x220000,root_address=0x230000,asset_address=0x240000,manager_address=0x250000,
    table_address=0x260000,entity_address=0x270000,core_address=0x280000,context_address=0x290000,
    rig_address=0x2a0000,client_address=0x2b0000,input_address=0x2c0000,mirror_address=0x2d0000;
constexpr std::uint64_t handle_value=0x4000000000000003ULL;
volatile bool throw_requested=true;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Memory {
    std::map<std::uintptr_t,std::vector<unsigned char>> blocks;
    unsigned reads{},node_reads{},slot_reads{},event_reads{};
    bool race_node{},unreadable{},node_prefix_only{},node_reread_unreadable{},race_event{};
    void block(std::uintptr_t p,std::size_t n){blocks.emplace(p,std::vector<unsigned char>(n));}
    template<class T>void put(std::uintptr_t p,const T& value){
        auto it=blocks.upper_bound(p);require(it!=blocks.begin(),"fixture block exists");--it;
        require(p-it->first<=it->second.size()&&sizeof(T)<=it->second.size()-(p-it->first),"fixture store bounded");
        std::memcpy(it->second.data()+p-it->first,&value,sizeof(T));
    }
    bool read(std::uintptr_t p,void* out,std::size_t n)noexcept{
        ++reads;if(unreadable)return false;
        if(p==node_address&&n==120){
            ++node_reads;
            if(node_prefix_only||(node_reads==2&&node_reread_unreadable))return false;
            if(node_reads==2&&race_node){auto& block=blocks.find(node_address)->second;block[0x70]^=1;}
        }
        if(p==event_address&&n==8&&++event_reads==2&&race_event){
            auto& block=blocks.find(event_address)->second;block[0]^=8;
        }
        if(p==table_address+3*0x140)++slot_reads;
        auto it=blocks.upper_bound(p);if(it==blocks.begin())return false;--it;
        if(p-it->first>it->second.size()||n>it->second.size()-(p-it->first))return false;
        std::memcpy(out,it->second.data()+p-it->first,n);return true;
    }
    Memory(){
        block(event_address,8);put(event_address,node_address);
        block(node_address,120);std::array<std::uint64_t,15> node{};
        node[0]=base+d::node_vtable_rva;node[1]=data_address;node[5]=root_address;node[9]=handle_value;
        node[10]=0x300000;node[11]=0x310000;node[12]=node[13]=asset_address+0x60;put(node_address,node);
        block(data_address-16,0x80);put(data_address-16,d::authored_guid);put(data_address,base+d::data_vtable_rva);
        put(data_address+0x58,std::array<std::uint8_t,2>{1,0});
        block(root_address,0x28);put(root_address+0x20,handle_value);
        block(asset_address-16,0xa0);put(asset_address-16,d::wipeout_guid);put(asset_address,base+d::asset_vtable_rva);
        put(asset_address+0x20,base+d::query_vtable_rva);put(asset_address+0x60,base+d::bool_vtable_rva);
        block(base+d::entity_manager_rva,8);put(base+d::entity_manager_rva,manager_address);
        block(manager_address,0x28);put(manager_address+0x10,std::uint32_t{7});put(manager_address+0x20,table_address);
        block(table_address+3*0x140,16);put(table_address+3*0x140,std::array<std::uint64_t,2>{handle_value,entity_address});
        block(input_address,8);put(input_address,std::uintptr_t{0x300000});
        block(mirror_address,1);put(mirror_address,std::uint8_t{1});
    }
};
auto reader(Memory& m){return [&m](std::uintptr_t p,void* out,std::size_t n)noexcept{return m.read(p,out,n);};}
d::Snapshot sample(Memory& m,d::Kind kind=d::Kind::begin){return d::inspect(base,kind,event_address,input_address,mirror_address,reader(m));}
d::Owner owner(){return {client_address,entity_address,core_address,context_address,rig_address,true};}
struct PulseFixture {
    d::Pulse pulse;
    std::uint64_t started=1000;
    std::uint8_t value{};
    std::uintptr_t property=asset_address+0x60;
    bool resolve_ok=true,qualify_ok=true,write_returns=true,reenter=false,attempt_rearm=false,rearm_result=true;
    unsigned resolutions{},qualified{},queries{};
    std::vector<std::uint8_t> writes;
    std::vector<std::uintptr_t> properties;
    PulseFixture(){require(pulse.rearm(false),"fresh pulse generation arms once");}
    void run(std::uint64_t serial=10,std::uint64_t now=1000,bool eligible=true,bool cancelled=false,bool changed=false,
        std::uintptr_t binding=0x310000,std::uint64_t handle=handle_value) noexcept {
        pulse.dispatch({binding,handle,serial},started,now,eligible,cancelled,changed,
            [&](std::uintptr_t& out) noexcept {++resolutions;out=property;return resolve_ok;},
            [&](bool) noexcept {++qualified;return qualify_ok;},
            [&](std::uintptr_t p,std::uintptr_t,std::uint8_t& out) noexcept {++queries;properties.push_back(p);out=value;return true;},
            [&](std::uintptr_t p,std::uintptr_t,std::uint8_t v) noexcept {
                properties.push_back(p);writes.push_back(v);value=v;
                if(attempt_rearm)rearm_result=pulse.rearm(true);
                if(reenter)run(serial+1,now+1);
                return write_returns;
            });
    }
};
void pulse_tests(){
    PulseFixture a;a.run();
    require(!a.pulse.rearm(true)&&a.pulse.generation.load()==1,"reset pending cannot be discarded to rearm another crash");
    require(a.writes==std::vector<std::uint8_t>{1}&&a.pulse.pending()&&a.pulse.written.load()&&a.pulse.true_serial.load()==10,
        "one true entry publishes its reset obligation and verifies typed readback");
    a.run();a.run(9,1010);a.run(11,1010,true,false,false,0x320000);a.run(11,1010,true,false,false,0x310000,handle_value+1);
    require(a.writes.size()==1&&a.pulse.reset_calls.load()==0,"same/older serial and different binding/full generation cannot consume reset");
    a.property+=0x1000;a.run(11,1800,false,true);
    require(a.writes==std::vector<std::uint8_t>({1,0})&&!a.pulse.pending()&&a.pulse.reset_false.load()&&
        a.pulse.phase.load()==d::PulsePhase::reset&&a.pulse.reset_serial.load()==11&&a.properties.back()==a.property,
        "cancelled/expired arm still resets on fresh next clock with newly resolved property, never a saved pointer");
    a.run(12,1900);require(a.writes.size()==2,"verified pulse never starts again");
    PulseFixture existing;existing.value=1;existing.run();existing.run(11,1017);
    require(existing.writes.empty()&&existing.pulse.phase.load()==d::PulsePhase::preexisting_true,
        "preexisting native true is neither overwritten nor reset by SDK");
    PulseFixture late;late.run(10,1250);late.run(11,999);late.run(12,1100,false);late.run(13,1100,true,true);
    require(late.resolutions==0&&late.writes.empty(),"arm boundary, reverse clock, eligibility and cancellation reject before native queries");
    PulseFixture native_clear;native_clear.run();native_clear.value=0;native_clear.run(11,1017);
    require(native_clear.writes.size()==1&&native_clear.pulse.native_cleared.load()&&!native_clear.pulse.pending()&&
        native_clear.pulse.reset_false.load(),"fresh later getter verifies a native clear without an unnecessary false setter");
    PulseFixture replaced;replaced.run();replaced.run(11,1017,true,false,true);
    require(replaced.writes.size()==1&&replaced.pulse.phase.load()==d::PulsePhase::owner_lost&&!replaced.pulse.reset_false.load(),
        "owner replacement never invokes an old binding and cannot claim successful cleanup");
    PulseFixture transient;transient.resolve_ok=false;transient.run();transient.resolve_ok=true;transient.run();
    require(transient.resolutions==1&&transient.writes.empty(),"failed preflight is bounded once per native serial");
    transient.run(11,1017);transient.qualify_ok=false;transient.run(12,1034);transient.qualify_ok=true;transient.run(13,1050);
    require(transient.writes==std::vector<std::uint8_t>({1,0}),"unavailable fresh reset callback leaves obligation for later qualified edge");
    PulseFixture uncertain;uncertain.write_returns=false;uncertain.run();uncertain.run(10,1001);
    require(uncertain.pulse.pending()&&!uncertain.pulse.written.load()&&uncertain.writes.size()==1,
        "fault after possible true store cannot retry activation or discard reset");
    uncertain.write_returns=true;uncertain.run(11,1017);
    require(uncertain.pulse.phase.load()==d::PulsePhase::reset&&uncertain.writes.size()==2,"uncertain true entry is cleaned through current later callback");
    PulseFixture recursive;recursive.reenter=true;recursive.run();
    require(recursive.writes.size()==1&&recursive.pulse.true_calls.load()==1,"nested/concurrent reservation cannot make a second true entry");
    recursive.run(11,1017);require(recursive.writes.size()==2&&recursive.pulse.reset_calls.load()==1,
        "nested cleanup likewise preserves single-entry serialization");
    PulseFixture twice;twice.run();twice.run(11,1017);
    require(!twice.pulse.rearm(false)&&twice.pulse.generation.load()==1,
        "verified reset still waits for completed or cancelled prior lifecycle");
    require(twice.pulse.rearm(true)&&twice.pulse.generation.load()==2,"completed reset admits a new monotonic crash generation");
    twice.started=300000;twice.run(100,300000);twice.run(101,300017);
    require(twice.writes==std::vector<std::uint8_t>({1,0,1,0})&&twice.pulse.reset_serial.load()==101,
        "two successive crashes each enter once and reset on their own fresh native edge after passive capture expiry");
    PulseFixture in_flight;in_flight.attempt_rearm=true;in_flight.run();
    require(!in_flight.rearm_result&&in_flight.pulse.generation.load()==1&&in_flight.pulse.pending(),
        "native entry cannot recursively rearm or erase its pending reset");
    PulseFixture unresolved;unresolved.write_returns=false;unresolved.run();
    require(!unresolved.pulse.rearm(true),"uncertain true call cannot be discarded by cancellation/rearm");
    unresolved.run(11,1017,true,false,true);
    require(!unresolved.pulse.rearm(true)&&unresolved.writes.size()==1,
        "owner loss cannot turn an unverified cleanup into permission to write a replacement owner");
    PulseFixture cancelled;cancelled.run(10,1000,true,true);
    require(cancelled.writes.empty()&&cancelled.pulse.rearm(true)&&cancelled.pulse.generation.load()==2,
        "cancellation before entry admits a later generation without native writes");
    require(d::admit_callback(false,1000,300000,true,false)&&d::admit_callback(false,1000,300000,false,true)&&
        !d::admit_callback(true,1000,201000,false,false)&&!d::admit_callback(false,1000,1100,false,false),
        "ordinary bounded crash/reset callbacks remain admitted beyond passive expiry; idle ordinary play does no inspection");
    bool retained_pending=true;std::uint64_t retained_generation=7;
    const auto merge_unavailable=[&]() noexcept {retained_pending=false;retained_generation=0;};
    require(!d::apply_available(false,merge_unavailable)&&retained_pending&&retained_generation==7,
        "unavailable lifecycle snapshot under contention preserves prior reset duty and generation");
    require(d::apply_available(true,[&]() noexcept {retained_pending=false;retained_generation=8;})&&
        !retained_pending&&retained_generation==8,"a coherent later snapshot can replace cached status");
    require(d::blocks_rearm(true,true,true,1,false)&&d::blocks_rearm(true,true,false,1,false)&&
        d::blocks_rearm(true,false,false,1,true)&&!d::blocks_rearm(true,true,false,1,true)&&
        !d::blocks_rearm(true,true,false,0,false),
        "new crash request cannot erase cancelled pending or owner-expired uncertain cleanup; verified and pre-entry cancellation can rearm");
}
}
int main(){try{
    pulse_tests();
    Memory m;auto s=sample(m);
    require(s.target&&s.node_coherent&&s.data_coherent&&s.root_coherent&&s.authored_wipeout,"exact authored target and node identity");
    std::array<unsigned char,128> oversized{};
    require(!m.read(node_address,oversized.data(),oversized.size())&&m.blocks.at(node_address).size()==0x78,
        "exact120B allocation is readable while the following8bytes remain inaccessible");
    require(s.header.stage==d::Stage::qualified&&s.header.event_readable&&s.header.node_readable_120&&
        s.header.node_type_matches&&s.header.node_rereadable_120&&s.header.node_equal&&s.header.event_equal,
        "successful header stages retain all source qualifications");
    require(s.input.request_wipeout&&s.mirror.request_wipeout&&s.handle.coherent&&s.handle.entity==entity_address,"typed asset and full handle mapping");
    require(d::probe_phase(1000,999)==d::ProbePhase::closed&&d::probe_phase(1000,1000)==d::ProbePhase::arm&&
        d::probe_phase(1000,1249)==d::ProbePhase::arm&&d::probe_phase(1000,1250)==d::ProbePhase::follow&&
        d::probe_phase(1000,3249)==d::ProbePhase::follow&&d::probe_phase(1000,3250)==d::ProbePhase::closed,
        "impact arm expires250ms while read-only lifecycle follow-up remains bounded to2250ms");
    require(s.begin_context_readable&&s.begin_context_argument==s.context_wrapper,"Begin observed wrapper matches retained node context");
    auto c=d::correlate(base,s,1000,1016,owner(),reader(m));require(c.available&&c.entity_matches&&c.full_handle_still_maps&&c.age_ms==16,"fresh canonical full handle correlation");
    require(d::owner_bank(s,{entity_address,1000,true},1016),"fresh local target reserves its independent owner bank");
    require(!d::owner_bank(s,{entity_address+0x100,1000,true},1016)&&!d::owner_bank(s,{entity_address,1000,true},1251)&&
        !d::owner_bank(s,{entity_address,1000,false},1016),"nonlocal/stale/incoherent hint stays in pre-owner bank");
    auto wrong=owner();wrong.entity+=0x100; c=d::correlate(base,s,1000,1016,wrong,reader(m));require(c.available&&!c.entity_matches,"different canonical owner never matches");
    bool done=false;d::Correlation tracked;
    d::advance_correlation(tracked,done,base,s,1000,1016,wrong,reader(m));require(!done&&!tracked.entity_matches,"wrong initial canonical frame does not end retry window");
    d::advance_correlation(tracked,done,base,s,1000,1032,owner(),reader(m));require(done&&tracked.entity_matches,"later correct canonical frame can correlate within window");
    done=false;m.unreadable=true;
    d::advance_correlation(tracked,done,base,s,1000,1016,owner(),reader(m));require(!done&&!tracked.full_handle_still_maps,"transient native table read failure does not end retries");
    m.unreadable=false;d::advance_correlation(tracked,done,base,s,1000,1032,owner(),reader(m));require(done&&tracked.entity_matches,"stable later table read can correlate within window");
    wrong=owner();wrong.available=false;c=d::correlate(base,s,1000,1016,wrong,reader(m));require(!c.available,"unavailable canonical frame is not ownership evidence");
    c=d::correlate(base,s,1000,1251,owner(),reader(m));require(!c.available,"old pointer capture cannot establish a later owner");
    c=d::correlate(base,s,1000,999,owner(),reader(m));require(!c.available,"backward time rejects correlation");
    m.put(table_address+3*0x140,std::array<std::uint64_t,2>{handle_value+(std::uint64_t{1}<<32),entity_address});
    c=d::correlate(base,s,1000,1016,owner(),reader(m));require(c.available&&!c.full_handle_still_maps&&!c.entity_matches,"same pointer with different generation rejects");
    m=Memory{};m.put(root_address+0x20,handle_value+1);s=sample(m);require(s.target&&!s.root_coherent,"root/handle mismatch stays visible but untrusted");
    c=d::correlate(base,s,1000,1016,owner(),reader(m));require(!c.available,"mismatched root is not correlated");
    m=Memory{};m.put(node_address,base+d::node_vtable_rva+8);s=sample(m);
    require(!s.target&&!s.node_coherent&&s.header.stage==d::Stage::node_type_mismatch&&
        s.header.vtable==base+d::node_vtable_rva+8&&s.header.node_readable_120&&m.reads==2,
        "wrong node type records exact header and never follows data/root pointers");
    m=Memory{};m.put(node_address,base+0x6178d40);s=sample(m);
    require(!s.target&&s.header.stage==d::Stage::node_type_mismatch&&m.reads==2,
        "historical neighboring128B class is not accepted as the observed Bool node");
    m=Memory{};m.node_prefix_only=true;s=sample(m);
    require(!s.target&&s.header.stage==d::Stage::node_unreadable&&s.header.event_readable&&
        s.header.node_span_valid&&!s.header.node_readable_120&&s.header.vtable_readable&&
        s.header.vtable==base+d::node_vtable_rva&&m.reads==3,"partial node only reads one bounded prefix for diagnosis");
    m=Memory{};m.put(event_address,std::uintptr_t{3});s=sample(m);
    require(!s.target&&s.header.event_readable&&!s.header.node_span_valid&&!s.header.vtable_readable&&
        s.header.stage==d::Stage::node_unreadable&&m.reads==1,"invalid node span records pointer without dereferencing it");
    m=Memory{};m.put(data_address,base+d::data_vtable_rva+8);s=sample(m);require(!s.target&&!s.data_coherent&&
        s.header.data_vtable_readable&&s.header.data_vtable==base+d::data_vtable_rva+8&&s.header.stage==d::Stage::data_unqualified,
        "wrong authored data type rejects and retains only the already-read actual vtable");
    m=Memory{};auto guid=d::wipeout_guid;guid[0]^=1;m.put(asset_address-16,guid);s=sample(m);require(!s.target&&!s.input.request_wipeout,"different Bool asset does not qualify");
    require(d::control_matches(base,s,owner(),reader(m)),"separate generic control probe can match another genuinely typed Bool asset");
    require(!d::correlate(base,s,1000,1016,owner(),reader(m)).available,"generic control qualification never weakens RequestWipeout target correlation");
    wrong=owner();wrong.context=0;require(!d::control_matches(base,s,wrong,reader(m)),"generic control still requires complete canonical owner");
    wrong=owner();wrong.entity+=0x100;require(!d::control_matches(base,s,wrong,reader(m)),"generic control rejects another owner");
    auto incomplete=s;incomplete.binding=0;require(!d::control_matches(base,incomplete,owner(),reader(m)),"generic control requires live-shaped binding");
    incomplete=s;incomplete.root_coherent=false;require(!d::control_matches(base,incomplete,owner(),reader(m)),"generic control cannot bypass root handle proof");
    m.put(table_address+3*0x140,std::array<std::uint64_t,2>{handle_value+(std::uint64_t{1}<<32),entity_address});
    require(!d::control_matches(base,s,owner(),reader(m)),"generic control rereads the current full generation-tagged handle");
    m=Memory{};m.put(asset_address+0x60,base+d::bool_vtable_rva+8);s=sample(m);require(!s.target&&!s.input.typed,"matching GUID with wrong interface rejects");
    m=Memory{};guid=d::authored_guid;guid[0]^=1;m.put(data_address-16,guid);s=sample(m);require(s.target&&!s.authored_wipeout,"same exact Bool intent through another authored action is distinguished");
    m=Memory{};m.put(input_address,std::uint8_t{1});s=sample(m,d::Kind::update);require(s.target&&s.update_after_coherent&&s.update_after[0]==1&&s.update_after[1]==1,"Update records after-call bool values separately");
    m=Memory{};m.put(input_address,std::uint8_t{2});s=sample(m,d::Kind::update);require(s.target&&!s.update_after_coherent,"invalid observed bool is not normalized");
    m=Memory{};m.race_node=true;s=sample(m);require(!s.target&&!s.node_coherent&&
        s.header.stage==d::Stage::node_changed&&s.header.node_rereadable_120&&!s.header.node_equal,
        "native node mutation is distinguished from inaccessible memory");
    m=Memory{};m.node_reread_unreadable=true;s=sample(m);require(!s.target&&
        s.header.stage==d::Stage::node_reread_unreadable&&s.header.node_readable_120&&!s.header.node_rereadable_120,
        "second node access failure has a separate rejection stage");
    m=Memory{};m.race_event=true;s=sample(m);require(!s.target&&s.header.node_equal&&s.header.event_rereadable&&
        !s.header.event_equal&&s.header.stage==d::Stage::event_changed,"EventPassParams mutation has a separate rejection stage");
    m=Memory{};m.unreadable=true;s=sample(m);require(!s.target&&
        !s.header.event_readable&&s.header.stage==d::Stage::event_unreadable,"unreadable event rejects without native calls");
    m=Memory{};s=d::inspect(base,d::Kind::begin,d::highest-3,0,0,reader(m));require(!s.target&&m.reads==0&&
        s.header.stage==d::Stage::event_unreadable,"event storage overflow rejected before read");
    m=Memory{};auto h=d::inspect_handle(base,3,reader(m));require(!h.coherent&&m.reads==0,"untagged native handle never enters table");
    unsigned error=41,calls=0,observed=0;
    d::forward_observed([&](std::uintptr_t a,std::uintptr_t b,std::uintptr_t cc,std::uintptr_t dd){
        ++calls;require(a==1&&b==2&&cc==3&&dd==4&&error==41,"unchanged original arguments and incoming error");error=97;
    },[&](auto,auto,auto,auto)noexcept{++observed;error=999;},[&]()noexcept{return error;},[&](unsigned value)noexcept{error=value;},
        std::uintptr_t{1},std::uintptr_t{2},std::uintptr_t{3},std::uintptr_t{4});
    require(calls==1&&observed==1&&error==97,"original once, after-observer and outgoing error preserved");
    calls=observed=0;error=51;
    d::forward_observed([&](std::uintptr_t event){++calls;require(event==event_address&&error==51,"unchanged End ABI");error=71;},
        [&](std::uintptr_t event)noexcept{++observed;if(event!=event_address)std::terminate();error=999;},
        [&]()noexcept{return error;},[&](unsigned value)noexcept{error=value;},event_address);
    require(calls==1&&observed==1&&error==71,"End original once and outgoing error preserved");
    calls=observed=0;
    try{
        d::forward_observed([&](std::uintptr_t,std::uintptr_t,std::uintptr_t){++calls;if(throw_requested)throw 19;},
            [&](auto,auto,auto)noexcept{++observed;},[&]()noexcept{return error;},[&](unsigned value)noexcept{error=value;},
            std::uintptr_t{1},std::uintptr_t{2},std::uintptr_t{3});
        require(false,"native exception must propagate");
    }catch(int value){require(value==19&&calls==1&&!observed,"no observer or fabricated result after original exception");}
    std::cout<<"intent snapshot/correlation/forwarding tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
