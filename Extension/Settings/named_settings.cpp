#include "named_settings.h"
#include "Engine/Core/Console/command_registry.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Settings/multiplayer_settings_lock.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Vfs/content_cache.h"
#include "Engine/Game/Build/20260929/named_settings.h"
#include "Engine/Game/Build/supported_build.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace dingosdk {
namespace {
namespace contract = addr::named_settings;
using Scalar = std::variant<bool, std::int32_t, std::uint32_t, float, std::string>;
using Getter = std::uintptr_t (*)(std::uintptr_t, const char *, std::uintptr_t *, bool);
using Setter = bool (*)(std::uintptr_t, const char *, std::uintptr_t, const void *);
#ifndef DINGOSDK_NAMED_GET
#define DINGOSDK_NAMED_GET(base) reinterpret_cast<Getter>((base) + contract::named_settings_get)
#endif
#ifndef DINGOSDK_NAMED_SET
#define DINGOSDK_NAMED_SET(base) reinterpret_cast<Setter>((base) + contract::named_settings_set)
#endif
struct Observation {
    std::uintptr_t manager{}, address{}, type{};
    Scalar value{};
    std::vector<std::int32_t> enum_values;
    bool same_field(const Observation &other) const {
        return manager == other.manager && address == other.address && type == other.type;
    }
};
struct Lease {
    Observation original, applied;
    // A player's console change, not one of ReSkate's own, and the value before it (ReSkate's,
    // when it had changed the setting first).
    bool player{};
    Observation player_from;
};
// What one observation shows for a setting. Built apart from its model row so the
// row, and the revision the runtime copies the rows by, change only when it differs.
struct View {
    bool available{}, override_active{};
    std::string type, value;
    std::string_view reason; // a literal or Runtime::failure
    std::vector<std::string> choices;
};
struct Runtime {
    std::uintptr_t base{};
    DWORD thread{};
    bool ready{}, unready_published{}, was_wanted{};
    std::size_t cursor{};
    ULONGLONG next_scan{}, next_listing{};
    // Registry sizes at the last listing; the list is rebuilt when they change.
    std::uint64_t listed{};
    std::uint64_t revision{1};
    std::set<std::string> known; // lower case
    std::string failure;
    std::vector<NamedSettingModel> models;
    std::vector<std::optional<Lease>> leases;
    // Players' overrides of settings locked in multiplayer, put back when a session starts.
    std::set<std::size_t> player_locked;
    View view; // scratch for observe(), reused so its strings keep their buffers
};
Runtime &runtime() {
    static Runtime value;
    return value;
}
// Every name seen in this or an earlier session, for the console registry.
struct Names {
    std::mutex mutex;
    std::vector<std::string> list;
};
Names &names() {
    static auto *value = new Names;
    return *value;
}
std::filesystem::path names_file() { return content_cache::directory() / L"engine-settings.txt"; }
bool in_image(std::uintptr_t at, std::size_t size) {
    const auto base = runtime().base;
    return size <= supported_build::game_image_size && at >= base &&
           at - base <= supported_build::game_image_size - size;
}
bool native_get(std::uintptr_t manager, const char *name, std::uintptr_t &address, std::uintptr_t &type) {
    __try {
        address = DINGOSDK_NAMED_GET(runtime().base)(manager, name, &type, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool native_set(const Observation &before, const char *name, const void *value, bool &accepted) {
    __try {
        // Null override type: the engine must copy using its freshly resolved
        // field type. Never allow console input to supply an address/type.
        accepted = DINGOSDK_NAMED_SET(runtime().base)(before.manager, name, 0, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void fault() {
    auto &r = runtime();
    r.ready = false;
    r.failure = "Native settings call failed; bindings disabled for this session.";
    for (auto &m : r.models) {
        m.available = false;
        m.value.clear();
        m.reason = r.failure;
    }
    ++r.revision;
}
std::string text(const Scalar &value) {
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "1" : "0";
            else if constexpr (std::is_same_v<T, float>) {
                std::array<char, 64> buffer{};
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
                return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : "unreadable";
            } else
                return std::to_string(v);
        },
        value);
}
// Engine fields and descriptors read by the settings sweep. A plain guarded copy: a
// system call per field, and per character of a string, added up on game frames.
constexpr std::size_t page_size = 0x1000;
bool read_string(std::uintptr_t address, std::string &result) {
    std::uintptr_t data{};
    if (!memory::peek(address, data) || !data)
        return false;
    result.clear();
    // Up to the end of one page at a time: the bytes after the terminator may belong
    // to a page that is not mapped.
    constexpr std::size_t limit = 4097;
    std::array<char, 256> chunk{};
    while (result.size() < limit) {
        const auto at = data + result.size();
        const auto size = std::min({chunk.size(), limit - result.size(), page_size - at % page_size});
        if (!memory::peek_bytes(at, chunk.data(), size))
            return false;
        const auto end = std::find(chunk.data(), chunk.data() + size, '\0');
        result.append(chunk.data(), end);
        if (end != chunk.data() + size)
            return true;
    }
    return false;
}
template <class T> bool read_scalar(std::uintptr_t address, Scalar &result) {
    T value{};
    if (!memory::peek(address, value))
        return false;
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value))
            return false;
    }
    result = value;
    return true;
}
std::optional<Observation> observe_into(std::size_t index, View &v) {
    auto &r = runtime();
    if (!r.ready) {
        v.reason = r.failure;
        return {};
    }
    Observation o;
    std::uintptr_t buckets{}, symbols{};
    std::uint32_t count{};
    if (!memory::peek(r.base + contract::named_settings_manager, o.manager) ||
        !memory::peek(o.manager + 0xa0, buckets) || !buckets || !memory::peek(o.manager + 0xa8, count) || !count ||
        count > 0x100000 || !memory::peek(r.base + contract::symbol_registry, symbols) || !memory::peek(symbols + 0xd0, count) ||
        !count || count > 0x100000)
        return {};
    if (!native_get(o.manager, r.models[index].name.c_str(), o.address, o.type)) {
        fault();
        v.reason = r.failure;
        return {};
    }
    if (!o.address || !o.type) {
        v.reason = "No registered native field matches this name in the current session.";
        return {};
    }
    std::uintptr_t descriptor{};
    std::uint16_t flags{}, size{};
    if (!in_image(o.type, 8) || !memory::peek(o.type, descriptor) || !in_image(descriptor, 0x38) ||
        !memory::peek(descriptor + 4, flags) || !memory::peek(descriptor + 6, size)) {
        v.reason = "The engine returned an unrecognized type descriptor.";
        return {};
    }
    const auto kind = (flags >> 5) & 31;
    v.reason = "The native field value could not be read or validated.";
    const auto type = o.type - r.base;
    bool read = false;
    if (type == contract::native_bool && kind == 10 && size == 1) {
        v.type = "Boolean";
        v.choices = {"0", "1"};
        std::uint8_t flag{};
        read = memory::peek(o.address, flag) && flag <= 1;
        o.value = flag != 0;
    } else if (type == contract::native_int32 && kind == 15 && size == 4) {
        v.type = "Int32";
        read = read_scalar<std::int32_t>(o.address, o.value);
    } else if (type == contract::native_uint32 && kind == 16 && size == 4) {
        v.type = "Uint32";
        read = read_scalar<std::uint32_t>(o.address, o.value);
    } else if (type == contract::native_float32 && kind == 19 && size == 4) {
        v.type = "Float32";
        read = read_scalar<float>(o.address, o.value);
    } else if (type == contract::native_cstring && kind == 7 && size == 8) {
        v.type = "CString";
        std::string characters;
        read = read_string(o.address, characters);
        o.value = std::move(characters);
    } else if (kind == 8 && size == 4) {
        v.type = "Enum";
        std::uint16_t n{};
        std::uintptr_t fields{};
        // Enum metadata is an explicit value list, not a guessed min/max. Some
        // enums contain sparse values such as 2051 and must retain those gaps.
        if (!memory::peek(descriptor + 0x2a, n) || !n || n > 256 || !memory::peek(descriptor + 0x30, fields) ||
            !in_image(fields, n * 24)) {
            v.reason = "Enum choices could not be validated.";
            return {};
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::int32_t choice{};
            if (!memory::peek(fields + i * 24 + 16, choice))
                return {};
            if (std::find(o.enum_values.begin(), o.enum_values.end(), choice) == o.enum_values.end()) {
                o.enum_values.push_back(choice);
                v.choices.push_back(std::to_string(choice));
            }
        }
        read = read_scalar<std::int32_t>(o.address, o.value);
    } else {
        v.type = "Native kind " + std::to_string(kind);
        v.reason = "No verified value codec for this native type.";
        return {};
    }
    if (!read)
        return {};
    v.available = true;
    v.reason = {};
    v.value = text(o.value);
    auto &lease = r.leases[index];
    // Object replacement or an external edit relinquishes ownership. A reset
    // must never overwrite a newer value supplied by the engine or game menu.
    if (lease && (!o.same_field(lease->applied) || o.value != lease->applied.value))
        lease.reset();
    v.override_active = lease.has_value();
    return o;
}
// Stores what an observation showed, counting a change only when the row differs.
void publish(std::size_t index, const View &v) {
    auto &r = runtime();
    auto &m = r.models[index];
    if (m.available == v.available && m.override_active == v.override_active && m.type == v.type &&
        m.value == v.value && std::string_view(m.reason) == v.reason && m.choices == v.choices)
        return;
    m.available = v.available;
    m.override_active = v.override_active;
    m.type = v.type;
    m.value = v.value;
    m.reason = v.reason;
    m.choices = v.choices;
    ++r.revision;
}
std::optional<Observation> observe(std::size_t index) {
    auto &r = runtime();
    auto &v = r.view;
    v.available = false;
    v.override_active = false;
    v.value.clear();
    v.type.clear();
    v.choices.clear();
    v.reason = "The native settings manager is not ready.";
    auto result = observe_into(index, v);
    publish(index, v);
    return result;
}
// A session override ended (restored, or back at its original value).
void end_override(std::size_t index) {
    auto &r = runtime();
    r.leases[index].reset();
    if (r.models[index].override_active) {
        r.models[index].override_active = false;
        ++r.revision;
    }
}
std::optional<Scalar> parse(const Observation &o, std::string_view input) {
    return std::visit(
        [&](const auto &v) -> std::optional<Scalar> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                if (input.size() > 4096 || input.find('\0') != std::string_view::npos)
                    return {};
                return std::string(input);
            } else if constexpr (std::is_same_v<T, bool>) {
                const auto result = console::parse_value(console::Type::boolean, input);
                if (result)
                    return std::get<bool>(*result);
                return {};
            } else {
                T result{};
                const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
                if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
                    return {};
                if constexpr (std::is_floating_point_v<T>) {
                    if (!std::isfinite(result))
                        return {};
                } else if constexpr (std::is_same_v<T, std::int32_t>) {
                    if (!o.enum_values.empty() &&
                        std::find(o.enum_values.begin(), o.enum_values.end(), result) == o.enum_values.end())
                        return {};
                }
                return result;
            }
        },
        o.value);
}
std::string change(std::size_t index, std::string_view input, bool restore, bool player = false) {
    auto &r = runtime();
    auto &m = r.models[index];
    const bool was_owned = r.leases[index].has_value();
    const auto current = observe(index);
    if (!current)
        return "error: " + m.name + ": " + m.reason;
    const auto old_lease = r.leases[index];
    if (restore && !old_lease)
        return m.name + (was_owned ? ": engine value changed; preserved the newer value." : ": no session override.");
    const auto value = restore ? std::optional<Scalar>(old_lease->original.value) : parse(*current, input);
    if (!value) {
        std::string reason = "error: " + m.name + " requires " + m.type;
        if (!m.choices.empty()) {
            reason += " (";
            for (const auto &c : m.choices)
                reason += c + " ";
            reason.back() = ')';
        }
        return reason + "; value rejected.";
    }
    if (*value == current->value) {
        if (restore)
            end_override(index);
        return m.name + " = [" + m.value + "] (unchanged)";
    }
    // Re-resolve immediately before the native setter; observation and mutation
    // both run on the client thread, outside the presentation/runtime mutex.
    const auto checked = observe(index);
    if (!checked || !checked->same_field(*current) || checked->value != current->value)
        return "error: " + m.name + " changed during validation; retry the command.";
    bool accepted{};
    const bool called = std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                // The native CString copy clones these bytes into engine storage.
                // Do not retain a borrowed pointer or free an engine allocation here.
                const char *ptr = v.c_str();
                return native_set(*checked, m.name.c_str(), &ptr, accepted);
            } else
                return native_set(*checked, m.name.c_str(), &v, accepted);
        },
        *value);
    if (!called) {
        fault();
        return "error: " + r.failure;
    }
    Observation applied = *checked;
    applied.value = *value;
    if (accepted) {
        const bool owned = old_lease && old_lease->player;
        r.leases[index] = Lease{old_lease ? old_lease->original : *checked, applied, player || owned,
                                owned ? old_lease->player_from : *checked};
        if (r.leases[index]->player && locked_in_multiplayer(m.name)) r.player_locked.insert(index);
    }
    const auto after = observe(index);
    if (!accepted || !after || !after->same_field(applied) || after->value != *value)
        return "error: " + m.name + ": the engine did not retain the requested value.";
    if (restore || (r.leases[index] && after->value == r.leases[index]->original.value))
        end_override(index);
    return m.name + " = [" + m.value + "]" + (restore ? " (restored)" : "");
}

// Setting names from the engine's own registry: each registered group's class
// fields (inherited ones first, value-type fields as "Group.Struct.Field").
// Reads only, under the manager's lock, on the game update thread. Several thousand
// names: every read is a guarded copy, and a name is copied a page fragment at a time
// rather than with a system call per character.
bool read_name(std::uintptr_t address, std::string &result) {
    result.clear();
    std::array<char, 128> chunk{};
    for (std::size_t done = 0; done < chunk.size();) {
        const auto at = address + done;
        const auto size = std::min<std::size_t>(chunk.size() - done, page_size - at % page_size);
        if (!memory::peek_bytes(at, chunk.data() + done, size)) return false;
        for (auto i = done; i < done + size; ++i) {
            const char c = chunk[i];
            if (!c) return !result.empty();
            if (c < 0x21 || c > 0x7e) return false;
            result += c;
        }
        done += size;
    }
    return false;
}
class Lister {
  public:
    Lister(std::uintptr_t base, std::uintptr_t registry) : base_(base) {
        memory::peek(registry + contract::name_buckets, buckets_);
        memory::peek(registry + contract::name_bucket_count, bucket_count_);
    }
    // The registry name of a field-data record or type descriptor.
    std::optional<std::string> name_of(std::uintptr_t key) const {
        if (!buckets_ || !bucket_count_ || bucket_count_ > 0x1000000) return {};
        std::uintptr_t node{};
        if (!memory::peek(buckets_ + (key % bucket_count_) * 8, node)) return {};
        for (unsigned hops = 0; node && hops < 4096; ++hops) {
            std::uintptr_t node_key{}, name{};
            if (!memory::peek(node, node_key)) return {};
            if (node_key == key) {
                std::string result;
                if (memory::peek(node + 8, name) && read_name(name, result)) return result;
                return {};
            }
            if (!memory::peek(node + 0x10, node)) return {};
        }
        return {};
    }
    void fields(std::uintptr_t type, const std::string &prefix, std::vector<std::string> &out, unsigned depth = 0) const {
        std::uintptr_t descriptor{};
        std::uint16_t flags{}, count{};
        if (depth > 12 || out.size() > 16384 || !in_image(type, 8) || !memory::peek(type, descriptor) ||
            !in_image(descriptor, 0x68) || !memory::peek(descriptor + 4, flags) ||
            !memory::peek(descriptor + contract::descriptor_field_count, count) || count > 1024)
            return;
        const auto kind = (flags >> 5) & 31;
        std::uintptr_t array{};
        if (kind == 3) {
            std::uintptr_t super{};
            if (memory::peek(type + contract::type_super, super) && super && super != type &&
                super != base_ + contract::system_settings_type)
                fields(super, prefix, out, depth + 1);
            memory::peek(descriptor + contract::class_fields, array);
        } else if (kind == 2) {
            memory::peek(descriptor + contract::value_fields, array);
        } else {
            return;
        }
        if (!array || !in_image(array, std::size_t{count} * contract::field_size)) return;
        for (std::uint16_t i = 0; i < count; ++i) {
            const auto field = array + std::size_t{i} * contract::field_size;
            const auto name = name_of(field);
            std::uintptr_t field_type{}, field_descriptor{};
            std::uint16_t field_flags{};
            if (!name || !memory::peek(field + contract::field_type, field_type) || !in_image(field_type, 8) ||
                !memory::peek(field_type, field_descriptor) || !in_image(field_descriptor, 8) ||
                !memory::peek(field_descriptor + 4, field_flags))
                continue;
            if (((field_flags >> 5) & 31) == 2) fields(field_type, prefix + *name + ".", out, depth + 1);
            else out.push_back(prefix + *name);
        }
    }
    // Every group in one of the manager's maps; `type_of` finds a node's class.
    template <class TypeOf>
    void groups(std::uintptr_t manager, std::size_t buckets_at, std::size_t count_at, std::size_t next_at,
                TypeOf type_of, std::vector<std::string> &out) const {
        std::uintptr_t buckets{};
        std::uint32_t count{};
        if (!memory::peek(manager + buckets_at, buckets) || !buckets || !memory::peek(manager + count_at, count) ||
            !count || count > 0x100000)
            return;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uintptr_t node{};
            if (!memory::peek(buckets + std::size_t{i} * 8, node)) return;
            for (unsigned hops = 0; node && hops < 4096; ++hops) {
                std::uintptr_t name{};
                std::string group;
                if (memory::peek(node, name) && read_name(name, group))
                    if (const auto type = type_of(node)) fields(type, group + ".", out);
                if (!memory::peek(node + next_at, node)) break;
            }
        }
    }

  private:
    std::uintptr_t base_{}, buckets_{};
    std::uint32_t bucket_count_{};
};
std::uintptr_t object_type(std::uintptr_t node) {
    std::uintptr_t object{}, type{};
    return memory::peek(node + 8, object) && object && memory::peek(object + 8, type) ? type : std::uintptr_t{};
}
std::uintptr_t raw_type(std::uintptr_t node) {
    std::uintptr_t type{};
    return memory::peek(node + 8, type) ? type : std::uintptr_t{};
}
void list_groups(const Lister &lister, std::uintptr_t manager, std::vector<std::string> &out) {
    const auto section = reinterpret_cast<LPCRITICAL_SECTION>(manager + contract::manager_lock);
    EnterCriticalSection(section);
    try {
        lister.groups(manager, contract::group_buckets, contract::group_bucket_count, contract::group_next,
            &object_type, out);
        lister.groups(manager, contract::raw_buckets, contract::raw_bucket_count, contract::raw_next, &raw_type, out);
    } catch (...) {
    }
    LeaveCriticalSection(section);
}
// Add names the engine registered since the last listing. Models only grow, so
// the indices the console captured stay valid.
void list_settings(Runtime &r, ULONGLONG now) {
    if (now < r.next_listing) return;
    r.next_listing = now + 2000;
    std::uintptr_t manager{}, registry{};
    std::uint32_t groups{}, raw{};
    if (!memory::peek(r.base + contract::named_settings_manager, manager) || !manager ||
        !memory::peek(manager + contract::group_count, groups) || !memory::peek(manager + contract::raw_count, raw) ||
        !memory::peek(r.base + contract::symbol_registry, registry) || !registry)
        return;
    const auto sizes = (std::uint64_t{groups} << 32) | raw;
    if (sizes == r.listed) return;
    r.listed = sizes;
    std::vector<std::string> found;
    list_groups(Lister(r.base, registry), manager, found);
    // Sorted by lower-case name, computed once per name rather than twice per comparison.
    std::vector<std::pair<std::string, std::string>> keyed;
    keyed.reserve(found.size());
    for (auto &name : found) keyed.emplace_back(console::lower(name), std::move(name));
    std::sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::string> added;
    for (auto &[key, name] : keyed)
        if (r.known.insert(std::move(key)).second) added.push_back(std::move(name));
    if (added.empty()) return;
    for (const auto &name : added) {
        NamedSettingModel model;
        model.name = name;
        model.reason = "Waiting for the game update thread.";
        r.models.push_back(std::move(model));
    }
    r.leases.resize(r.models.size());
    ++r.revision;
    std::vector<std::string> all;
    {
        auto &n = names();
        std::lock_guard lock(n.mutex);
        n.list.insert(n.list.end(), added.begin(), added.end());
        all = n.list;
    }
    // Remembered for the next session's console, which is built before the
    // engine has registered every group. A worker writes it: the game thread
    // never waits on the disk. Each list contains the previous one, so only
    // the newest is written if two are queued.
    static std::atomic<std::uint64_t> queued{};
    const auto sequence = ++queued;
    try {
        std::thread([path = names_file(), list = std::move(all), sequence] {
            static std::mutex writing;
            std::lock_guard lock(writing);
            if (sequence != queued.load()) return;
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            auto temporary = path;
            temporary += L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                for (const auto &name : list) output << name << '\n';
                if (!output) return;
            }
            std::filesystem::rename(temporary, path, error);
        }).detach();
    } catch (...) {
    }
}
} // namespace
std::vector<std::string> named_setting_names() {
    auto &n = names();
    std::lock_guard lock(n.mutex);
    return n.list;
}
bool initialize_named_settings(std::uintptr_t base) {
    auto &r = runtime();
    r = {};
    r.base = base;
    r.failure = "Native settings contract does not match the supported executable.";
    // Names found in earlier sessions; this session's are listed from the
    // engine's settings registry on the game thread (list_settings).
    {
        std::ifstream input(names_file());
        for (std::string line; std::getline(input, line);) {
            if (line.empty() || line.size() > 256 || !r.known.insert(console::lower(line)).second) continue;
            NamedSettingModel model;
            model.name = line;
            model.reason = "Waiting for the game update thread.";
            r.models.push_back(std::move(model));
        }
    }
    {
        auto &n = names();
        std::lock_guard lock(n.mutex);
        n.list.clear();
        for (const auto &model : r.models) n.list.push_back(model.name);
    }
    r.leases.resize(r.models.size());
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!memory::read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x100000 ||
        !memory::read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != supported_build::game_image_size)
        return false;
    for (const auto &site : contract::named_settings_sites) {
        std::array<unsigned char, 32> bytes{};
        if (!memory::read(base + site.rva, bytes) || bytes != site.bytes)
            return false;
    }
    r.ready = true;
    r.failure.clear();
    return true;
}
// A session started: the player's overrides of settings that change how the game plays go back.
void put_back_locked(Runtime &r) {
    for (auto it = r.player_locked.begin(); it != r.player_locked.end();) {
        const auto index = *it;
        if (index < r.leases.size() && r.leases[index] && r.leases[index]->player) {
            const auto back = text(r.leases[index]->player_from.value);
            const auto result = change(index, back, false);
            // Whatever remains is ReSkate's own change, or nothing.
            if (r.leases[index]) r.leases[index]->player = false;
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Multiplayer: {} changes how the game plays; put back for the session ({}).", r.models[index].name, result);
        }
        it = r.player_locked.erase(it);
    }
}
void refresh_named_settings(bool wanted) {
    auto &r = runtime();
    if (!r.thread)
        r.thread = GetCurrentThreadId();
    if (r.thread != GetCurrentThreadId())
        return;
    if (r.ready && !r.player_locked.empty() && multiplayer_settings_locked())
        put_back_locked(r);
    if (!r.ready) {
        // Once: nothing observes the rows again until the contract matches.
        if (!r.unready_published) {
            for (auto &m : r.models) {
                m.available = false;
                m.reason = r.failure;
            }
            ++r.revision;
            r.unready_published = true;
        }
        return;
    }
    const auto now = GetTickCount64();
    list_settings(r, now);
    // Only the menu and console show these values. While one of them does,
    // refresh 32 rows a frame and sweep again 500 ms after each pass, bounding
    // first-use reflection work instead of resolving the catalog in one frame.
    // Otherwise trickle through 32 rows every 500 ms: idle game frames stay
    // cheap and a menu opened later is never far behind.
    if (wanted && !r.was_wanted)
        r.next_scan = 0;
    r.was_wanted = wanted;
    if (now < r.next_scan)
        return;
    for (unsigned n = 0; n < 32 && r.cursor < r.models.size(); ++n)
        (void)observe(r.cursor++);
    if (r.cursor == r.models.size()) {
        r.cursor = 0;
        r.next_scan = now + 500;
    } else if (!wanted) {
        r.next_scan = now + 500;
    }
}
const std::vector<NamedSettingModel> &named_settings_model() { return runtime().models; }
std::uint64_t named_settings_revision() { return runtime().revision; }
std::string change_named_setting(std::string_view name, std::string_view value, bool restore) {
    auto &r = runtime();
    if (!r.thread || r.thread != GetCurrentThreadId())
        return "error: Native settings require the game update thread.";
    for (std::size_t i = 0; i < r.models.size(); ++i)
        if (console::equal(r.models[i].name, name))
            return change(i, value, restore);
    return "error: Unknown native setting; only catalog entries can execute.";
}
std::optional<std::string> named_setting_value(std::string_view name) {
    auto &r = runtime();
    if (!r.thread || r.thread != GetCurrentThreadId())
        return {};
    for (std::size_t i = 0; i < r.models.size(); ++i)
        if (console::equal(r.models[i].name, name)) {
            const auto current = observe(i);
            if (current)
                return text(current->value);
            return {};
        }
    return {};
}
std::string player_change_named_setting(std::string_view name, std::string_view value, bool restore) {
    auto &r = runtime();
    if (!r.thread || r.thread != GetCurrentThreadId())
        return "error: Native settings require the game update thread.";
    for (std::size_t i = 0; i < r.models.size(); ++i)
        if (console::equal(r.models[i].name, name)) {
            if (!restore && multiplayer_settings_locked() && locked_in_multiplayer(r.models[i].name))
                return "error: " + r.models[i].name +
                       " changes how the game plays, so it cannot be changed during a multiplayer session.";
            return change(i, value, restore, true);
        }
    return "error: Unknown native setting; only catalog entries can execute.";
}
std::string restore_named_settings() {
    auto &r = runtime();
    if (!r.thread || r.thread != GetCurrentThreadId())
        return "error: Native settings require the game update thread.";
    std::string result;
    for (std::size_t i = 0; i < r.leases.size(); ++i)
        if (r.leases[i]) {
            if (!result.empty())
                result += '\n';
            result += change(i, {}, true);
        }
    return result.empty() ? "No native session overrides to restore." : result;
}
} // namespace dingosdk
