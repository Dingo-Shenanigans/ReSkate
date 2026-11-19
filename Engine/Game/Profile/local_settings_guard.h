#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dingosdk::local_settings_guard {
// These are copied observations, not native ownership or completion proofs.
// A caller keeps query and primary-user witnesses independent, then resamples
// both at its own dependent boundary. None of these guards invokes native code.
template<class Layout> inline bool span(std::uintptr_t p, std::size_t bytes) noexcept {
    return p >= Layout::lowest_address && bytes && bytes <= Layout::highest_address &&
        p <= Layout::highest_address - bytes;
}

struct ProfileQuery {
    const char* status{"unavailable"};
    std::uintptr_t manager{}, begin{}, end{}, object{}, vtable{}, function{};
    bool coherent{};
    bool operator==(const ProfileQuery&) const = default;
};
template<class Layout, class Read> ProfileQuery inspect_profile_query(std::uintptr_t base, const Layout&, Read&& read) noexcept {
    ProfileQuery out;
    try {
        if (base % Layout::pointer_alignment || !span<Layout>(base, Layout::query_manager_rva + Layout::pointer_size)) return out;
        const auto global = base + Layout::query_manager_rva;
        const auto get = [&read]<class T>(std::uintptr_t p, T& v) {
            return span<Layout>(p, sizeof(T)) && read(p, &v, sizeof(T));
        };
        if (global % Layout::pointer_alignment || !get(global, out.manager)) return out;
        if (out.manager) {
            if (out.manager % Layout::pointer_alignment || !span<Layout>(out.manager, Layout::query_end + Layout::pointer_size) ||
                !get(out.manager + Layout::query_begin, out.begin) || !get(out.manager + Layout::query_end, out.end) ||
                out.begin % Layout::pointer_alignment || out.end % Layout::pointer_alignment || out.end < out.begin || out.end - out.begin > Layout::query_slots * Layout::pointer_size ||
                (out.begin && !span<Layout>(out.begin, out.begin == out.end ? 1 : out.end - out.begin))) return out;
            if (out.begin != out.end) {
                if (!get(out.begin, out.object)) return out;
                if (out.object && (out.object % Layout::pointer_alignment || !get(out.object, out.vtable) ||
                    out.vtable % Layout::pointer_alignment || !span<Layout>(out.vtable, Layout::query_method_slot + Layout::pointer_size) || !get(out.vtable + Layout::query_method_slot, out.function) ||
                    !span<Layout>(out.function, 1))) return out;
            }
        }
        std::uintptr_t manager_again{}, begin_again{}, end_again{}, object_again{}, vtable_again{}, function_again{};
        if (!get(global, manager_again) || manager_again != out.manager ||
            (out.manager && (!get(out.manager + Layout::query_begin, begin_again) || begin_again != out.begin ||
                !get(out.manager + Layout::query_end, end_again) || end_again != out.end)) ||
            (out.begin != out.end && (!get(out.begin, object_again) || object_again != out.object)) ||
            (out.object && (!get(out.object, vtable_again) || vtable_again != out.vtable ||
                !get(out.vtable + Layout::query_method_slot, function_again) || function_again != out.function)) ||
            (out.object && (!get(out.object, vtable_again) || vtable_again != out.vtable)) ||
            (out.begin != out.end && (!get(out.begin, object_again) || object_again != out.object)) ||
            (out.manager && (!get(out.manager + Layout::query_begin, begin_again) || begin_again != out.begin ||
                !get(out.manager + Layout::query_end, end_again) || end_again != out.end)) ||
            !get(global, manager_again) || manager_again != out.manager) {
            out.status = "changed"; return out;
        }
        out.status = "complete"; out.coherent = true;
    } catch (...) { out.status = "reader_exception"; }
    return out;
}
struct NativeProfileQuery {
    const char* status{"unavailable"};
    bool coherent{}, storage_present{};
    std::uint8_t completed{}, deferred_load{}, deferred_save{};
    std::uint32_t list_request{}, read_request{}, load_state{}, save_state{};
};
template<class Layout, class Read> NativeProfileQuery inspect_native_profile_query(std::uintptr_t image_base,
    const ProfileQuery& query, const Layout&, Read&& read) noexcept {
    NativeProfileQuery out;
    try {
        if (!query.coherent || !query.object || image_base<Layout::lowest_address ||
            !span<Layout>(image_base, Layout::query_manager_rva + Layout::pointer_size) || query.vtable!=image_base+Layout::query_vtable_rva ||
            query.function!=image_base+Layout::query_method_rva || !span<Layout>(query.object,Layout::query_size)) return out;
        std::array<std::uint8_t,Layout::query_method_prefix.size()> actual_prefix{};
        std::array<std::uint8_t,Layout::query_size> first{},second{};
        if (!read(query.function,actual_prefix.data(),actual_prefix.size()) || actual_prefix!=Layout::query_method_prefix ||
            !read(query.object,first.data(),first.size()) || !read(query.object,second.data(),second.size()) ||
            first!=second || first[Layout::query_completed]>1 || first[Layout::query_deferred_load]>1 || first[Layout::query_deferred_save]>1) return out;
        const auto scalar=[&first]<class T>(std::size_t offset) noexcept {
            T result{};std::memcpy(&result,first.data()+offset,sizeof(result));return result;
        };
        if (scalar.template operator()<std::uintptr_t>(0)!=query.vtable) return out;
        const auto again=inspect_profile_query(image_base,Layout{},read);
        if (!again.coherent && std::strcmp(again.status,"reader_exception")==0) {
            out.status="reader_exception";return out;
        }
        if (!again.coherent || again.manager!=query.manager || again.begin!=query.begin || again.end!=query.end ||
            again.object!=query.object || again.vtable!=query.vtable || again.function!=query.function) {
            out.status="changed";return out;
        }
        out.storage_present=scalar.template operator()<std::uintptr_t>(Layout::query_storage)!=0;
        out.completed=first[Layout::query_completed];out.deferred_load=first[Layout::query_deferred_load];out.deferred_save=first[Layout::query_deferred_save];
        out.list_request=scalar.template operator()<std::uint32_t>(Layout::query_list_request);
        out.read_request=scalar.template operator()<std::uint32_t>(Layout::query_read_request);
        out.load_state=scalar.template operator()<std::uint32_t>(Layout::query_load_state);
        out.save_state=scalar.template operator()<std::uint32_t>(Layout::query_save_state);
        out.status="complete";out.coherent=true;
    } catch (...) { out.status="reader_exception"; }
    return out;
}

struct PrimaryUser {
    const char* status{"unavailable"};
    bool coherent{}, eligible{};
    bool operator==(const PrimaryUser&) const = default;
    std::uintptr_t manager{}, service{}, user{}, provider{}, global_provider{};
    std::uint32_t visited_nodes{}, record_count{}, native_index{};
    std::uint8_t guest{};
};
// Independent query and user witnesses must still agree at the dependent call.
// Agreement does not join their ownership or retain either native object.
inline bool observations_qualify(std::uintptr_t requested_query, const ProfileQuery& opening,
    const NativeProfileQuery& typed, const PrimaryUser& user, const ProfileQuery& closing,
    const PrimaryUser& user_again) noexcept {
    return requested_query && opening.coherent && opening.object == requested_query && typed.coherent &&
        user.coherent && user.eligible && closing.coherent && closing == opening &&
        user_again.coherent && user_again == user;
}
namespace detail {
template<class T, std::size_t N> T field(const std::array<std::uint8_t, N>& bytes, std::size_t offset) noexcept {
    T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
template<class Layout> struct User {
    std::uintptr_t vtable{}, method{}, provider{};
    std::uint32_t index{};
    std::uint8_t guest{};
    std::array<std::uint8_t, Layout::user_method_prefix.size()> code{};
    bool operator==(const User&) const = default;
};
template<class Layout> struct Node { std::uintptr_t address{}; std::array<std::uint8_t, Layout::tree_node_size> bytes{}; };
}
// Eligibility is limited to the primary guest without either native provider.
// It is not authentication, storage completion, spawn or gameplay readiness.
// The service lookup and first nonremoved primary record follow the native
// selection; bounded rereads establish agreement without holding a native lock.
template<class Layout, class Read> PrimaryUser inspect_primary_user(std::uintptr_t base, const Layout&, Read&& read) noexcept {
    PrimaryUser out;
    try {
        const auto raw = [&read](std::uintptr_t p, void* bytes, std::size_t size) {
            return span<Layout>(p, size) && read(p, bytes, size);
        };
        const auto get = [&raw]<class T>(std::uintptr_t p, T& value) {
            return raw(p, &value, sizeof(value));
        };
        const auto pointer = [](std::uintptr_t p) { return !(p % Layout::pointer_alignment) && span<Layout>(p, 1); };
        if (base % Layout::pointer_alignment || !span<Layout>(base, Layout::provider_rva + Layout::pointer_size) ||
            !get(base + Layout::manager_rva, out.manager) || !get(base + Layout::provider_rva, out.global_provider)) return out;
        std::uintptr_t head{}, key{}, candidate{}, service_table{}, service_method{}, begin{}, end{};
        std::array<detail::Node<Layout>, Layout::maximum_nodes> nodes{};
        std::array<std::uint8_t, Layout::maximum_records * Layout::record_stride> records{};
        detail::User<Layout> user;
        const auto read_user = [&](detail::User<Layout>& value) {
            std::uintptr_t table_again{}, method_again{};
            return pointer(out.user) && span<Layout>(out.user, Layout::user_size) && get(out.user, value.vtable) &&
                value.vtable == base + Layout::user_vtable_rva && get(value.vtable + Layout::user_method_slot, value.method) &&
                value.method == base + Layout::user_method_rva && raw(value.method, value.code.data(), value.code.size()) &&
                value.code == Layout::user_method_prefix && get(out.user + Layout::user_index, value.index) &&
                get(out.user + Layout::user_provider, value.provider) && get(out.user + Layout::user_guest, value.guest) && value.guest <= 1 &&
                get(out.user, table_again) && table_again == value.vtable &&
                get(value.vtable + Layout::user_method_slot, method_again) && method_again == value.method;
        };
        if (out.manager) {
            if (!pointer(out.manager) || !span<Layout>(out.manager, Layout::manager_size) ||
                !get(out.manager + Layout::manager_head, head) || !get(out.manager + Layout::manager_key, key)) return out;
            candidate = out.manager + Layout::manager_sentinel;
            auto node = head;
            while (node) {
                for (std::uint32_t i = 0; i < out.visited_nodes; ++i)
                    if (nodes[i].address == node) { out.status = "cycle"; return out; }
                if (out.visited_nodes == nodes.size()) { out.status = "node_limit"; return out; }
                auto& saved = nodes[out.visited_nodes]; saved.address = node;
                if (!pointer(node) || !raw(node, saved.bytes.data(), saved.bytes.size())) return out;
                ++out.visited_nodes;
                if (detail::field<std::uintptr_t>(saved.bytes, Layout::tree_key) >= key) {
                    candidate = node; node = detail::field<std::uintptr_t>(saved.bytes, Layout::tree_left);
                } else node = detail::field<std::uintptr_t>(saved.bytes, Layout::tree_right);
            }
            if (candidate != out.manager + Layout::manager_sentinel) {
                for (std::uint32_t i = 0; i < out.visited_nodes; ++i)
                    if (nodes[i].address == candidate && detail::field<std::uintptr_t>(nodes[i].bytes, Layout::tree_key) == key)
                        out.service = detail::field<std::uintptr_t>(nodes[i].bytes, Layout::tree_service);
            }
        }
        if (out.service) {
            if (!pointer(out.service) || !span<Layout>(out.service, Layout::service_size) || !get(out.service, service_table) ||
                service_table != base + Layout::service_vtable_rva || !get(service_table + Layout::service_method_slot, service_method) ||
                service_method != base + Layout::service_method_rva || !get(out.service + Layout::records_begin, begin) ||
                !get(out.service + Layout::records_end, end) || begin % Layout::pointer_alignment || end % Layout::pointer_alignment || end < begin || (end - begin) % Layout::record_stride ||
                (begin && !span<Layout>(begin, begin == end ? 1 : end - begin))) return out;
            if ((end - begin) / Layout::record_stride > Layout::maximum_records) { out.status = "record_limit"; return out; }
            out.record_count = static_cast<std::uint32_t>((end - begin) / Layout::record_stride);
            const auto size = std::size_t{out.record_count} * Layout::record_stride;
            if (size && !raw(begin, records.data(), size)) return out;
            for (std::uint32_t i = 0; i < out.record_count; ++i) {
                const auto offset = std::size_t{i} * Layout::record_stride;
                const auto user_pointer = detail::field<std::uintptr_t>(records, offset + Layout::record_user);
                if (!records[offset + Layout::record_removed] && !detail::field<std::uint32_t>(records, offset + Layout::record_index) && user_pointer) {
                    out.user = user_pointer; break;
                }
            }
            if (out.user && !read_user(user)) return out;
        }
        const auto changed = [&]() { out.status = "changed"; return out; };
        // Validate dependent payloads before rerooting. A second user read
        // after record/node rereads catches replacement during those reads.
        detail::User<Layout> current_user;
        if (out.user && (!read_user(current_user) || current_user != user)) return changed();
        if (out.record_count) {
            std::array<std::uint8_t, Layout::maximum_records * Layout::record_stride> again{};
            if (!raw(begin, again.data(), std::size_t{out.record_count} * Layout::record_stride) || again != records) return changed();
        }
        for (std::uint32_t i = 0; i < out.visited_nodes; ++i) {
            std::array<std::uint8_t, Layout::tree_node_size> again{};
            if (!raw(nodes[i].address, again.data(), again.size()) || again != nodes[i].bytes) return changed();
        }
        if (out.user && (!read_user(current_user) || current_user != user)) return changed();
        std::uintptr_t value{};
        if (out.service && (!get(out.service, value) || value != service_table ||
            !get(service_table + Layout::service_method_slot, value) || value != service_method ||
            !get(out.service + Layout::records_begin, value) || value != begin || !get(out.service + Layout::records_end, value) || value != end))
            return changed();
        if (out.manager && (!get(out.manager + Layout::manager_head, value) || value != head ||
            !get(out.manager + Layout::manager_key, value) || value != key)) return changed();
        if (!get(base + Layout::provider_rva, value) || value != out.global_provider ||
            !get(base + Layout::manager_rva, value) || value != out.manager) return changed();
        out.native_index = user.index; out.guest = user.guest; out.provider = user.provider;
        out.coherent = true; out.status = "complete";
        out.eligible = out.user && !out.native_index && out.guest == 1 && !out.provider && !out.global_provider;
    } catch (...) { out.status = "reader_exception"; }
    return out;
}
template<class Layout> struct StoragePath {
    const char* status{"unavailable"};
    bool coherent{};
    std::uintptr_t path_pointer{};
    std::array<char, Layout::maximum_path_bytes + 1> text{};
    std::size_t length{}; // Byte length, excluding NUL; never a UTF-16 character count.
};
// Read only the bounded narrow payload. Conversion, absolute-path validation
// and containment belong to the caller before it passes a path to native I/O.
template<class Layout, class Read> StoragePath<Layout> inspect_storage_path(std::uintptr_t base, std::uintptr_t platform,
    std::uintptr_t configuration, const Layout&, Read&& read) noexcept {
    StoragePath<Layout> out;
    try {
        const auto raw = [&read](std::uintptr_t address, void* bytes, std::size_t size) {
            return span<Layout>(address, size) && read(address, bytes, size);
        };
        const auto get = [&raw]<class T>(std::uintptr_t address, T& value) {
            return raw(address, &value, sizeof(value));
        };
        if (base % Layout::pointer_alignment || !span<Layout>(base, Layout::pc_storage_vtable_rva + Layout::pointer_size) || platform % Layout::pointer_alignment || configuration % Layout::pointer_alignment ||
            !span<Layout>(platform, Layout::pointer_size) || !span<Layout>(configuration, Layout::configuration_size)) return out;
        std::uintptr_t table{};
        if (!get(platform, table) || table != base + Layout::pc_storage_vtable_rva ||
            !get(configuration + Layout::configuration_path, out.path_pointer)) { out.status = "unsupported_configuration"; return out; }
        if (!span<Layout>(out.path_pointer, 1)) { out.status = "invalid_path_pointer"; return out; }
        for (; out.length < out.text.size(); ++out.length) {
            if (!get(out.path_pointer + out.length, out.text[out.length])) {
                out.status = "unreadable_path"; return out;
            }
            if (!out.text[out.length]) break;
        }
        if (!out.length || out.length == out.text.size()) { out.status = "invalid_path_length"; return out; }
        const auto same_identity = [&]() {
            std::uintptr_t current_table{}, current_path{};
            return get(platform, current_table) && current_table == table &&
                get(configuration + Layout::configuration_path, current_path) && current_path == out.path_pointer;
        };
        const auto same_content = [&]() {
            std::array<char, Layout::maximum_path_bytes + 1> again{};
            return raw(out.path_pointer, again.data(), out.length + 1) && again == out.text;
        };
        // Reroot after each complete content read so pointer/type replacement
        // during a read cannot make a stale native buffer look current.
        if (!same_content() || !same_identity() || !same_content() || !same_identity()) {
            out.status = "changed"; return out;
        }
        out.coherent = true; out.status = "complete";
    } catch (...) { out.status = "reader_exception"; }
    return out;
}
} // namespace dingosdk::local_settings_guard
