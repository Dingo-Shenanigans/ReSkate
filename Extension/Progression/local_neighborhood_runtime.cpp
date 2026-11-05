#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_neighborhood_runtime.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_neighborhood.h"

namespace dingosdk::profile_runtime {
// Included in local_profile_runtime.cpp's private namespace.

NeighborhoodRuntime& neighborhood_runtime() { static auto* n = new NeighborhoodRuntime; return *n; }

bool neighborhood_type_contract(std::uintptr_t base) {
    struct Field { std::uint64_t hash, offset; std::uintptr_t type; } field{};
    std::uint32_t hash{}; std::uint16_t size{};
    return read(base + addr::local_neighborhood::neighborhood_flag_field, field) && field.hash == 0x124511ee &&
        field.offset == 0xbd && field.type == base + addr::engine::bool_type &&
        read(base + addr::local_neighborhood::neighborhood_record_hash, hash) && hash == 0xdcf37866 &&
        read(base + addr::local_neighborhood::neighborhood_record_size, size) && size == sizeof(NeighborhoodRecord) &&
        read(base + addr::local_neighborhood::neighborhood_progression_field, field) && field.hash == 0x9444f3fc &&
        field.offset == 0x38 && field.type == base + addr::engine::uint32_type;
}


bool known_neighborhood(const NeighborhoodRecord& record, std::string& id) {
    return identifier(record.bytes.data() + 0x88, id) &&
        std::find(profile::neighborhood_ids.begin(), profile::neighborhood_ids.end(), id) !=
            profile::neighborhood_ids.end();
}

std::optional<std::uint32_t> neighborhood_cap(const NeighborhoodRecord& record) {
    // The native level-definition callback publishes 0x20-byte entries: two
    // strings, rewards array, then the numeric level at +0x18. An empty native
    // array means the offline service supplied no curve: use our explicit
    // local cap. Malformed/unreadable arrays are rejected, not treated as empty.
    const auto levels = record.get<std::uintptr_t>(0x90);
    std::uint32_t count{};
    if (!read(levels - 4, count)) return {};
    count &= 0x7fffffff;
    if (!count) return local_runtime().store->offline_rank_cap();
    if (count > 10000) return {};
    std::uint32_t cap{};
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t level{};
        if (!read(levels + i * 0x20 + 0x18, level) || level > 10000 || level < cap) return {};
        cap = level;
    }
    return cap ? std::optional{cap} : std::nullopt;
}

// Read-only snapshot of each district as published, once per district. Kept
// for the unresolved box-tile lock: the level array at +0x90 is empty offline,
// and the boxes page reads the u32 fields at 0xb0/0xb4/0xb8 (writing them
// without also publishing that array crashes it), so all are reported.
void log_neighborhood_state(const std::string& id, const NeighborhoodRecord& record,
    const std::optional<std::uint32_t>& cap) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> reported;
    {
        std::lock_guard lock(mutex);
        if (!reported.insert(id).second) return;
    }
    const auto levels = record.get<std::uintptr_t>(0x90);
    std::uint32_t count{};
    const bool readable = levels && read(levels - 4, count);
    std::ostringstream event;
    event << "{\"event\":\"local_profile_neighborhood_state\",\"id\":" << std::quoted(id)
        << ",\"rank\":" << record.get<std::uint32_t>(0x74)
        << ",\"xp_rank\":" << record.get<std::uint32_t>(0x68)
        << ",\"at_max\":" << (record.get<std::uint8_t>(0x78) ? "true" : "false")
        << ",\"unlocked\":" << (record.get<std::uint8_t>(0xbd) ? "true" : "false")
        << ",\"levels\":" << (readable ? std::to_string(count & 0x7fffffff) : std::string("null"))
        << ",\"cap\":" << (cap ? std::to_string(*cap) : std::string("null"))
        << ",\"f_b0\":" << record.get<std::uint32_t>(0xb0)
        << ",\"f_b4\":" << record.get<std::uint32_t>(0xb4)
        << ",\"f_b8\":" << record.get<std::uint32_t>(0xb8) << '}';
    dingosdk::logging::event(dingosdk::logging::Channel::progression, event.str().c_str(),
        dingosdk::logging::Level::info);
}

bool apply_neighborhood_profile(NeighborhoodRecord& record) {
    auto& s = local_runtime();
    std::string id;
    if (!known_neighborhood(record, id)) return false;
    auto saved = s.store->neighborhood_state(id);
    bool changed{};
    // Native response conversion hashes ProgressionId, then a native
    // copy places that hash into the base record. The card identifies
    // districts by this field, separately from the model's context key.
    std::string progression_id;
    if (identifier(record.bytes.data() + 0x30, progression_id) && progression_id == id) {
        const auto hash = game::native_name_hash(progression_id);
        if (record.get<std::uint32_t>(0x38) != hash) {
            record.set(0x38, hash); changed = true;
        }
    }
    if (saved.unlocked && record.get<std::uint8_t>(0xbd) == 0) {
        record.set<std::uint8_t>(0xbd, 1); changed = true;
    }
    const auto cap = neighborhood_cap(record);
    if (saved.max_ranks && cap) {
        // Commit before publishing a new rank. Failure leaves the native input
        // and UI untouched; a subsequent update may retry the disk transaction.
        s.store->set_neighborhood_rank(id, *cap);
        saved.rank = *cap;
    }
    if (saved.rank) {
        const auto level = cap ? (std::min)(*cap, *saved.rank) : *saved.rank;
        const auto at_max = cap && level == *cap;
        if (record.get<std::uint32_t>(0x74) != level || record.get<std::uint32_t>(0x68) != level ||
            record.get<std::uint8_t>(0x78) != static_cast<std::uint8_t>(at_max)) {
            record.set(0x74, level); record.set(0x68, level);
            record.set<std::uint8_t>(0x78, at_max); changed = true;
        }
        log_neighborhood_state(id, record, cap);
        std::lock_guard observation_lock(neighborhood_runtime().observation_mutex);
        auto& ranks = neighborhood_runtime().logged_ranks;
        if (!ranks.contains(id) || ranks[id] != level) {
            ranks[id] = level;
            std::ostringstream event;
            event << "{\"event\":\"local_profile_neighborhood_rank\",\"id\":" << std::quoted(id)
                << ",\"level\":" << level << ",\"at_max\":" << (at_max ? "true" : "false") << '}';
            dingosdk::logging::event(dingosdk::logging::Channel::progression, event.str().c_str());
        }
    } else if (saved.max_ranks && !neighborhood_runtime().logged_pending.exchange(true)) {
        dingosdk::logging::event(dingosdk::logging::Channel::progression, "{\"event\":\"local_profile_neighborhood_rank_pending\",\"reason\":\"level_definitions_unavailable\"}");
    }
    return changed;
}

bool publish_neighborhood_hook(std::uintptr_t manager, std::uint64_t context,
    std::uintptr_t type, const void* source) {
    auto& s = local_runtime();
    NeighborhoodRecord record;
    bool replace{};
    if (s.active.load(std::memory_order_acquire) && type == s.base + addr::local_neighborhood::neighborhood_type) {
        PreserveError preserve;
        try {
            if (read_bytes(reinterpret_cast<std::uintptr_t>(source), &record, sizeof(record)))
                replace = apply_neighborhood_profile(record);
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::progression, "{\"event\":\"local_profile_neighborhood_publish_failed\"}"); }
    }
    // Native synchronous assignment copies strings, arrays and references. The
    // borrowed input is neither retained nor destroyed by this adapter. Keep
    // native exceptions and LastError behavior outside the local catch block.
    return game::native_data().models.publish(manager, context, type, replace ? &record : source);
}

void hydrate_neighborhoods(std::uintptr_t owner) {
    auto& s = local_runtime(); auto& n = neighborhood_runtime();
    std::uintptr_t current_owner{}; std::uint64_t list{};
    if (!read(s.base + addr::engine::location_owner, current_owner) || owner != current_owner ||
        !read(owner + 0x30, list) || !list) return;
    const auto model = game::native_data().models.get_model(0xbf0f9789);
    if (!model) return;
    // Value() exposes borrowed pointers. Keep the native recursive write lock
    // until the synchronous copies finish, including their nested arrays.
    game::ModelWriteLock model_lock(model);
    const auto list_value = game::native_data().models.value(model, list, 0, 0);
    std::uintptr_t elements{}; std::uint32_t count{};
    if (!read(list_value, elements) || !read(elements - 4, count)) return;
    count &= 0x7fffffff;
    if (count > 10000) return;
    struct Reference { std::uintptr_t name; std::uint64_t context; };
    struct ReferenceArray {
        std::uint32_t capacity{4}, count{4};
        std::array<Reference, 4> elements{};
    } references;
    static_assert(offsetof(ReferenceArray, elements) == 8 && sizeof(Reference) == 16);
    unsigned created{};
    for (std::size_t i = 0; i < profile::neighborhood_ids.size(); ++i) {
        const auto id = profile::neighborhood_ids[i];
        const auto type = s.base + addr::local_neighborhood::neighborhood_type;
        auto context = game::native_data().models.find(model, type, game::native_name_hash(id), 0, 0);
        NeighborhoodRecord record;
        if (!context && count == 0 && s.store->bool_option(profile::unlock_neighborhoods_option).value_or(false)) {
            n.construct(&record);
            // Default native fields contain only empty, non-owning values.
            // CString literals below are borrowed for the synchronous publish.
            references.elements[i].name = record.get<std::uintptr_t>(0x80);
            static constexpr std::array<const char*, 4> names{
                "entertainment", "financial", "historic", "stadium"};
            record.set(0x20, id.data()); record.set(0x30, id.data());
            record.set(0x80, names[i]); record.set(0x88, id.data());
            record.set<std::uint8_t>(0x3c, 1); record.set<std::uint8_t>(0xbc, 1);
            record.set<std::uint8_t>(0xbd, 1);
            context = game::native_data().models.create(model, type, game::native_name_hash(id), 0, false, 2);
            if (!context || !publish_neighborhood_hook(model, context, type, &record)) return;
            ++created;
        } else if (context) {
            const auto value = game::native_data().models.value(model, context, 0, 0);
            if (!read_bytes(value, &record, sizeof(record))) return;
            references.elements[i].name = s.base + addr::engine::empty_cstring; // native empty CString
            if (apply_neighborhood_profile(record)) game::native_data().models.publish(model, context, type, &record);
        } else return;
        references.elements[i].context = context;
    }
    if (count == 0) {
        std::uintptr_t descriptor{}, array_type{};
        if (!read(s.base + addr::engine::reference_type, descriptor) || !read(descriptor + 0x20, array_type)) return;
        const auto* values = references.elements.data();
        if (game::native_data().models.publish(model, list, array_type, &values)) {
            std::ostringstream event;
            event << "{\"event\":\"local_profile_neighborhoods_hydrated\",\"records\":4,\"created\":"
                << created << '}';
            dingosdk::logging::event(dingosdk::logging::Channel::progression, event.str().c_str());
        }
    }
}

void update_neighborhoods_hook(std::uintptr_t owner) {
    auto& s = local_runtime(); auto& n = neighborhood_runtime();
    n.update(owner);
    PreserveError preserve;
    if (!s.active.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (owner == n.last_owner && now < n.next_poll) return;
    n.last_owner = owner; n.next_poll = now + 1000;
    try { hydrate_neighborhoods(owner); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::progression, "{\"event\":\"local_profile_neighborhood_hydration_failed\"}"); }
}
}