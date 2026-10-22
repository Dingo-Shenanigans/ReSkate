#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dingosdk::bmx::ecs_detail {
inline constexpr std::uint32_t initial_event_hash = 3835844145U;
inline constexpr std::uintptr_t highest_address = 0x00007fffffffffffULL;
inline bool span(std::uintptr_t p, std::size_t bytes) noexcept {
    return p >= 0x10000 && bytes && bytes <= highest_address && p <= highest_address - bytes;
}
struct Capabilities {
    const char* status{"unavailable"};
    std::uint32_t count{};
    bool coherent{}, data_model{}, schematics{};
};
struct ProfileQuery {
    const char* status{"unavailable"};
    std::uintptr_t manager{}, begin{}, end{}, object{}, vtable{}, function{};
    bool coherent{};
};
// Native 8544b0 -> 5c67f0(manager, 0) reads the first asynchronous
// profile-settings query and invokes its vtable+8. Observe its identity only;
// an object or a function address does not establish that the query succeeded.
template<class Read> ProfileQuery inspect_profile_query(std::uintptr_t global, Read&& read) noexcept {
    ProfileQuery out;
    const auto get = [&read]<class T>(std::uintptr_t p, T& v) noexcept {
        return span(p, sizeof(T)) && read(p, &v, sizeof(T));
    };
    if (global % 8 || !get(global, out.manager)) return out;
    if (out.manager) {
        if (out.manager % 8 || !span(out.manager, 0x50) ||
            !get(out.manager + 0x40, out.begin) || !get(out.manager + 0x48, out.end) ||
            out.begin % 8 || out.end % 8 || out.end < out.begin || out.end - out.begin > 3 * 8 ||
            (out.begin && !span(out.begin, out.begin == out.end ? 1 : out.end - out.begin))) return out;
        if (out.begin != out.end) {
            if (!get(out.begin, out.object)) return out;
            if (out.object && (out.object % 8 || !get(out.object, out.vtable) ||
                out.vtable % 8 || !span(out.vtable, 16) || !get(out.vtable + 8, out.function) ||
                !span(out.function, 1))) return out;
        }
    }
    std::uintptr_t manager_again{}, begin_again{}, end_again{}, object_again{}, vtable_again{}, function_again{};
    if (!get(global, manager_again) || manager_again != out.manager ||
        (out.manager && (!get(out.manager + 0x40, begin_again) || begin_again != out.begin ||
            !get(out.manager + 0x48, end_again) || end_again != out.end)) ||
        (out.begin != out.end && (!get(out.begin, object_again) || object_again != out.object)) ||
        (out.object && (!get(out.object, vtable_again) || vtable_again != out.vtable ||
            !get(out.vtable + 8, function_again) || function_again != out.function)) ||
        (out.object && (!get(out.object, vtable_again) || vtable_again != out.vtable)) ||
        (out.begin != out.end && (!get(out.begin, object_again) || object_again != out.object)) ||
        (out.manager && (!get(out.manager + 0x40, begin_again) || begin_again != out.begin ||
            !get(out.manager + 0x48, end_again) || end_again != out.end)) ||
        !get(global, manager_again) || manager_again != out.manager) {
        out.status = "changed"; return out;
    }
    out.status = "complete"; out.coherent = true;
    return out;
}
struct NativeProfileQuery {
    const char* status{"unavailable"};
    bool coherent{}, storage_present{};
    std::uint8_t completed{}, deferred_load{}, deferred_save{};
    std::uint32_t list_request{}, read_request{}, load_state{}, save_state{};
};
// Only this native query type has a proven completion byte at +70. Native
// 204a50 reads it; successful defaults and handled errors can both complete it.
// Observe the request state without invoking its update or completion methods.
template<class Read> NativeProfileQuery inspect_native_profile_query(std::uintptr_t image_base,
    const ProfileQuery& query, Read&& read) noexcept {
    NativeProfileQuery out;
    constexpr std::uintptr_t query_vtable_rva=0x608fc38, query_method_rva=0x204a50;
    constexpr std::array<std::uint8_t,5> prefix{0x0f,0xb6,0x41,0x70,0xc3};
    if (!query.coherent || !query.object || image_base<0x10000 ||
        image_base>highest_address-0x71eff08 || query.vtable!=image_base+query_vtable_rva ||
        query.function!=image_base+query_method_rva || !span(query.object,0x90)) return out;
    std::array<std::uint8_t,5> actual_prefix{};
    std::array<std::uint8_t,0x90> first{},second{};
    if (!read(query.function,actual_prefix.data(),actual_prefix.size()) || actual_prefix!=prefix ||
        !read(query.object,first.data(),first.size()) || !read(query.object,second.data(),second.size()) ||
        first!=second || first[0x70]>1 || first[0x84]>1 || first[0x8c]>1) return out;
    const auto scalar=[&first]<class T>(unsigned offset) noexcept {
        T result{};std::memcpy(&result,first.data()+offset,sizeof(result));return result;
    };
    if (scalar.template operator()<std::uintptr_t>(0)!=query.vtable) return out;
    const auto again=inspect_profile_query(image_base+0x71eff00,read);
    if (!again.coherent || again.manager!=query.manager || again.begin!=query.begin || again.end!=query.end ||
        again.object!=query.object || again.vtable!=query.vtable || again.function!=query.function) {
        out.status="changed";return out;
    }
    out.storage_present=scalar.template operator()<std::uintptr_t>(0x60)!=0;
    out.completed=first[0x70];out.deferred_load=first[0x84];out.deferred_save=first[0x8c];
    out.list_request=scalar.template operator()<std::uint32_t>(0x74);
    out.read_request=scalar.template operator()<std::uint32_t>(0x78);
    out.load_state=scalar.template operator()<std::uint32_t>(0x80);
    out.save_state=scalar.template operator()<std::uint32_t>(0x88);
    out.status="complete";out.coherent=true;return out;
}
// Native 18b70a0 searches uint32 hashes between these two pointers.
template<class Read> Capabilities inspect_capabilities(std::uintptr_t context, std::uint32_t offset, Read&& read) noexcept {
    Capabilities out;
    if (offset > 0x1000000 || offset % 8 || context % 8 || !span(context, std::size_t{offset} + 16)) return out;
    std::uintptr_t begin{}, end{}, begin_again{}, end_again{};
    const auto get = [&read]<class T>(std::uintptr_t p, T& v) noexcept {
        return span(p, sizeof(T)) && read(p, &v, sizeof(T));
    };
    if (!get(context + offset, begin) || !get(context + offset + 8, end) ||
        begin % 4 || end % 4 || end < begin || (end - begin) % 4) return out;
    if (begin == end && begin && !span(begin, 1)) return out;
    if ((end - begin) / 4 > 256) { out.status = "limit"; return out; }
    out.count = static_cast<std::uint32_t>((end - begin) / 4);
    std::array<std::uint32_t, 256> values{}, again{};
    const auto bytes = std::size_t{out.count} * 4;
    if (bytes && (!span(begin, bytes) || !read(begin, values.data(), bytes))) return out;
    if (!get(context + offset, begin_again) || begin_again != begin ||
        !get(context + offset + 8, end_again) || end_again != end ||
        (bytes && (!read(begin, again.data(), bytes) || values != again)) ||
        !get(context + offset, begin_again) || begin_again != begin ||
        !get(context + offset + 8, end_again) || end_again != end) {
        out.status = "changed"; return out;
    }
    for (std::uint32_t i = 0; i < out.count; ++i) {
        out.data_model = out.data_model || values[i] == 0x3c6f6595;
        out.schematics = out.schematics || values[i] == 0xd209f736;
    }
    out.status = "complete"; out.coherent = true;
    return out;
}
struct Receiver { std::uintptr_t node{}, observer{}, vtable{}; };
struct Bucket {
    const char* status{"unavailable"};
    std::uintptr_t table{}, head{}, sentinel{};
    std::uint32_t count{}, visited{}, matches{}, invalid_observers{};
    std::array<Receiver, 64> receivers{};
    bool coherent{};
};
// Layout is proven at native 4b2e2b0. Read twice before reporting a complete
// observation, since the engine owns and may change this table concurrently.
template<class Read> Bucket inspect_bucket(std::uintptr_t queue, Read&& read) noexcept {
    Bucket out;
    const auto get = [&read]<class T>(std::uintptr_t p, T& v) noexcept {
        return span(p, sizeof(T)) && read(p, &v, sizeof(T));
    };
    if (!span(queue, 0x18c4) || queue % 8 || !get(queue + 0x18c0, out.count) ||
        !out.count || out.count > 65536 || !get(queue + 0x18b8, out.table) || out.table % 8 ||
        !span(out.table, (std::size_t{out.count} + 1) * 8)) return out;
    const auto slot = out.table + std::uintptr_t{initial_event_hash % out.count} * 8;
    if (!get(slot, out.head) || !get(out.table + std::uintptr_t{out.count} * 8, out.sentinel)) return out;
    struct Node { std::uintptr_t address{}, observer{}, next{}, vtable{}; std::uint32_t hash{}; };
    std::array<Node, 64> nodes{};
    auto p = out.head;
    out.status = "complete";
    while (p && p != out.sentinel) {
        for (std::uint32_t i = 0; i < out.visited; ++i)
            if (nodes[i].address == p) { out.status = "cycle"; return out; }
        if (out.visited == nodes.size()) { out.status = "limit"; return out; }
        auto& n = nodes[out.visited];
        n.address = p;
        if (p % 8 || !span(p, 0x18) || !get(p, n.hash) || !get(p + 8, n.observer) || !get(p + 0x10, n.next)) {
            out.status = "unreadable_node"; return out;
        }
        ++out.visited;
        if (n.hash == initial_event_hash) {
            const bool valid = !(n.observer % 8) && get(n.observer, n.vtable) && span(n.vtable, 0x30) && !(n.vtable % 8);
            if (!valid) ++out.invalid_observers;
            out.receivers[out.matches++] = {p, n.observer, valid ? n.vtable : 0};
        }
        p = n.next;
    }
    std::uintptr_t current_table{}, current_head{}, current_sentinel{};
    std::uint32_t count{};
    if (!get(queue + 0x18c0, count) || count != out.count || !get(queue + 0x18b8, current_table) || current_table != out.table ||
        !get(slot, current_head) || current_head != out.head ||
        !get(current_table + std::uintptr_t{count} * 8, current_sentinel) || current_sentinel != out.sentinel) {
        out.status = "changed"; return out;
    }
    for (std::uint32_t i = 0; i < out.visited; ++i) {
        const auto& n = nodes[i];
        std::uintptr_t current_observer{}, current_next{}, current_vtable{};
        std::uint32_t hash{};
        if (!get(n.address, hash) || hash != n.hash || !get(n.address + 8, current_observer) || current_observer != n.observer ||
            !get(n.address + 0x10, current_next) || current_next != n.next ||
            (n.vtable && (!get(current_observer, current_vtable) || current_vtable != n.vtable))) {
            out.status = "changed"; return out;
        }
    }
    out.coherent = true;
    return out;
}
}
