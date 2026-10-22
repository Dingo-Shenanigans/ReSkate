#include "Extension/BMX/bmx_local_profile_guard.h"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string_view>

namespace {
namespace guard = dingosdk::bmx::local_profile_guard;
constexpr std::uintptr_t base_address=0x10000000, manager_address=0x20000, node_address=0x30000, service_address=0x40000;
constexpr std::uintptr_t records_address=0x50000, user_address=0x60000, second_user_address=0x70000;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Memory {
    std::map<std::uintptr_t,std::uint8_t> bytes;
    std::map<std::uintptr_t,unsigned> reads;
    std::function<void(std::uintptr_t,unsigned)> mutate;
    template<class T> void put(std::uintptr_t p, const T& value) {
        const auto* data=reinterpret_cast<const std::uint8_t*>(&value);
        for(std::size_t i=0;i<sizeof(value);++i) bytes[p+i]=data[i];
    }
    void zero(std::uintptr_t p,std::size_t size) {for(std::size_t i=0;i<size;++i)bytes[p+i]=0;}
    bool read(std::uintptr_t p,void* data,std::size_t size) {
        if(mutate)mutate(p,++reads[p]);
        auto* out=static_cast<std::uint8_t*>(data);
        for(std::size_t i=0;i<size;++i){const auto it=bytes.find(p+i);if(it==bytes.end())return false;out[i]=it->second;}
        return true;
    }
    auto inspect(std::uintptr_t image=base_address) {
        return guard::inspect_primary_user(image,[&](std::uintptr_t p,void* out,std::size_t size){return read(p,out,size);});
    }
    void tree_node(std::uintptr_t p,std::uintptr_t key,std::uintptr_t less=0,std::uintptr_t greater=0,std::uintptr_t value=0) {
        zero(p,0x30);put(p,greater);put(p+8,less);put(p+0x20,key);put(p+0x28,value);
    }
    void record(unsigned i,std::uint32_t index,std::uintptr_t value,std::uint8_t removed=0) {
        const auto p=records_address+std::uintptr_t{i}*0x28;zero(p,0x28);put(p+8,value);put(p+0x1c,index);put(p+0x20,removed);
    }
    void native_user(std::uintptr_t p=user_address,std::uint32_t index=0,std::uint8_t guest=1,std::uintptr_t provider=0) {
        put(p,base_address+guard::user_vtable_rva);put(p+0x140,index);put(p+0x148,provider);put(p+0x1b8,guest);
    }
    Memory() {
        put(base_address+guard::manager_rva,manager_address);put(base_address+guard::provider_rva,std::uintptr_t{});
        put(manager_address+0x18,node_address);put(manager_address+0x38,std::uintptr_t{7});tree_node(node_address,7,0,0,service_address);
        put(service_address,base_address+guard::service_vtable_rva);put(base_address+guard::service_vtable_rva+0x28,base_address+guard::service_method_rva);
        put(service_address+0x240,records_address);put(service_address+0x248,records_address+0x28);record(0,0,user_address);native_user();
        put(base_address+guard::user_vtable_rva+0x30,base_address+guard::user_method_rva);put(base_address+guard::user_method_rva,guard::user_method_bytes);
    }
};
void states() {
    Memory m;auto result=m.inspect();
    check(result.coherent&&result.eligible&&result.user==user_address&&result.service==service_address&&result.native_index==0&&
        result.guest==1&&result.visited_nodes==1&&result.record_count==1,"exact unsupported-provider primary native user is eligible");
    m.native_user(user_address,1);check(m.inspect().coherent&&!m.inspect().eligible,"native secondary user index cannot be substituted");
    m.native_user(user_address,0,0);check(m.inspect().coherent&&!m.inspect().eligible,"a nonguest primary user retains normal behavior");
    m.native_user(user_address,0,1,0x80000);check(!m.inspect().eligible,"an existing per-user provider preserves the native gate");
    m.native_user();m.put(base_address+guard::provider_rva,std::uintptr_t{0x80000});
    check(m.inspect().coherent&&!m.inspect().eligible,"an existing global provider preserves the native gate");
    Memory absent;absent.put(base_address+guard::manager_rva,std::uintptr_t{});
    check(absent.inspect().coherent&&!absent.inspect().eligible,"stable missing manager is observed absence, never eligibility");
    Memory empty;empty.put(service_address+0x248,records_address);
    check(empty.inspect().coherent&&!empty.inspect().eligible,"stable empty user range is ineligible");
}
void first_match() {
    Memory m;m.put(service_address+0x248,records_address+5*0x28);
    m.record(0,0,user_address,1);m.record(1,1,user_address);m.record(2,0,0);m.record(3,0,user_address);m.record(4,0,second_user_address);
    m.native_user(second_user_address,0,1);m.native_user(user_address,0,0);
    check(m.inspect().coherent&&!m.inspect().eligible&&m.inspect().user==user_address,
        "native first matching record wins after removed, nonprimary and null-user records");
    m.native_user(user_address);check(m.inspect().eligible,"valid first matching record is accepted despite later duplicate");
    m.put(user_address,base_address+guard::user_vtable_rva+8);
    check(!m.inspect().coherent&&!m.inspect().eligible,"unsupported first user cannot be replaced with later eligible duplicate");
    Memory lower;lower.tree_node(node_address,9,node_address+0x100,0,0x90000);lower.tree_node(node_address+0x100,7,0,0,service_address);
    check(lower.inspect().eligible&&lower.inspect().visited_nodes==2,"service lookup uses native lower-bound candidate");
    lower.tree_node(node_address+0x100,6);
    check(lower.inspect().coherent&&!lower.inspect().service&&!lower.inspect().eligible,"nearest nonmatching key is not the service");
}
void malformed_and_bounds() {
    for(unsigned what=0;what<9;++what){Memory m;
        if(what==0)m.put(service_address,base_address+guard::service_vtable_rva+8);
        if(what==1)m.put(base_address+guard::service_vtable_rva+0x28,base_address+guard::service_method_rva+1);
        if(what==2)m.put(base_address+guard::user_vtable_rva+0x30,base_address+guard::user_method_rva+1);
        if(what==3)m.put(base_address+guard::user_method_rva,std::uint8_t{});
        if(what==4)m.native_user(user_address,0,2);
        if(what==5)m.put(service_address+0x248,records_address-8);
        if(what==6)m.put(service_address+0x248,records_address+1);
        if(what==7){m.put(service_address+0x240,std::uintptr_t{0xfffffffffffffff8ULL});m.put(service_address+0x248,std::uintptr_t{0xfffffffffffffff8ULL});}
        if(what==8)m.record(0,0,user_address+1);
        check(!m.inspect().coherent&&!m.inspect().eligible,"malformed identity, method, boolean or pointer range fails closed");
    }
    Memory invalid_base;unsigned calls{};
    const auto result=guard::inspect_primary_user(guard::highest_address,[&](auto,auto,auto){++calls;return false;});
    check(!result.eligible&&!calls,"overflowing image base is rejected before any read");
    Memory unreadable;unreadable.bytes.erase(node_address+0x10);check(!unreadable.inspect().coherent,"unreadable tree node is unknown");
    Memory cycle;cycle.tree_node(node_address,7,node_address,0,service_address);
    check(std::string_view(cycle.inspect().status)=="cycle"&&!cycle.inspect().eligible,"cyclic native tree is bounded");
    Memory deep;for(unsigned i=0;i<64;++i)deep.tree_node(node_address+i*0x100,7,i==63?0:node_address+(i+1)*0x100,0,i==63?service_address:0);
    check(deep.inspect().coherent&&deep.inspect().eligible&&deep.inspect().visited_nodes==64,"64-node complete path is supported");
    deep.tree_node(node_address+63*0x100,7,node_address+64*0x100);
    check(std::string_view(deep.inspect().status)=="node_limit"&&!deep.inspect().eligible,"65th tree node is not followed");
    Memory many;many.put(service_address+0x248,records_address+64*0x28);
    for(unsigned i=0;i<64;++i)many.record(i,i==63?0:1,i==63?user_address:0);
    check(many.inspect().eligible&&many.inspect().record_count==64,"64 native records with primary last are supported");
    many.put(service_address+0x248,records_address+65*0x28);
    check(std::string_view(many.inspect().status)=="record_limit"&&!many.inspect().eligible,"65th record is rejected");
}
void races() {
    for(unsigned what=0;what<11;++what){Memory m;
        m.mutate=[&](std::uintptr_t p,unsigned n){
            if(what==0&&p==user_address&&n==2)m.native_user(user_address,1);
            if(what==1&&p==user_address&&n==2)m.put(user_address+0x148,std::uintptr_t{0x80000});
            if(what==2&&p==base_address+guard::user_method_rva&&n==2)m.put(base_address+guard::provider_rva,std::uintptr_t{0x80000});
            if(what==3&&p==records_address&&n==2)m.record(0,0,second_user_address);
            if(what==4&&p==node_address&&n==2)m.tree_node(node_address,8,0,0,service_address);
            if(what==5&&p==base_address+guard::user_method_rva&&n==2)m.put(service_address+0x248,records_address);
            if(what==6&&p==base_address+guard::user_method_rva&&n==2)m.put(manager_address+0x18,std::uintptr_t{});
            if(what==7&&p==base_address+guard::user_method_rva&&n==2)m.put(base_address+guard::manager_rva,std::uintptr_t{});
            if(what==8&&p==records_address&&n==2)m.native_user(user_address,0,0);
            if(what==9&&p==user_address+0x1b8&&n==3)m.put(user_address,base_address+guard::user_vtable_rva+8);
            if(what==10&&p==user_address+0x1b8&&n==3)m.put(base_address+guard::user_vtable_rva+0x30,base_address+guard::user_method_rva+1);
        };
        const auto result=m.inspect();
        check(!result.coherent&&!result.eligible&&std::string_view(result.status)=="changed",
            "changes to user, provider, records, tree and lookup roots are rejected");
    }
    Memory exception;const auto result=guard::inspect_primary_user(base_address,[](auto,auto,auto)->bool{throw 1;});
    check(!result.coherent&&!result.eligible&&std::string_view(result.status)=="reader_exception","reader exceptions fail closed");
}
}
int main(){states();first_match();malformed_and_bounds();races();std::cout<<"BMX native primary-user guard cases passed\n";}
