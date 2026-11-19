#include "Engine/Game/Profile/local_settings_guard.h"
#include "Engine/Game/Build/20260929/local_native_settings.h"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {
namespace guard = dingosdk::local_settings_guard;
namespace pins = dingosdk::game::build::v20260929::local_native_settings;
using Layout = pins::Layout;
unsigned groups{}, checks{};
void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void run(void (*group)()) { ++groups; group(); }
// Addresses below are synthetic fixtures; the executable layout comes only
// from its build header. No game, filesystem or native function is accessed.
namespace primary_user_tests {
constexpr std::uintptr_t base_address=0x10000000, manager_address=0x20000, node_address=0x30000, service_address=0x40000;
constexpr std::uintptr_t records_address=0x50000, user_address=0x60000, second_user_address=0x70000;

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
        return guard::inspect_primary_user(image,pins::guard_layout,[&](std::uintptr_t p,void* out,std::size_t size){return read(p,out,size);});
    }
    void tree_node(std::uintptr_t p,std::uintptr_t key,std::uintptr_t less=0,std::uintptr_t greater=0,std::uintptr_t value=0) {
        zero(p,Layout::tree_node_size);put(p+Layout::tree_right,greater);put(p+Layout::tree_left,less);put(p+Layout::tree_key,key);put(p+Layout::tree_service,value);
    }
    void record(unsigned i,std::uint32_t index,std::uintptr_t value,std::uint8_t removed=0) {
        const auto p=records_address+std::uintptr_t{i}*Layout::record_stride;zero(p,Layout::record_stride);put(p+Layout::record_user,value);put(p+Layout::record_index,index);put(p+Layout::record_removed,removed);
    }
    void native_user(std::uintptr_t p=user_address,std::uint32_t index=0,std::uint8_t guest=1,std::uintptr_t provider=0) {
        put(p,base_address+Layout::user_vtable_rva);put(p+Layout::user_index,index);put(p+Layout::user_provider,provider);put(p+Layout::user_guest,guest);
    }
    Memory() {
        put(base_address+Layout::manager_rva,manager_address);put(base_address+Layout::provider_rva,std::uintptr_t{});
        put(manager_address+Layout::manager_head,node_address);put(manager_address+Layout::manager_key,std::uintptr_t{7});tree_node(node_address,7,0,0,service_address);
        put(service_address,base_address+Layout::service_vtable_rva);put(base_address+Layout::service_vtable_rva+Layout::service_method_slot,base_address+Layout::service_method_rva);
        put(service_address+Layout::records_begin,records_address);put(service_address+Layout::records_end,records_address+Layout::record_stride);record(0,0,user_address);native_user();
        put(base_address+Layout::user_vtable_rva+Layout::user_method_slot,base_address+Layout::user_method_rva);put(base_address+Layout::user_method_rva,Layout::user_method_prefix);
    }
};
void states() {
    Memory m;auto result=m.inspect();
    check(result.coherent&&result.eligible&&result.user==user_address&&result.service==service_address&&result.native_index==0&&
        result.guest==1&&result.visited_nodes==1&&result.record_count==1,"exact unsupported-provider primary native user is eligible");
    m.native_user(user_address,1);check(m.inspect().coherent&&!m.inspect().eligible,"native secondary user index cannot be substituted");
    m.native_user(user_address,0,0);check(m.inspect().coherent&&!m.inspect().eligible,"a nonguest primary user retains normal behavior");
    m.native_user(user_address,0,1,0x80000);check(!m.inspect().eligible,"an existing per-user provider preserves the native gate");
    m.native_user();m.put(base_address+Layout::provider_rva,std::uintptr_t{0x80000});
    check(m.inspect().coherent&&!m.inspect().eligible,"an existing global provider preserves the native gate");
    Memory absent;absent.put(base_address+Layout::manager_rva,std::uintptr_t{});
    check(absent.inspect().coherent&&!absent.inspect().eligible,"stable missing manager is observed absence, never eligibility");
    Memory empty;empty.put(service_address+Layout::records_end,records_address);
    check(empty.inspect().coherent&&!empty.inspect().eligible,"stable empty user range is ineligible");
}
void first_match() {
    Memory m;m.put(service_address+Layout::records_end,records_address+5*Layout::record_stride);
    m.record(0,0,user_address,1);m.record(1,1,user_address);m.record(2,0,0);m.record(3,0,user_address);m.record(4,0,second_user_address);
    m.native_user(second_user_address,0,1);m.native_user(user_address,0,0);
    check(m.inspect().coherent&&!m.inspect().eligible&&m.inspect().user==user_address,
        "native first matching record wins after removed, nonprimary and null-user records");
    m.native_user(user_address);check(m.inspect().eligible,"valid first matching record is accepted despite later duplicate");
    m.put(user_address,base_address+Layout::user_vtable_rva+8);
    check(!m.inspect().coherent&&!m.inspect().eligible,"unsupported first user cannot be replaced with later eligible duplicate");
    Memory lower;lower.tree_node(node_address,9,node_address+0x100,0,0x90000);lower.tree_node(node_address+0x100,7,0,0,service_address);
    check(lower.inspect().eligible&&lower.inspect().visited_nodes==2,"service lookup uses native lower-bound candidate");
    lower.tree_node(node_address+0x100,6);
    check(lower.inspect().coherent&&!lower.inspect().service&&!lower.inspect().eligible,"nearest nonmatching key is not the service");
}
void malformed_and_bounds() {
    for(unsigned what=0;what<9;++what){Memory m;
        if(what==0)m.put(service_address,base_address+Layout::service_vtable_rva+8);
        if(what==1)m.put(base_address+Layout::service_vtable_rva+Layout::service_method_slot,base_address+Layout::service_method_rva+1);
        if(what==2)m.put(base_address+Layout::user_vtable_rva+Layout::user_method_slot,base_address+Layout::user_method_rva+1);
        if(what==3)m.put(base_address+Layout::user_method_rva,std::uint8_t{});
        if(what==4)m.native_user(user_address,0,2);
        if(what==5)m.put(service_address+Layout::records_end,records_address-8);
        if(what==6)m.put(service_address+Layout::records_end,records_address+1);
        if(what==7){m.put(service_address+Layout::records_begin,std::uintptr_t{0xfffffffffffffff8ULL});m.put(service_address+Layout::records_end,std::uintptr_t{0xfffffffffffffff8ULL});}
        if(what==8)m.record(0,0,user_address+1);
        check(!m.inspect().coherent&&!m.inspect().eligible,"malformed identity, method, boolean or pointer range fails closed");
    }
    unsigned calls{};
    const auto result=guard::inspect_primary_user(Layout::highest_address,pins::guard_layout,[&](auto,auto,auto){++calls;return false;});
    check(!result.eligible&&!calls,"overflowing image base is rejected before any read");
    Memory unreadable;unreadable.bytes.erase(node_address+2*Layout::pointer_size);check(!unreadable.inspect().coherent,"unreadable tree node is unknown");
    Memory cycle;cycle.tree_node(node_address,7,node_address,0,service_address);
    check(std::string_view(cycle.inspect().status)=="cycle"&&!cycle.inspect().eligible,"cyclic native tree is bounded");
    Memory deep;for(unsigned i=0;i<64;++i)deep.tree_node(node_address+i*0x100,7,i==63?0:node_address+(i+1)*0x100,0,i==63?service_address:0);
    check(deep.inspect().coherent&&deep.inspect().eligible&&deep.inspect().visited_nodes==64,"64-node complete path is supported");
    deep.tree_node(node_address+63*0x100,7,node_address+64*0x100);
    check(std::string_view(deep.inspect().status)=="node_limit"&&!deep.inspect().eligible,"65th tree node is not followed");
    Memory many;many.put(service_address+Layout::records_end,records_address+64*Layout::record_stride);
    for(unsigned i=0;i<64;++i)many.record(i,i==63?0:1,i==63?user_address:0);
    check(many.inspect().eligible&&many.inspect().record_count==64,"64 native records with primary last are supported");
    many.put(service_address+Layout::records_end,records_address+65*Layout::record_stride);
    check(std::string_view(many.inspect().status)=="record_limit"&&!many.inspect().eligible,"65th record is rejected");
}
void races() {
    for(unsigned what=0;what<11;++what){Memory m;
        m.mutate=[&](std::uintptr_t p,unsigned n){
            if(what==0&&p==user_address&&n==2)m.native_user(user_address,1);
            if(what==1&&p==user_address&&n==2)m.put(user_address+Layout::user_provider,std::uintptr_t{0x80000});
            if(what==2&&p==base_address+Layout::user_method_rva&&n==2)m.put(base_address+Layout::provider_rva,std::uintptr_t{0x80000});
            if(what==3&&p==records_address&&n==2)m.record(0,0,second_user_address);
            if(what==4&&p==node_address&&n==2)m.tree_node(node_address,8,0,0,service_address);
            if(what==5&&p==base_address+Layout::user_method_rva&&n==2)m.put(service_address+Layout::records_end,records_address);
            if(what==6&&p==base_address+Layout::user_method_rva&&n==2)m.put(manager_address+Layout::manager_head,std::uintptr_t{});
            if(what==7&&p==base_address+Layout::user_method_rva&&n==2)m.put(base_address+Layout::manager_rva,std::uintptr_t{});
            if(what==8&&p==records_address&&n==2)m.native_user(user_address,0,0);
            if(what==9&&p==user_address+Layout::user_guest&&n==3)m.put(user_address,base_address+Layout::user_vtable_rva+8);
            if(what==10&&p==user_address+Layout::user_guest&&n==3)m.put(base_address+Layout::user_vtable_rva+Layout::user_method_slot,base_address+Layout::user_method_rva+1);
        };
        const auto result=m.inspect();
        check(!result.coherent&&!result.eligible&&std::string_view(result.status)=="changed",
            "changes to user, provider, records, tree and lookup roots are rejected");
    }
    const auto result=guard::inspect_primary_user(base_address,pins::guard_layout,[](auto,auto,auto)->bool{throw 1;});
    check(!result.coherent&&!result.eligible&&std::string_view(result.status)=="reader_exception","reader exceptions fail closed");
}
} // namespace primary_user_tests
namespace storage_path_tests {
constexpr std::uintptr_t base_address=0x10000000, platform_address=0x20000;
constexpr std::uintptr_t configuration_address=0x30000, path_address=0x40000;

struct Memory {
    std::map<std::uintptr_t, std::uint8_t> bytes;
    std::function<void(std::uintptr_t, std::size_t)> mutate;
    unsigned content_reads{};
    template<class T> void put(std::uintptr_t address, const T& value) {
        const auto* data=reinterpret_cast<const std::uint8_t*>(&value);
        for(std::size_t i=0;i<sizeof(value);++i)bytes[address+i]=data[i];
    }
    void path(std::string_view value) {
        for(std::size_t i=0;i<value.size();++i)put(path_address+i,value[i]);
        put(path_address+value.size(),char{});
    }
    bool read(std::uintptr_t address,void* data,std::size_t size) {
        if(address==path_address&&size>1)++content_reads;
        if(mutate)mutate(address,size);
        auto* out=static_cast<std::uint8_t*>(data);
        for(std::size_t i=0;i<size;++i){const auto found=bytes.find(address+i);if(found==bytes.end())return false;out[i]=found->second;}
        return true;
    }
    auto inspect(std::uintptr_t image=base_address,std::uintptr_t platform=platform_address,
        std::uintptr_t configuration=configuration_address) {
        return guard::inspect_storage_path(image,platform,configuration,pins::guard_layout,
            [&](std::uintptr_t address,void* data,std::size_t size){return read(address,data,size);});
    }
    Memory(){put(platform_address,base_address+Layout::pc_storage_vtable_rva);
        put(configuration_address+Layout::configuration_path,path_address);path("C:\\tests\\reskate\\Game\\Skate\\data");}
};
void payloads() {
    Memory memory;const std::string ascii="C:\\tests\\reskate\\Game\\Skate\\data";
    const auto result=memory.inspect();
    check(result.coherent&&result.path_pointer==path_address&&result.length==ascii.size()&&
        std::string_view(result.text.data(),result.length)==ascii&&result.text[result.length]==0,
        "ASCII native Windows absolute path is read as narrow bytes");
    const std::string utf8="C:\\tests\\r\xc3\xa9skate\\\xe8\xb7\xaf\\data";
    memory.path(utf8);const auto unicode=memory.inspect();
    check(unicode.coherent&&unicode.length==utf8.size()&&std::string_view(unicode.text.data(),unicode.length)==utf8,
        "multibyte UTF-8 survives exactly with byte length");
    memory.path(std::string(260,'x'));const auto maximum=memory.inspect();
    check(maximum.coherent&&maximum.length==260&&maximum.text[260]==0,"260-byte payload plus NUL fits the native bound");
    memory.path("x");check(memory.inspect().coherent,"one-byte payload needs no readable memory beyond its NUL");
    memory.path(std::string("x\xff",2));check(memory.inspect().coherent,"UTF-8 validity belongs to the conversion caller");
}
void malformed() {
    Memory empty;empty.path("");check(!empty.inspect().coherent,"empty path is rejected");
    Memory long_path;long_path.path(std::string(261,'x'));
    check(!long_path.inspect().coherent&&std::string_view(long_path.inspect().status)=="invalid_path_length",
        "261 nonzero bytes are rejected without an unbounded search");
    Memory unterminated;unterminated.bytes.erase(path_address+31);
    check(!unterminated.inspect().coherent,"unreadable payload or terminator is rejected");
    Memory wrong_type;wrong_type.put(platform_address,base_address+Layout::pc_storage_vtable_rva+8);
    check(!wrong_type.inspect().coherent,"only exact native PC storage type is supported");
    Memory null_path;null_path.put(configuration_address+Layout::configuration_path,std::uintptr_t{});
    check(!null_path.inspect().coherent,"null path pointer is rejected");
    Memory overflow;overflow.put(configuration_address+Layout::configuration_path,std::uintptr_t{0xffffffffffffffffULL});
    check(!overflow.inspect().coherent,"overflowing path pointer is rejected before character reads");
    Memory edge;edge.put(configuration_address+Layout::configuration_path,Layout::highest_address-1);edge.put(Layout::highest_address-1,'x');
    check(!edge.inspect().coherent,"a path crossing the canonical address boundary is rejected");
    Memory missing;missing.bytes.erase(configuration_address+Layout::configuration_path+Layout::pointer_size/2);
    check(!missing.inspect().coherent,"unreadable configuration pointer is rejected");
    for(const auto value:{std::uintptr_t{},Layout::highest_address,std::uintptr_t{0xfffffffffffffff8ULL}}){
        unsigned reads{};const auto rejected=guard::inspect_storage_path(value,platform_address,configuration_address,pins::guard_layout,
            [&](auto,auto,auto){++reads;return false;});
        check(!rejected.coherent&&!reads,"invalid image base is rejected before target reads");
    }
    Memory addresses;
    check(!addresses.inspect(base_address,platform_address+1).coherent&&
        !addresses.inspect(base_address,platform_address,configuration_address+1).coherent&&
        !addresses.inspect(base_address,platform_address,Layout::highest_address-Layout::configuration_size/2).coherent,
        "unaligned or overflowing native object spans are rejected");
}
void races() {
    for(unsigned kind=0;kind<5;++kind){Memory memory;
        memory.mutate=[&](std::uintptr_t address,std::size_t size){
            if(address!=path_address||size<=1)return;
            if(kind==0&&memory.content_reads==1)memory.put(path_address,'D');
            if(kind==1&&memory.content_reads==1)memory.put(configuration_address+Layout::configuration_path,path_address+0x100);
            if(kind==2&&memory.content_reads==1)memory.put(platform_address,base_address+Layout::pc_storage_vtable_rva+8);
            if(kind==3&&memory.content_reads==2)memory.put(path_address,'D');
            if(kind==4&&memory.content_reads==2)memory.put(platform_address,base_address+Layout::pc_storage_vtable_rva+8);
        };
        const auto result=memory.inspect();
        check(!result.coherent&&std::string_view(result.status)=="changed","content, pointer and platform-type races are rejected");
    }
    const auto result=guard::inspect_storage_path(base_address,platform_address,configuration_address,pins::guard_layout,
        [](auto,auto,auto)->bool{throw 1;});
    check(!result.coherent&&std::string_view(result.status)=="reader_exception","reader exceptions fail closed");
}
} // namespace storage_path_tests

namespace query_tests {
constexpr std::uintptr_t image_base = 0x10000000, manager = 0x20000;
constexpr std::uintptr_t table = 0x30000, object = 0x40000, synthetic_vtable = 0x50000;
struct Memory {
    std::map<std::uintptr_t, std::uint8_t> bytes;
    template<class T> void put(std::uintptr_t p, const T& value) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[p+i] = data[i];
    }
    bool read(std::uintptr_t p, void* data, std::size_t size) {
        auto* out = static_cast<std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(p+i);
            if (found == bytes.end()) return false;
            out[i] = found->second;
        }
        return true;
    }
    auto inspect() {
        return guard::inspect_profile_query(image_base,pins::guard_layout,
            [&](auto p,auto out,auto size) { return read(p,out,size); });
    }
    void root(std::uintptr_t vtable = synthetic_vtable, std::uintptr_t method = table+0x100) {
        put(image_base+Layout::query_manager_rva,manager);
        put(manager+Layout::query_begin,table);
        put(manager+Layout::query_end,table+Layout::query_slots*Layout::pointer_size);
        put(table,object); put(object,vtable); put(vtable+Layout::query_method_slot,method);
    }
    void native_root() {
        for (std::size_t i=0;i<Layout::query_size;++i) put(object+i,std::uint8_t{});
        root(image_base+Layout::query_vtable_rva,image_base+Layout::query_method_rva);
        put(image_base+Layout::query_method_rva,Layout::query_method_prefix);
    }
};
void native_snapshot() {
    Memory m;m.native_root();
    m.put(object+Layout::query_storage,manager+0x100);
    m.put(object+Layout::query_completed,std::uint8_t{1});
    m.put(object+Layout::query_list_request,std::uint32_t{23});
    m.put(object+Layout::query_read_request,std::uint32_t{24});
    m.put(object+Layout::query_load_state,std::uint32_t{5});
    m.put(object+Layout::query_save_state,std::uint32_t{1});
    const auto read = [&](auto p,auto out,auto size) { return m.read(p,out,size); };
    const auto query=m.inspect();
    const auto details=guard::inspect_native_profile_query(image_base,query,pins::guard_layout,read);
    check(details.coherent && details.completed==1 && details.storage_present && details.list_request==23 &&
        details.read_request==24 && details.load_state==5 && details.save_state==1 &&
        details.deferred_load==0 && details.deferred_save==0,
        "typed query retains completion and independent native request observations");
    auto wrong=query;wrong.vtable+=Layout::pointer_size;
    check(!guard::inspect_native_profile_query(image_base,wrong,pins::guard_layout,read).coherent,
        "another query type has no known completion layout");
    m.put(image_base+Layout::query_method_rva,std::uint8_t{});
    check(!guard::inspect_native_profile_query(image_base,query,pins::guard_layout,read).coherent,
        "completion method fingerprint must match");
    m.put(image_base+Layout::query_method_rva,Layout::query_method_prefix);
    unsigned reads{};
    const auto changed=guard::inspect_native_profile_query(image_base,query,pins::guard_layout,
        [&](auto p,auto out,auto size) {
            if(p==object && size==Layout::query_size && ++reads==2) m.put(object+Layout::query_completed,std::uint8_t{});
            return m.read(p,out,size);
        });
    check(!changed.coherent,"query payload mutation between snapshots is unavailable");
    for(unsigned phase=0;phase<3;++phase){
        const auto failed=guard::inspect_native_profile_query(image_base,query,pins::guard_layout,
            [&](auto p,auto out,auto size) {
                if((phase==0 && p==image_base+Layout::query_method_rva) ||
                    (phase==1 && p==object && size==Layout::query_size) ||
                    (phase==2 && p==image_base+Layout::query_manager_rva)) throw 1;
                return m.read(p,out,size);
            });
        check(!failed.coherent && std::string_view(failed.status)=="reader_exception",
            "reader exceptions during prefix, payload or reroot reads fail closed");
    }
}
void query_roots() {
    Memory m;
    m.put(image_base+Layout::query_manager_rva,std::uintptr_t{});
    check(m.inspect().coherent && !m.inspect().manager,"missing manager is coherent absence");
    m.put(image_base+Layout::query_manager_rva,manager);
    m.put(manager+Layout::query_begin,table);m.put(manager+Layout::query_end,table);
    check(m.inspect().coherent && !m.inspect().object,"empty query vector is coherent absence");
    m.root();
    check(m.inspect().coherent && m.inspect().function==table+0x100,
        "query method identity is observed without invoking it");
    m.put(manager+Layout::query_end,table+(Layout::query_slots+1)*Layout::pointer_size);
    check(!m.inspect().coherent,"unexpected query vector size is unavailable");
    m.root();unsigned reads{};
    const auto changed=guard::inspect_profile_query(image_base,pins::guard_layout,
        [&](auto p,auto out,auto size) {
            if(p==synthetic_vtable+Layout::query_method_slot && ++reads==2) m.put(manager+Layout::query_end,table);
            return m.read(p,out,size);
        });
    check(!changed.coherent,"query roots still match after method identity reread");
    m.root();reads=0;
    const auto object_changed=guard::inspect_profile_query(image_base,pins::guard_layout,
        [&](auto p,auto out,auto size) {
            if(p==synthetic_vtable+Layout::query_method_slot && ++reads==2) m.put(table,object+0x100);
            return m.read(p,out,size);
        });
    check(!object_changed.coherent,"query object mutation during method reread is rejected");
    m.root();reads=0;
    const auto vtable_changed=guard::inspect_profile_query(image_base,pins::guard_layout,
        [&](auto p,auto out,auto size) {
            if(p==synthetic_vtable+Layout::query_method_slot && ++reads==2) m.put(object,synthetic_vtable+0x100);
            return m.read(p,out,size);
        });
    check(!vtable_changed.coherent,"query vtable mutation during method reread is rejected");
    m.put(manager+Layout::query_begin,std::uintptr_t{0xfffffffffffffff8ULL});
    m.put(manager+Layout::query_end,std::uintptr_t{0xfffffffffffffff8ULL});
    check(!m.inspect().coherent,"noncanonical empty query vector is unavailable");
    const auto failed=guard::inspect_profile_query(image_base,pins::guard_layout,
        [](auto,auto,auto)->bool{throw 1;});
    check(!failed.coherent && std::string_view(failed.status)=="reader_exception",
        "query-root reader exceptions fail closed");
}
void witness_composition() {
    Memory m;m.native_root();
    const auto opening=m.inspect();
    const auto typed=guard::inspect_native_profile_query(image_base,opening,pins::guard_layout,
        [&](auto p,auto out,auto size) { return m.read(p,out,size); });
    primary_user_tests::Memory users;
    const auto user=users.inspect();
    check(guard::observations_qualify(object,opening,typed,user,m.inspect(),users.inspect()),
        "independent coherent query and user witnesses qualify only while both still agree");
    check(!guard::observations_qualify(object+Layout::pointer_size,opening,typed,user,opening,user),
        "an eligible user cannot substitute for the requested query identity");
    auto unknown_query=opening;unknown_query.coherent=false;
    check(!guard::observations_qualify(object,unknown_query,typed,user,opening,user) &&
        !guard::observations_qualify(object,opening,typed,user,unknown_query,user),
        "unknown opening or closing query observations cannot qualify");
    auto wrong_query=opening;wrong_query.vtable+=Layout::pointer_size;
    const auto unsupported=guard::inspect_native_profile_query(image_base,wrong_query,pins::guard_layout,
        [&](auto p,auto out,auto size) { return m.read(p,out,size); });
    check(!guard::observations_qualify(object,opening,unsupported,user,opening,user),
        "a different native query type has no qualifying typed witness");
    auto closing=opening;closing.object+=Layout::pointer_size;
    check(!guard::observations_qualify(object,opening,typed,user,closing,user),
        "query identity drift at the closing boundary cannot qualify");
    closing=opening;closing.function+=Layout::pointer_size;
    check(!guard::observations_qualify(object,opening,typed,user,closing,user),
        "query method drift at the closing boundary cannot qualify");
    auto unknown_user=user;unknown_user.coherent=false;
    check(!guard::observations_qualify(object,opening,typed,unknown_user,opening,user) &&
        !guard::observations_qualify(object,opening,typed,user,opening,unknown_user),
        "unknown opening or closing user observations cannot qualify");
    auto ineligible=user;ineligible.eligible=false;
    check(!guard::observations_qualify(object,opening,typed,ineligible,opening,ineligible),
        "coherent but ineligible users retain the native gate");
    for(unsigned field=0;field<8;++field){
        auto changed=user;
        if(field==0)changed.manager+=Layout::pointer_size;
        if(field==1)changed.service+=Layout::pointer_size;
        if(field==2)changed.user+=Layout::pointer_size;
        if(field==3)changed.provider=0x80000;
        if(field==4)changed.global_provider=0x80000;
        if(field==5)++changed.native_index;
        if(field==6)changed.guest=0;
        if(field==7)++changed.record_count;
        check(!guard::observations_qualify(object,opening,typed,user,opening,changed),
            "every closing user identity, provider, native index or guest drift is rejected");
    }
}
} // namespace query_tests
}
int main() {
    run(primary_user_tests::states);run(primary_user_tests::first_match);
    run(primary_user_tests::malformed_and_bounds);run(primary_user_tests::races);
    run(storage_path_tests::payloads);run(storage_path_tests::malformed);run(storage_path_tests::races);
    run(query_tests::native_snapshot);run(query_tests::query_roots);run(query_tests::witness_composition);
    std::cout << "Local settings guards: " << groups << " groups, " << checks << " checks passed\n";
}
