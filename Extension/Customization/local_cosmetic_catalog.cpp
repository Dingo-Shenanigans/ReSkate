#include "Engine/Core/Log/logging.h"
#include "local_cosmetic_catalog.h"
#include "local_customization_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Objects/object_categories.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_cosmetic_catalog.h"
#include "item_origins.h"

namespace dingosdk::profile_runtime {
// Retail OwnableData catalog bridge; included after the installed-item reader.

// Only used by the authored-offline profile on the game-update thread.

void cosmetic_release(CosmeticShared& value) noexcept {
    const auto control = value.control;
    value = {};
    if (!control) return;
    const auto address = reinterpret_cast<std::uintptr_t>(control);
    const auto table = *reinterpret_cast<std::uintptr_t**>(control);
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(address + 8)) == 0)
        reinterpret_cast<void (*)(void*)>(table[1])(control);
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(address + 12)) == 0)
        reinterpret_cast<void (*)(void*)>(table[2])(control);
}

CosmeticCatalogFunctions& cosmetic_catalog_functions() {
    static auto* f = new CosmeticCatalogFunctions;
    return *f;
}

void initialize_cosmetic_catalog_functions(std::uintptr_t base) {
    auto& f = cosmetic_catalog_functions();
    f.allocate = reinterpret_cast<decltype(f.allocate)>(base + cosmetic_allocate_contract.rva);
    f.construct = reinterpret_cast<decltype(f.construct)>(base + cosmetic_construct_contract.rva);
    f.decode = reinterpret_cast<decltype(f.decode)>(base + cosmetic_decode_contract.rva);
    f.create = reinterpret_cast<decltype(f.create)>(base + cosmetic_create_catalog_contract.rva);
    f.complete = reinterpret_cast<decltype(f.complete)>(base + cosmetic_complete_catalog_contract.rva);
    f.populate_inventory = reinterpret_cast<decltype(f.populate_inventory)>(base + cosmetic_populate_inventory_contract.rva);
    f.ui_ready = reinterpret_cast<decltype(f.ui_ready)>(base + cosmetic_ui_ready_contract.rva);
    f.message_control_vtable = base + addr::local_cosmetic_catalog::message_control_vtable;
    f.allocator_adapter = {base + addr::engine::allocator_adapter_vtable, 0, 8};
}

void cosmetic_varint(std::string& out, std::uint64_t value) {
    while (value >= 128) { out += static_cast<char>((value & 127) | 128); value >>= 7; }
    out += static_cast<char>(value);
}

void cosmetic_wire_number(std::string& out, unsigned field, std::uint64_t value) {
    cosmetic_varint(out, static_cast<std::uint64_t>(field) << 3); cosmetic_varint(out, value);
}

void cosmetic_wire_string(std::string& out, unsigned field, std::string_view value) {
    cosmetic_varint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
    cosmetic_varint(out, value.size()); out.append(value);
}

std::string cosmetic_ownable_message(const std::string& key, const CosmeticItemInfo& info, bool owned,
    const dingosdk::Json& metadata) {
    // Local identity is deliberately the installed asset key. It is never sent
    // to an online service or represented as an original server catalog ID.
    std::string title = (key.starts_with("Own_") || key.starts_with("own_")) ? key.substr(4) : key;
    std::replace(title.begin(), title.end(), '_', ' ');
    std::string presentation;
    cosmetic_wire_string(presentation, 2, metadata.value("title", title));
    if (metadata.contains("description")) cosmetic_wire_string(presentation, 3, metadata.at("description").get<std::string>());
    // The Object Browser makes one tile per subcategory. Objects name their
    // tile (objects::browser_tile) in "group": the game's own variant family,
    // or the object itself when the content cache does not place it.
    cosmetic_wire_string(presentation, 5, info.build_kit ? metadata.value("group", key) : info.category);
    // BuildKitObjectType name ("QuickDrop") as the live records send it.
    if (info.build_kit && metadata.contains("object_type"))
        cosmetic_wire_string(presentation, 6, metadata.at("object_type").get<std::string>());
    cosmetic_wire_number(presentation, 7, static_cast<std::uint64_t>(static_cast<std::int64_t>(info.sort_priority)));
    // categoryIdHash. For objects the Object Browser matches it against its
    // category's hash (live records send djb2 of e.g. "category_ramp"); a
    // mismatch leaves the category's tiles without a default item, drawn as Ø.
    if (info.build_kit && metadata.contains("browser_category"))
        cosmetic_wire_number(presentation, 9, cosmetic_hash(metadata.at("browser_category").get<std::string>()));
    else
        cosmetic_wire_number(presentation, 9, info.categories.empty() ? 0 : info.categories.front());
    cosmetic_wire_string(presentation, 10, key);
    cosmetic_wire_number(presentation, 11, info.hash);
    // Empty icon/background invoke the native baseitem:/baseitembg: thumbnails.
    std::string body;
    cosmetic_wire_string(body, 1, key);
    cosmetic_wire_string(body, 6, presentation);
    cosmetic_wire_number(body, 10, owned ? 1 : 0);
    // The OwnableData decoder maps tag 0xba (field 23) to rarityId +160.
    // The ID references a separate server rarity definition; never infer one from an asset key.
    if (metadata.contains("rarity_id")) {
        cosmetic_wire_string(body, 23, metadata.at("rarity_id").get<std::string>());
    } else if (!info.build_kit && content_cache::catalogs().available && !reserved_cosmetic(key)) {
        // A cosmetic a mod installed is in no retail catalogue, so it has no
        // rarity of its own and its card would show none. Collector is the
        // game's own sixth tier, already styled and already used by 395 retail
        // items: it reads as something apart without claiming to be Legendary.
        // A rarity_id the mod or the profile gives still wins, and with no
        // catalogue installed nothing is judged modded.
        cosmetic_wire_string(body, 23, "collector");
    }
    std::string framed;
    cosmetic_varint(framed, body.size()); framed += body;
    return framed;
}

CosmeticShared make_cosmetic_message(const std::string& wire) {
    auto& f = cosmetic_catalog_functions();
    std::uintptr_t allocator{};
    auto* block = static_cast<std::byte*>(f.allocate(&allocator, 0x1e8, 0));
    if (!block) throw std::bad_alloc();
    std::memset(block, 0, 0x1e8);
    std::memcpy(block, &f.message_control_vtable, 8);
    const std::uint32_t one = 1;
    std::memcpy(block + 8, &one, 4); std::memcpy(block + 12, &one, 4);
    std::memcpy(block + 0x1e0, &allocator, 8);
    const auto adapter = reinterpret_cast<std::uintptr_t>(f.allocator_adapter.data());
    const std::array<std::uintptr_t, 3> context{adapter, adapter, 0};
    f.construct(block + 0x10, context.data());
    CosmeticSharedGuard result{{block + 0x10, block}};
    // The native reader constructor builds precisely this: bytes, length, offset, ended.
    std::array<std::uintptr_t, 4> reader{reinterpret_cast<std::uintptr_t>(wire.data()), wire.size(), 0, 0};
    if (!f.decode(reader.data(), result.value.body) || reader[2] != wire.size())
        throw std::runtime_error("Native cosmetic record decode failed");
    const auto value = result.value; result.value = {};
    return value;
}

// The completion callback retains a shared map and shared messages separately.

// Its recovered refcount ABI increments both counters for each strong alias.

// Only the map storage is ReSkate-owned; messages use native control blocks.

void* cosmetic_map_destructor(CosmeticCatalogOwner* owner, unsigned flags) noexcept {
    owner->clear();
    if (flags & 1) delete owner;
    return owner;
}

void cosmetic_map_destroy(CosmeticCatalogOwner* owner) noexcept { owner->clear(); }

void cosmetic_map_free(CosmeticCatalogOwner* owner) noexcept { delete owner; }

void* cosmetic_map_deleter(void*, void*) noexcept { return nullptr; }

CosmeticShared make_cosmetic_catalog(const profile::Snapshot& snapshot) {
    static const std::array<std::uintptr_t, 4> table{
        reinterpret_cast<std::uintptr_t>(&cosmetic_map_destructor),
        reinterpret_cast<std::uintptr_t>(&cosmetic_map_destroy),
        reinterpret_cast<std::uintptr_t>(&cosmetic_map_free),
        reinterpret_cast<std::uintptr_t>(&cosmetic_map_deleter)};
    const auto& items = cosmetic_runtime().items;
    if (items.empty() || items.size() > 8192) throw std::runtime_error("Invalid local catalog size");
    auto owner = std::make_unique<CosmeticCatalogOwner>();
    owner->vtable = table.data();
    owner->nodes.resize(items.size());
    const auto bucket_count = static_cast<std::uint32_t>(items.size() * 2 + 1);
    owner->buckets.resize(bucket_count + 1);
    // EASTL's iteration scans past empty buckets until this non-null sentinel.
    owner->buckets.back() = &owner->sentinel;
    owner->map = {owner->buckets.data(), bucket_count, static_cast<std::uint32_t>(items.size())};
    const auto catalog = snapshot.customization.value("catalog", dingosdk::Json::object());
    const auto object_dropper = snapshot.extensions.value("object_dropper", dingosdk::Json::object());
    const auto object_catalog = object_dropper.value("catalog", dingosdk::Json::object());
    const auto groups = objects::object_groups(content_cache::catalogs());
    std::set<std::uint32_t> hashes;
    std::size_t index{}, left_out{};
    // One item the catalog cannot hold (two mods' items with one identity, a record the game will not
    // read) is left out and named in the log; the rest still publish. Failing the whole catalog for
    // it left a player owning nothing: no items in the customization menu, no playcard.
    const auto leave_out = [&](const std::string& key, const char* why) {
        if (++left_out <= 20)
            dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::customization,
                "Cosmetic item {} left out of the catalog: {}.", key, why);
    };
    for (const auto& [key, info] : items) {
        if (!hashes.insert(info.hash).second) {
            leave_out(key, "another item has the same identity hash");
            continue;
        }
        auto& node = owner->nodes[index];
        node.hash = info.hash;
        const auto& item_catalog = info.build_kit ? object_catalog : catalog;
        const bool owned = installed_item_owned(snapshot, key, info.hash, info.build_kit);
        auto metadata = profile::item_display_metadata(key, item_catalog.value(key, dingosdk::Json::object()));
        if (info.build_kit) {
            // The same tile and category the browser layout gives this object.
            const auto placement = objects::object_placement(key, content_cache::catalogs(), groups);
            metadata["group"] = placement.group ? placement.group->id : key;
            metadata["browser_category"] = placement.category ? placement.category->id : info.category;
        }
        // A mod's item (or one of the game's own a mod changes, unless it has a description of
        // its own) keeps three empty lines where the game shows a description: room at the
        // bottom of the game's item card, whose bottom edge never moves, for the overlay to show
        // the mod there.
        if (!info.build_kit && !metadata.contains("description") && item_origins::find(key))
            metadata["description"] = std::string(item_origins::reserved_description);
        try {
            node.item = make_cosmetic_message(cosmetic_ownable_message(key, info, owned, metadata));
        } catch (const std::exception& failure) {
            node = {};
            hashes.erase(info.hash);
            leave_out(key, failure.what());
            continue;
        }
        ++index;
        const auto bucket = info.hash % bucket_count;
        node.next = owner->buckets[bucket]; owner->buckets[bucket] = &node;
    }
    if (!index) throw std::runtime_error("No cosmetic item could be put in the catalog");
    if (left_out)
        dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::customization,
            "Cosmetic catalog: {} of {} items left out (see above).", left_out, items.size());
    owner->map.count = static_cast<std::uint32_t>(index);
    cosmetic_runtime().catalog_items = index;
    CosmeticShared result{&owner->map, owner.get()}; owner.release();
    return result;
}

void publish_cosmetic_catalog() {
    auto& c = cosmetic_runtime(); auto& f = cosmetic_catalog_functions();
    const auto base = local_runtime().base;
    std::uintptr_t manager{}, arena{}, allocator_vtable{}, ui_allocator{};
    if (!f.ui_ready || !f.ui_ready() || !read(base + addr::local_cosmetic_catalog::ownable_manager, manager)) return;
    if (manager && manager == c.published_manager) {
        // Items installed or removed since (a live mod apply, picked up at the
        // level load that reloads the cosmetics bundle): hand the manager a new
        // catalog. The native completion replaces the installed one, rebuilds
        // its indexes and tells the CAS pages the catalog changed.
        if (c.published_generation == c.items_generation) return;
        std::uintptr_t vtable{};
        if (!read(manager, vtable) || vtable != base + addr::local_cosmetic_catalog::ownable_manager_vtable) return;
        CosmeticSharedGuard catalog{make_cosmetic_catalog(*local_runtime().store->shared_snapshot())};
        f.complete(&catalog.value, &manager);
        std::uintptr_t installed{}; std::uint32_t count{};
        if (!read(manager + 0x28, installed) || !installed || !read(installed + 12, count) || count != c.catalog_items)
            throw std::runtime_error("Cosmetic catalog republication did not complete");
        c.published_generation = c.items_generation;
        dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::customization,
            "Cosmetic catalog republished after a mod change: {} items.", count);
        return;
    }
    // A real service-backed manager retains ownership of its own catalog.
    if (manager && manager != c.published_manager) return;
    if (!read(base + addr::engine::default_arena, arena) || !arena || !read(arena, allocator_vtable) ||
        allocator_vtable < base || allocator_vtable >= base + supported_build::game_image_size ||
        !read(base + addr::engine::ui_allocator, ui_allocator) || !ui_allocator) return;
    f.allocator_adapter[1] = arena;
    CosmeticSharedGuard catalog{make_cosmetic_catalog(*local_runtime().store->shared_snapshot())};
    dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_cosmetic_catalog_decoded\"}");
    f.create(ui_allocator, 0); // No online provider or network request.
    if (!read(base + addr::local_cosmetic_catalog::ownable_manager, manager) || !manager) throw std::runtime_error("Cosmetic UI manager unavailable");
    std::uintptr_t vtable{};
    if (!read(manager, vtable) || vtable != base + addr::local_cosmetic_catalog::ownable_manager_vtable)
        throw std::runtime_error("Unexpected cosmetic UI manager");
    dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_cosmetic_catalog_manager_created\"}");
    f.complete(&catalog.value, &manager); // Consumes map; publishes both native indexes and UI readiness.
    std::uintptr_t installed{}; std::uint32_t count{};
    if (!read(manager + 0x28, installed) || !installed || !read(installed + 12, count) || count != c.catalog_items)
        throw std::runtime_error("Cosmetic catalog publication did not complete");
    c.published_manager = manager;
    c.published_generation = c.items_generation;
    std::ostringstream event;
    event << "{\"event\":\"local_cosmetic_catalog_published\",\"items\":" << count << '}';
    dingosdk::logging::event(dingosdk::logging::Channel::customization, event.str().c_str());
}

std::vector<std::uint32_t> cosmetic_owned_ids(const profile::Snapshot& snapshot) {
    std::vector<std::uint32_t> ids;
    for (const auto& [key, info] : cosmetic_runtime().items)
        if (installed_item_owned(snapshot, key, info.hash, info.build_kit)) ids.push_back(info.hash);
    return ids;
}

void publish_cosmetic_inventory() {
    auto& s = local_runtime(); auto& c = cosmetic_runtime(); auto& f = cosmetic_catalog_functions();
    if (!s.active.load(std::memory_order_acquire) || c.update_thread != GetCurrentThreadId() ||
        !c.published_manager || !f.populate_inventory || !f.ui_ready || !f.ui_ready()) return;
    std::uintptr_t ownables{}, inventory{}, vtable{}, bound_catalog{}, data_model{}, service{};
    std::uint32_t count{};
    if (!read(s.base + addr::local_cosmetic_catalog::ownable_manager, ownables) || ownables != c.published_manager ||
        !read(s.base + addr::local_cosmetic_catalog::inventory_manager, inventory)) return;
    if (!inventory) {
        c.published_inventory = c.inventory_data_model = 0;
        c.published_inventory_count = 0;
        return;
    }
    // The native constructor builds this manager with the current OwnableData manager.
    // Its normal subscriptions are skipped without an inventory
    // service, even though the default-owned catalog is already available.
    if (!read(inventory, vtable) || vtable != s.base + addr::local_cosmetic_catalog::inventory_manager_vtable ||
        !read(inventory + 0x1b0, service) || service ||
        !read(inventory + 0x1d0, bound_catalog) || (bound_catalog && bound_catalog != ownables) ||
        !read(inventory + 0x1c8, data_model) || !data_model ||
        !read(inventory + 0x10c, count) || count > 8192) return;
    if (!bound_catalog) {
        // Offline UI setup can construct inventory before our catalog exists.
        // The native binder stores a borrowed OwnableData pointer at +0x1d0; without a
        // service there is no later subscription to repair that null binding.
        // Bind only an empty offline inventory to our current published catalog,
        // on the game-update thread, before the native consumer dereferences it.
        if (count) return;
        const auto previous = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(inventory + 0x1d0),
            reinterpret_cast<void*>(ownables), nullptr);
        if (previous && reinterpret_cast<std::uintptr_t>(previous) != ownables) return;
    }
    if (inventory == c.published_inventory && data_model == c.inventory_data_model &&
        count == c.published_inventory_count) return;

    const auto ids = cosmetic_owned_ids(*s.store->shared_snapshot());
    if (ids.size() > 8192) throw std::runtime_error("Invalid local cosmetic inventory size");
    // Same synchronous consumer as the native default-owned subscription
    // callback. It copies IDs into native models,
    // sets ownership and updates the category collections used by CAS pages.
    // Only begin/end are read; no borrowed storage survives this call.
    const auto begin = reinterpret_cast<std::uintptr_t>(ids.data());
    const auto end = begin + ids.size() * sizeof(std::uint32_t);
    const std::array<std::uintptr_t, 4> borrowed{begin, end, end, 0};
    f.populate_inventory(inventory, borrowed.data());
    if (!read(inventory + 0x10c, count) || count != ids.size()) {
        std::ostringstream event;
        event << "{\"event\":\"local_cosmetic_inventory_incomplete\",\"expected\":" << ids.size()
              << ",\"actual\":" << count << '}';
        dingosdk::logging::event(dingosdk::logging::Channel::customization, event.str().c_str());
        throw std::runtime_error("Cosmetic inventory publication did not complete");
    }
    c.published_inventory = inventory; c.inventory_data_model = data_model;
    c.published_inventory_count = count;
    std::ostringstream event;
    event << "{\"event\":\"local_cosmetic_inventory_published\",\"items\":" << count << '}';
    dingosdk::logging::event(dingosdk::logging::Channel::customization, event.str().c_str(),
        dingosdk::logging::Level::info);
}
}
