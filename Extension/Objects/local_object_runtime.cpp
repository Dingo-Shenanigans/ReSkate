#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_object.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Engine/Game/Abi/native_data.h"
#include "local_object_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Extension/Objects/object_categories.h"
#include "Extension/Customization/local_customization.h"

namespace dingosdk::profile_runtime {
// Build-kit category subscription: the browser needs this separately from ownership.

ObjectRuntime& object_runtime() { static auto* r = new ObjectRuntime; return *r; }

bool local_object_browser_ready() noexcept {
    PreserveError preserve;
    if (!executing_expression || !local_runtime().active.load(std::memory_order_acquire)) return false;
    std::uintptr_t instance{}, resource{}, current{};
    std::uint32_t key{};
    std::array<std::uint32_t, 10> layout{};
    // Asked by every online check the game's flow scripts make: the running graph's key is
    // peeked first, and the rest read only for the object browser's own graph.
    if (!memory::peek(executing_expression + 0x38, current) || !memory::peek(current + 0x10, key) || key != 0x7f3a3f0b ||
        !read(executing_expression + 0x30, instance) || !read(instance, resource) || current != resource ||
        !read(resource + 0x20, layout) || layout != std::array<std::uint32_t, 10>{
            80, 360, 127, 304, 5, 0, 1, 327704, 1441814, 16910336}) return false;
    // This constructor can use the local category subscription already installed.
    // Its callback waits for inventory; startup/backend and other UI graphs retain
    // the offline result. No global connection state is changed.
    dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_browser_ready\"}");
    return true;
}

void initialize_object_functions(std::uintptr_t base) {
    auto& f = object_runtime().functions;
    f.allocate = reinterpret_cast<decltype(f.allocate)>(base + object_allocate_contract.rva);
    f.construct = reinterpret_cast<decltype(f.construct)>(base + object_ctor_contract.rva);
    f.initialize = reinterpret_cast<decltype(f.initialize)>(base + object_register_contract.rva);
    f.parse = reinterpret_cast<decltype(f.parse)>(base + object_parse_contract.rva);
    f.release = reinterpret_cast<decltype(f.release)>(base + object_release_contract.rva);
}

// Read-only category service DTOs consumed synchronously by the native parser.

// Native conversion deep-copies strings and nested arrays into its result class.

bool accept_object_category(const void*, const void*) { return true; }

std::string object_title(const std::string& key, const dingosdk::Json& overrides) {
    const auto metadata = profile::item_display_metadata(key, overrides);
    if (metadata.contains("title") && metadata.at("title").is_string()) return metadata.at("title").string();
    std::string title = key.starts_with("own_") ? key.substr(4) : key;
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}

void fill_object_categories(std::uintptr_t result, const profile::Snapshot& snapshot) {
    const auto section = snapshot.extensions.value("object_dropper", dingosdk::Json::object());
    const auto catalog = section.value("catalog", dingosdk::Json::object());
    std::vector<objects::InstalledObject> owned;
    for (const auto& [key, info] : cosmetic_runtime().items)
        if (info.build_kit && installed_item_owned(snapshot, key, info.hash, true))
            owned.push_back({key, info.category, object_title(key, catalog.value(key, dingosdk::Json::object()))});
    // Titles come from the cache's category records. Their icons are cdn:/ links
    // that cannot load offline, and the browser would draw the link as text, so
    // each category uses an object thumbnail instead.
    const auto layout = objects::browser_layout(owned, content_cache::catalogs());
    std::vector<ObjectServiceCategory> categories(layout.size());
    std::vector<std::vector<ObjectServiceSubcategory>> subcategories(layout.size());
    // The DTOs borrow these strings until the native parser has copied them.
    std::vector<std::vector<std::string>> icons(layout.size());
    // Sized up front so the borrowed pointers stay valid (no reallocation).
    std::vector<std::string> category_icons(layout.size());
    for (std::size_t index = 0; index < layout.size(); ++index) {
        const auto& group = layout[index];
        auto& parent = categories[index]; auto& children = subcategories[index];
        parent.id.set(group.id); parent.title.set(group.title);
        // The category bar shows its first tile's thumbnail in place of the
        // cache's offline-only icon. Left unset, every tab drew the game's
        // empty-icon placeholder. Same "baseitem:" path as the tiles below.
        if (!group.tiles.empty()) {
            category_icons[index] = "baseitem:" + group.tiles.front().items.front();
            parent.icon.set(category_icons[index]);
        }
        parent.hash = cosmetic_hash(group.id);
        parent.priority = static_cast<std::uint32_t>(index);
        children.resize(group.tiles.size());
        icons[index].reserve(group.tiles.size());
        for (std::size_t item = 0; item < group.tiles.size(); ++item) {
            auto& child = children[item];
            // Each subcategory produces one selectable tile. Its identity must
            // match OwnableData.presentation.category (local_cosmetic_catalog.cpp).
            const auto& tile = group.tiles[item];
            // "baseitem:<asset key>" resolves to the object's own thumbnail
            // from the game data (the DelMarBaseItemAsset Thumbnail chain).
            icons[index].push_back("baseitem:" + tile.items.front());
            child.id.set(tile.id); child.title.set(tile.title); child.icon.set(icons[index].back());
            child.hash = cosmetic_hash(tile.id);
            child.priority = static_cast<std::uint32_t>(item);
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(children.data());
        const auto end = begin + children.size() * sizeof(ObjectServiceSubcategory);
        parent.subcategories = {begin, end, end};
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(categories.data());
    const std::array<std::uintptr_t, 3> vector{begin, begin + categories.size() * sizeof(ObjectServiceCategory),
        begin + categories.size() * sizeof(ObjectServiceCategory)};
    std::array<std::uintptr_t, 2> shared{reinterpret_cast<std::uintptr_t>(&vector), 0};
    const std::array<std::uintptr_t, 4> predicate{0, 0, 0, reinterpret_cast<std::uintptr_t>(&accept_object_category)};
    // The native converter consumes this additional reference, including on an empty list.
    std::uintptr_t consumed{};
    game::native_data().values.null_reference(&consumed, result & ~std::uintptr_t{4});
    object_runtime().functions.parse(shared.data(), &consumed, predicate.data(), predicate.data());
}

void* object_categories_hook(void* destination, const void* callback, const void* group) {
    auto& s = local_runtime(); auto& o = object_runtime(); auto& f = game::native_data().values;
    if (!s.active.load(std::memory_order_acquire)) return o.functions.subscribe(destination, callback, group);
    {
        PreserveError preserve;
        std::lock_guard lock(s.native_mutex);
        try {
            std::string id;
            if (identifier(group, id) && id == "qdbuildkit" && o.pending.available()) {
                game::NativeDelegateGuard retained;
                f.copy_delegate(&retained.value, callback);
                if (retained.value) {
                    o.pending.push({retained.value}); retained.value = 0;
                    dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_queued\"}");
                }
                return f.null_reference(destination, 0);
            }
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_failed\",\"operation\":\"subscribe\"}"); }
    }
    return o.functions.subscribe(destination, callback, group);
}

void update_object_categories() {
    auto& o = object_runtime();
    if (o.pending.empty() || !cosmetic_runtime().published_inventory) return;
    std::uintptr_t allocator{};
    if (!read(local_runtime().base + addr::engine::ui_allocator, allocator) || !allocator) return;
    o.pending.deliver([&](const ObjectRuntime::Pending&, std::uintptr_t callback) {
        try {
            auto* raw = o.functions.allocate(0x20, 8, allocator, 0, 1);
            if (!raw) throw std::bad_alloc();
            o.functions.construct(raw);
            o.functions.initialize(raw, local_runtime().base + addr::local_object::category_result_type, 0, 0, 1);
            ObjectReferenceGuard result;
            game::native_data().values.null_reference(&result.value, reinterpret_cast<std::uintptr_t>(raw));
            fill_object_categories(result.value, (*local_runtime().store->shared_snapshot()));
            game::native_data().values.invoke(&callback, &result.value);
            dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_published\"}");
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_failed\",\"operation\":\"callback\"}"); }
    });
}
}
