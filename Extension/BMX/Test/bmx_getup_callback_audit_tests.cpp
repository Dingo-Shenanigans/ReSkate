#include "Extension/BMX/bmx_getup_callback_audit.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>

namespace audit=dingosdk::bmx::getup_callback_audit;
namespace intent=dingosdk::bmx::intent_observer::detail;
namespace {
void require(bool value,const char* issue){if(!value){std::cerr<<issue<<'\n';std::exit(1);}}
struct Memory {
    std::map<std::uintptr_t,unsigned char> bytes;
    std::uintptr_t changing{};unsigned changed_reads{};
    template<class T> void put(std::uintptr_t p,T value) {
        const auto* raw=reinterpret_cast<const unsigned char*>(&value);
        for(std::size_t i=0;i<sizeof(T);++i)bytes[p+i]=raw[i];
    }
    bool read(std::uintptr_t p,void* out,std::size_t size) {
        if(p==changing&&++changed_reads==2)bytes[p]^=1;
        auto* target=static_cast<unsigned char*>(out);
        for(std::size_t i=0;i<size;++i) {
            const auto found=bytes.find(p+i);if(found==bytes.end())return false;target[i]=found->second;
        }
        return true;
    }
};
struct Fixture {
    static constexpr std::uintptr_t base=0x140000000,context=0x51000,manager=0x52000,begin=0x53000,
        player=0x54000,handle=0x55000,collection=0x56000,component=0x57000,selector=0x58000,causes=0x59000;
    intent::Owner owner{0x100000,0x200000,0x300000,0x400000,0x500000,true};
    audit::Types types{0x100,0x200,0x300,0x400,0x500,0x600,0x700};Memory memory;
    Fixture() {
        memory.put(owner.client,base+types.client);memory.put(owner.client+0xc4,std::uint32_t{13});
        memory.put(owner.entity,base+types.entity);memory.put(base+types.manager_offset,std::uint32_t{0x80});
        memory.put(owner.client+8,context);memory.put(context+0x80,manager);memory.put(owner.entity+0x20,context);
        memory.put(manager,base+types.manager);memory.put(manager+0x4c8,begin);memory.put(manager+0x4d0,begin+8);
        memory.put(begin,player);memory.put(player,base+types.player);memory.put(player+0x78,context);
        memory.put(player+0x45,std::uint8_t{1});memory.put(player+0x44,std::uint8_t{0});memory.put(player+0xb8,owner.entity);
        memory.put(owner.entity+0xf8,player);memory.put(owner.entity+0x7e0,std::uint8_t{0});memory.put(player+0xb0,handle);
        memory.put(owner.entity+0x70,collection);memory.put(owner.entity+0x628,component);memory.put(component+0x70,owner.core);
        memory.put(owner.core+0x3c0,owner.context);memory.put(owner.core+0x440,selector);memory.put(owner.core+0x428,causes);
        memory.put(owner.core+0x438,owner.rig);memory.put(handle,owner.entity+8);memory.put(collection,owner.entity);
        memory.put(component,base+types.component);memory.put(component+0x18,collection);memory.put(owner.core,base+types.core);
        memory.put(selector+8,owner.context);memory.put(causes+0x20,owner.context);memory.put(owner.rig,owner.context);
        memory.put(owner.rig+0x4630,owner.core);
    }
    bool valid(){return audit::owner_matches(base,owner,types,[&](auto p,void* out,std::size_t n){return memory.read(p,out,n);});}
};
void ownership() {
    Fixture f;require(f.valid(),"Current supported canonical owner chain should qualify without SDK locks.");
    f.memory.changing=Fixture::player+0xb8;require(!f.valid(),"A player/entity replacement during qualification must reject.");
    Fixture remote;remote.memory.put(Fixture::player+0x44,std::uint8_t{1});require(!remote.valid(),"Remote player must reject.");
    Fixture teleport;teleport.memory.put(teleport.owner.entity+0x7e0,std::uint8_t{1});require(!teleport.valid(),"Teleporting owner must reject.");
    Fixture replacement;replacement.memory.put(Fixture::component+0x70,std::uintptr_t{0x600000});require(!replacement.valid(),"Replacement core cannot borrow an old frame.");
    Fixture rig;rig.memory.put(rig.owner.rig+0x4630,std::uintptr_t{0x600000});require(!rig.valid(),"Rig backlink must match the pinned core.");
    Fixture missing;missing.memory.bytes.erase(Fixture::causes+0x20);require(!missing.valid(),"An unreadable chain is unknown, never a match.");
    Fixture wrong_type;wrong_type.memory.put(Fixture::component,Fixture::base+wrong_type.types.component+1);require(!wrong_type.valid(),"Unexpected component class must reject.");
    Fixture stale;stale.owner.available=false;require(!stale.valid(),"Unavailable source frame cannot authorize getter context.");
    Fixture two;two.memory.put(Fixture::manager+0x4d0,Fixture::begin+16);require(!two.valid(),"Multiple local players must reject canonical correlation.");
    Fixture null_context;null_context.owner.context=0;null_context.memory.put(null_context.owner.core+0x3c0,std::uintptr_t{0});
    null_context.memory.put(Fixture::selector+8,std::uintptr_t{0});null_context.memory.put(Fixture::causes+0x20,std::uintptr_t{0});
    null_context.memory.put(null_context.owner.rig,std::uintptr_t{0});require(!null_context.valid(),"A mutually consistent null pinned context is still unavailable.");
    Fixture null_link;null_link.memory.put(null_link.owner.client+8,std::uintptr_t{0});
    require(!null_link.valid(),"A null client context must reject before following its manager offset.");
    for(const auto address:{f.owner.client+8,Fixture::context+0x80,Fixture::manager+0x4c8,Fixture::manager+0x4d0,
        Fixture::begin,Fixture::player+0xb0,f.owner.entity+0x70,f.owner.entity+0x628,Fixture::component+0x70,
        f.owner.core+0x3c0,f.owner.core+0x440,f.owner.core+0x428,f.owner.core+0x438}) {
        Fixture bad;bad.memory.put(address,std::numeric_limits<std::uintptr_t>::max()-4);
        require(!bad.valid(),"An unbounded linked pointer must reject before address arithmetic.");
    }
}
void window_and_clock() {
    audit::Window w;require(!w.live(0),"An unarmed audit has no capture permission.");
    require(w.begin(1000)&&w.live(1000)&&w.live(1249),"Audit must include each callback throughout its original window.");
    require(!w.live(999)&&!w.live(1250),"Clock reversal and exact deadline must reject.");
    require(!w.begin(2000)&&w.deadline==1250,"A second arm cannot silently extend a diagnostic window.");
    audit::Window overflow;require(!overflow.begin(std::numeric_limits<std::uint64_t>::max()-100),"Deadline arithmetic cannot wrap.");
    require(dingosdk::bmx::getup_action::exact({700,12},{700,12}),"Same native callback edge qualifies.");
    require(!dingosdk::bmx::getup_action::exact({700,12},{701,12.016666}),"A diagnostic getter cannot drift to another edge.");
    audit::Dispatch unknown;require(!unknown.available&&!unknown.called&&!unknown.serial_reserved_this_callback,
        "Action lock contention or absent instrumentation must not invent a selected candidate.");
}
}
int main(){ownership();window_and_clock();std::cout<<"GetUp callback audit ownership, window and unknown selection tests passed.\n";}
