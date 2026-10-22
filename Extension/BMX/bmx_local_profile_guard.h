#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dingosdk::bmx::local_profile_guard {
inline constexpr std::uintptr_t highest_address = 0x00007fffffffffffULL;
inline constexpr std::uintptr_t manager_rva = 0x73c7a98, provider_rva = 0x74cbc50;
inline constexpr std::uintptr_t service_vtable_rva = 0x60e9950, service_method_rva = 0x18ac8a0;
inline constexpr std::uintptr_t user_vtable_rva = 0x634f110, user_method_rva = 0x272c9b0;
inline constexpr std::array<std::uint8_t, 8> user_method_bytes{0x0f,0xb6,0x81,0xb8,0x01,0x00,0x00,0xc3};
inline bool span(std::uintptr_t p, std::size_t size) noexcept {
    return p >= 0x10000 && size && size <= highest_address && p <= highest_address - size;
}
struct PrimaryUser {
    const char* status{"unavailable"};
    bool coherent{}, eligible{};
    std::uintptr_t manager{}, service{}, user{}, provider{}, global_provider{};
    std::uint32_t visited_nodes{}, record_count{}, native_index{};
    std::uint8_t guest{};
};
namespace detail {
template<class T, std::size_t N> T field(const std::array<std::uint8_t, N>& bytes, std::size_t offset) noexcept {
    T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
struct User {
    std::uintptr_t vtable{}, method{}, provider{};
    std::uint32_t index{};
    std::uint8_t guest{};
    std::array<std::uint8_t, 8> code{};
    bool operator==(const User&) const = default;
};
struct Node { std::uintptr_t address{}; std::array<std::uint8_t, 0x30> bytes{}; };
}
// Mirrors the native service lower_bound and 18ac790's first index-0,
// nonremoved, nonnull user selection. 272c9b0 reads Guest at +1b8; the primary
// constructor 272b6a0 also sets it when native provider construction is absent.
// Eligibility describes only that exact unsupported-provider local-settings
// case. It is not login, storage, spawn or gameplay readiness.
// No native functions, reference-count operations or writes occur. Repeated
// reads establish agreement, not a native lock or ownership: callers must use
// this only at their validated boundary and recheck before a dependent action.
template<class Read> PrimaryUser inspect_primary_user(std::uintptr_t base, Read&& read) noexcept {
    PrimaryUser out;
    try {
        const auto raw = [&read](std::uintptr_t p, void* bytes, std::size_t size) {
            return span(p, size) && read(p, bytes, size);
        };
        const auto get = [&raw]<class T>(std::uintptr_t p, T& value) {
            return raw(p, &value, sizeof(value));
        };
        const auto pointer = [](std::uintptr_t p) { return !(p % 8) && span(p, 1); };
        if (base % 8 || !span(base, provider_rva + 8) ||
            !get(base + manager_rva, out.manager) || !get(base + provider_rva, out.global_provider)) return out;
        std::uintptr_t head{}, key{}, candidate{}, service_table{}, service_method{}, begin{}, end{};
        std::array<detail::Node, 64> nodes{};
        std::array<std::uint8_t, 64 * 0x28> records{};
        detail::User user;
        const auto read_user = [&](detail::User& value) {
            std::uintptr_t table_again{}, method_again{};
            return pointer(out.user) && span(out.user, 0x1b9) && get(out.user, value.vtable) &&
                value.vtable == base + user_vtable_rva && get(value.vtable + 0x30, value.method) &&
                value.method == base + user_method_rva && raw(value.method, value.code.data(), value.code.size()) &&
                value.code == user_method_bytes && get(out.user + 0x140, value.index) &&
                get(out.user + 0x148, value.provider) && get(out.user + 0x1b8, value.guest) && value.guest <= 1 &&
                get(out.user, table_again) && table_again == value.vtable &&
                get(value.vtable + 0x30, method_again) && method_again == value.method;
        };
        if (out.manager) {
            if (!pointer(out.manager) || !span(out.manager, 0x40) ||
                !get(out.manager + 0x18, head) || !get(out.manager + 0x38, key)) return out;
            candidate = out.manager + 8;
            auto node = head;
            while (node) {
                for (std::uint32_t i = 0; i < out.visited_nodes; ++i)
                    if (nodes[i].address == node) { out.status = "cycle"; return out; }
                if (out.visited_nodes == nodes.size()) { out.status = "node_limit"; return out; }
                auto& saved = nodes[out.visited_nodes]; saved.address = node;
                if (!pointer(node) || !raw(node, saved.bytes.data(), saved.bytes.size())) return out;
                ++out.visited_nodes;
                if (detail::field<std::uintptr_t>(saved.bytes, 0x20) >= key) {
                    candidate = node; node = detail::field<std::uintptr_t>(saved.bytes, 8);
                } else node = detail::field<std::uintptr_t>(saved.bytes, 0);
            }
            if (candidate != out.manager + 8) {
                for (std::uint32_t i = 0; i < out.visited_nodes; ++i)
                    if (nodes[i].address == candidate && detail::field<std::uintptr_t>(nodes[i].bytes, 0x20) == key)
                        out.service = detail::field<std::uintptr_t>(nodes[i].bytes, 0x28);
            }
        }
        if (out.service) {
            if (!pointer(out.service) || !span(out.service, 0x250) || !get(out.service, service_table) ||
                service_table != base + service_vtable_rva || !get(service_table + 0x28, service_method) ||
                service_method != base + service_method_rva || !get(out.service + 0x240, begin) ||
                !get(out.service + 0x248, end) || begin % 8 || end % 8 || end < begin || (end - begin) % 0x28 ||
                (begin && !span(begin, begin == end ? 1 : end - begin))) return out;
            if ((end - begin) / 0x28 > 64) { out.status = "record_limit"; return out; }
            out.record_count = static_cast<std::uint32_t>((end - begin) / 0x28);
            const auto size = std::size_t{out.record_count} * 0x28;
            if (size && !raw(begin, records.data(), size)) return out;
            for (std::uint32_t i = 0; i < out.record_count; ++i) {
                const auto offset = std::size_t{i} * 0x28;
                const auto user_pointer = detail::field<std::uintptr_t>(records, offset + 8);
                if (!records[offset + 0x20] && !detail::field<std::uint32_t>(records, offset + 0x1c) && user_pointer) {
                    out.user = user_pointer; break;
                }
            }
            if (out.user && !read_user(user)) return out;
        }
        const auto changed = [&]() { out.status = "changed"; return out; };
        // Validate dependent payloads before rerooting. A second user read
        // after record/node rereads catches replacement during those reads.
        detail::User current_user;
        if (out.user && (!read_user(current_user) || current_user != user)) return changed();
        if (out.record_count) {
            std::array<std::uint8_t, 64 * 0x28> again{};
            if (!raw(begin, again.data(), std::size_t{out.record_count} * 0x28) || again != records) return changed();
        }
        for (std::uint32_t i = 0; i < out.visited_nodes; ++i) {
            std::array<std::uint8_t, 0x30> again{};
            if (!raw(nodes[i].address, again.data(), again.size()) || again != nodes[i].bytes) return changed();
        }
        if (out.user && (!read_user(current_user) || current_user != user)) return changed();
        std::uintptr_t value{};
        if (out.service && (!get(out.service, value) || value != service_table ||
            !get(service_table + 0x28, value) || value != service_method ||
            !get(out.service + 0x240, value) || value != begin || !get(out.service + 0x248, value) || value != end))
            return changed();
        if (out.manager && (!get(out.manager + 0x18, value) || value != head ||
            !get(out.manager + 0x38, value) || value != key)) return changed();
        if (!get(base + provider_rva, value) || value != out.global_provider ||
            !get(base + manager_rva, value) || value != out.manager) return changed();
        out.native_index = user.index; out.guest = user.guest; out.provider = user.provider;
        out.coherent = true; out.status = "complete";
        out.eligible = out.user && !out.native_index && out.guest == 1 && !out.provider && !out.global_provider;
    } catch (...) { out.status = "reader_exception"; }
    return out;
}
}
