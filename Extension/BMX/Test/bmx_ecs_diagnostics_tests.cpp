#include "Extension/BMX/bmx_ecs_diagnostics_guard.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string_view>

namespace {
using namespace dingosdk::bmx::ecs_detail;
constexpr std::uintptr_t queue = 0x20000, table = 0x30000, node = 0x40000, observer = 0x50000, vtable = 0x60000;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Memory {
    std::map<std::uintptr_t, unsigned char> bytes;
    bool change_node{}, change_head{};
    unsigned node_reads{}, head_reads{};
    template<class T> void put(std::uintptr_t p, T value) {
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i) bytes[p + i] = data[i];
    }
    bool read(std::uintptr_t p, void* out, std::size_t size) {
        if (p == node && ++node_reads == 2 && change_node) put(node, initial_event_hash + 1);
        if (p == table + (initial_event_hash % 3) * 8 && ++head_reads == 2 && change_head) put(p, std::uintptr_t{});
        auto* data = static_cast<unsigned char*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(p + i);
            if (found == bytes.end()) return false;
            data[i] = found->second;
        }
        return true;
    }
    Bucket inspect() { return inspect_bucket(queue, [this](std::uintptr_t p, void* out, std::size_t size) { return read(p, out, size); }); }
    void root(std::uintptr_t head = 0) {
        put(queue + 0x18b8, table); put(queue + 0x18c0, std::uint32_t{3});
        put(table + (initial_event_hash % 3) * 8, head); put(table + 3 * 8, std::uintptr_t{});
    }
    void receiver(std::uintptr_t p, std::uint32_t hash, std::uintptr_t next = 0) {
        put(p, hash); put(p + 8, observer); put(p + 0x10, next); put(observer, vtable);
    }
};
}
int main() {
    {
        Memory m;
        constexpr std::uintptr_t image_base=0x10000000, global=image_base+0x71eff00;
        const auto read=[&](std::uintptr_t p,void* out,std::size_t size){return m.read(p,out,size);};
        for(unsigned i=0;i<0x90;++i)m.put(observer+i,std::uint8_t{});
        m.put(global,node);m.put(node+0x40,table);m.put(node+0x48,table+24);m.put(table,observer);
        m.put(observer,image_base+0x608fc38);m.put(image_base+0x608fc40,image_base+0x204a50);
        const std::array<std::uint8_t,5> prefix{0x0f,0xb6,0x41,0x70,0xc3};
        for(unsigned i=0;i<prefix.size();++i)m.put(image_base+0x204a50+i,prefix[i]);
        m.put(observer+0x60,node+0x100);m.put(observer+0x70,std::uint8_t{1});
        m.put(observer+0x74,std::uint32_t{23});m.put(observer+0x78,std::uint32_t{24});
        m.put(observer+0x80,std::uint32_t{5});m.put(observer+0x88,std::uint32_t{1});
        const auto query=inspect_profile_query(global,read);
        const auto details=inspect_native_profile_query(image_base,query,read);
        check(details.coherent && details.completed==1 && details.storage_present && details.list_request==23 &&
            details.read_request==24 && details.load_state==5,"typed native query state must retain completion and request identity");
        auto wrong=query;wrong.vtable+=8;
        check(!inspect_native_profile_query(image_base,wrong,read).coherent,"a different native query type has no known completion offset");
        m.put(image_base+0x204a50,std::uint8_t{0});
        check(!inspect_native_profile_query(image_base,query,read).coherent,"completion method fingerprint must match");
        m.put(image_base+0x204a50,prefix[0]);
        unsigned reads{};
        const auto changed=inspect_native_profile_query(image_base,query,[&](std::uintptr_t p,void* out,std::size_t size){
            if(p==observer && size==0x90 && ++reads==2)m.put(observer+0x70,std::uint8_t{});
            return m.read(p,out,size);
        });
        check(!changed.coherent,"query state mutation between snapshots is unavailable");
    }
    {
        Memory m;
        const auto read = [&](std::uintptr_t p, void* out, std::size_t size) { return m.read(p, out, size); };
        const auto inspect = [&] { return inspect_profile_query(queue, read); };
        m.put(queue, std::uintptr_t{});
        check(inspect().coherent && !inspect().manager, "missing manager is coherent absence");
        m.put(queue, node); m.put(node + 0x40, table); m.put(node + 0x48, table);
        check(inspect().coherent && !inspect().object, "empty query vector is coherent absence");
        m.put(node + 0x48, table + 24); m.put(table, observer); m.put(observer, vtable); m.put(vtable + 8, table + 0x100);
        check(inspect().coherent && inspect().function == table + 0x100, "query function identity is observed without a call");
        m.put(node + 0x48, table + 32);
        check(!inspect().coherent, "unexpected query vector size is unavailable");
        m.put(node + 0x48, table + 24);
        unsigned reads{};
        const auto changed = inspect_profile_query(queue, [&](std::uintptr_t p, void* out, std::size_t size) {
            if (p == vtable + 8 && ++reads == 2) m.put(node + 0x48, table);
            return m.read(p, out, size);
        });
        check(!changed.coherent, "query roots must still match after method identity reread");
        m.put(node + 0x48, table + 24); reads = 0;
        const auto query_changed = inspect_profile_query(queue, [&](std::uintptr_t p, void* out, std::size_t size) {
            if (p == vtable + 8 && ++reads == 2) m.put(table, observer + 0x100);
            return m.read(p, out, size);
        });
        check(!query_changed.coherent, "group zero identity mutation during method reread invalidates snapshot");
        m.put(table, observer); reads = 0;
        const auto vtable_changed = inspect_profile_query(queue, [&](std::uintptr_t p, void* out, std::size_t size) {
            if (p == vtable + 8 && ++reads == 2) m.put(observer, vtable + 0x100);
            return m.read(p, out, size);
        });
        check(!vtable_changed.coherent, "query vtable mutation during method reread invalidates snapshot");
        m.put(node + 0x40, std::uintptr_t{0xfffffffffffffff8ULL}); m.put(node + 0x48, std::uintptr_t{0xfffffffffffffff8ULL});
        check(!inspect().coherent, "noncanonical empty query vector is unavailable");
    }
    {
        Memory m;
        const auto inspect = [&](auto&& read) { return inspect_capabilities(queue, 0x80, read); };
        const auto read = [&](std::uintptr_t p, void* out, std::size_t size) { return m.read(p, out, size); };
        m.put(queue + 0x80, std::uintptr_t{}); m.put(queue + 0x88, std::uintptr_t{});
        check(inspect(read).coherent && !inspect(read).data_model, "empty capability vector is coherent absence");
        m.put(queue + 0x80, std::uintptr_t{0xfffffffffffffffcULL});
        m.put(queue + 0x88, std::uintptr_t{0xfffffffffffffffcULL});
        check(!inspect(read).coherent, "noncanonical empty vector root must not prove absence");
        m.put(queue + 0x80, table); m.put(queue + 0x88, table + 12);
        m.put(table, std::uint32_t{7}); m.put(table + 4, std::uint32_t{0xd209f736}); m.put(table + 8, std::uint32_t{0x3c6f6595});
        const auto found = inspect(read);
        check(found.coherent && found.data_model && found.schematics && found.count == 3, "capability membership uses exact hashes");
        unsigned reads{};
        const auto changed = inspect([&](std::uintptr_t p, void* out, std::size_t size) {
            if (p == table && ++reads == 2) m.put(table, std::uint32_t{8});
            return m.read(p, out, size);
        });
        check(!changed.coherent && std::string_view{changed.status} == "changed", "capability entry mutation rejects absence or presence");
        reads = 0;
        const auto header_changed = inspect([&](std::uintptr_t p, void* out, std::size_t size) {
            if (p == table && ++reads == 2) m.put(queue + 0x88, table + 8);
            return m.read(p, out, size);
        });
        check(!header_changed.coherent && std::string_view{header_changed.status} == "changed", "header mutation during second data read rejects the snapshot");
        m.put(queue + 0x88, table + 257 * 4);
        check(std::string_view{inspect(read).status} == "limit", "capability vector is bounded");
        m.put(queue + 0x88, table - 4);
        check(!inspect(read).coherent, "reversed capability vector is unavailable");
        m.put(queue + 0x88, table + 4); m.bytes.erase(table);
        check(!inspect(read).coherent, "unreadable capability vector is unavailable");
        check(!inspect_capabilities(highest_address - 8, 0x80, read).coherent, "capability offset overflow rejected");
    }
    {
        Memory m; m.root(); const auto r = m.inspect();
        check(r.coherent && r.matches == 0 && r.visited == 0, "empty bucket must be a complete absence observation");
    }
    {
        Memory m; m.root(node); m.receiver(node, 7, node + 0x20); m.receiver(node + 0x20, initial_event_hash);
        const auto r = m.inspect();
        check(r.coherent && r.matches == 1 && r.visited == 2 && r.receivers[0].vtable == vtable, "collision chain must find the exact event receiver");
    }
    {
        Memory m; m.root(node); m.receiver(node, initial_event_hash, node + 0x20); m.receiver(node + 0x20, initial_event_hash);
        check(m.inspect().matches == 2, "every matching receiver must be counted");
    }
    {
        Memory m; m.root(node); m.receiver(node, initial_event_hash); m.put(node + 8, std::uintptr_t{0});
        const auto r = m.inspect();
        check(r.coherent && r.matches == 1 && r.invalid_observers == 1 && r.receivers[0].vtable == 0, "invalid receiver is present but unusable");
    }
    {
        Memory m; m.root(node); m.receiver(node, initial_event_hash, node);
        const auto r = m.inspect();
        check(!r.coherent && std::string_view{r.status} == "cycle", "cycle must never become complete absence");
    }
    {
        Memory m; m.root(node);
        for (std::uintptr_t i = 0; i < 65; ++i) m.receiver(node + i * 0x20, 7, i == 64 ? 0 : node + (i + 1) * 0x20);
        const auto r = m.inspect();
        check(!r.coherent && r.visited == 64 && std::string_view{r.status} == "limit", "walk must stop at 64 nodes");
    }
    {
        Memory m; m.root(node); m.receiver(node, initial_event_hash); m.change_node = true;
        const auto r = m.inspect();
        check(!r.coherent && std::string_view{r.status} == "changed", "node mutation invalidates snapshot");
    }
    {
        Memory m; m.root(node); m.receiver(node, initial_event_hash); m.change_head = true;
        check(!m.inspect().coherent, "bucket root mutation invalidates snapshot");
    }
    {
        Memory m; m.root(node);
        check(!m.inspect().coherent, "unreadable node must not imply zero receivers");
        m.put(queue + 0x18c0, std::uint32_t{0}); check(!m.inspect().coherent, "zero bucket count is invalid");
        m.put(queue + 0x18c0, std::uint32_t{65537}); check(!m.inspect().coherent, "oversized bucket table is invalid");
        m.put(queue + 0x18c0, std::uint32_t{3}); m.put(queue + 0x18b8, highest_address - 7);
        check(!m.inspect().coherent, "table address overflow must be rejected");
    }
    std::cout << "BMX passive ECS diagnostics checks passed\n";
}
