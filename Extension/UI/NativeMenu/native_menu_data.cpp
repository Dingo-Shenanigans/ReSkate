#include "native_menu_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace dingosdk::multiplayer::menu_data {
namespace engine = addr::engine;
namespace menu = addr::native_menu;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
unsigned kind(Address type) { return (read<std::uint16_t>(read<Address>(type) + 4) >> 5) & 31; }
unsigned size(Address type) { return kind(type) == 3 ? 8 : read<std::uint16_t>(read<Address>(type) + 6); }
std::string string(Address at, std::size_t maximum) {
    std::string result;
    if (!at) return result;
    for (std::size_t i = 0; i <= maximum; ++i) {
        const auto c = read<char>(at + i);
        if (!c) return result;
        require(i < maximum, "Native menu text exceeds its limit.");
        result += c;
    }
    return result;
}
Address Context::type(Schema schema) const {
    if (const auto cached = types_.find(schema.hash); cached != types_.end()) {
        require(size(cached->second) == schema.size, "Native menu schema differs.");
        return cached->second;
    }
    const auto first = read<Address>(manager + 0x600), last = read<Address>(manager + 0x608);
    require(first && last >= first && (last - first) % 72 == 0 && last - first <= 65536 * 72,
            "Native menu type registry differs.");
    for (auto at = first; at < last; at += 72) {
        const auto begin = read<Address>(at + 8), end = read<Address>(at + 16);
        if (!begin || end <= begin) continue;
        const auto candidate = read<Address>(begin), meta = read<Address>(candidate);
        if (read<std::uint32_t>(meta) != schema.hash) continue;
        require(kind(candidate) == 2 && size(candidate) == schema.size, "Native menu schema differs.");
        types_.emplace(schema.hash, candidate);
        return candidate;
    }
    throw std::runtime_error("Native menu schema has not loaded yet.");
}
Address Context::type_of(Handle handle) const {
    const auto record = reinterpret_cast<Address (*)(Address, Handle, std::uint8_t)>(base + addr::native_party::model_record)(manager, handle, 0);
    return record ? reinterpret_cast<Address (*)(Address, Handle, Address, std::uint8_t)>(base + addr::native_party::model_type)(manager, handle, record, 0) : 0;
}
Address Context::address(Value value) const {
    require(value.handle && value.type && type_of(value.handle) == value.type, "Native menu model has expired.");
    // Return typed storage, including the address of a nullable asset pointer.
    // The unboxed form returns the asset itself (or null), which cannot be used
    // as a publication source and incorrectly rejects empty icon fields.
    const auto result = game::native_data().models.value(manager, value.handle, 0, 1);
    if (!result) throw std::runtime_error("Native menu model value is unavailable (handle " +
        std::to_string(value.handle) + ", schema " + std::to_string(read<std::uint32_t>(read<Address>(value.type))) + ").");
    return result;
}
std::vector<Root> Context::roots(std::initializer_list<std::uint32_t> hashes) const {
    const auto first = read<Address>(manager + 0x98), last = read<Address>(manager + 0xa0);
    require(first && last >= first && (last - first) % 40 == 0 && last - first <= 131072 * 40,
            "Native menu model registry differs.");
    std::vector<Root> result;
    const auto table = read<Address>(manager + 0x600), table_end = read<Address>(manager + 0x608);
    require(table && table_end >= table && (table_end - table) % 72 == 0 &&
            table_end - table <= 65536 * 72, "Native menu type registry differs.");
    struct Record {
        Handle id; Address value; Handle root;
        std::uint16_t type, reserved;
        std::uint32_t parent, index, padding;
    };
    static_assert(sizeof(Record) == 40);
    // The registry can hold a hundred thousand records, and the pause menu's page slots look
    // here every 250 ms until the menu opens: read it in chunks into one reused buffer (no
    // zero-filled copy of the whole thing), and decide each record type once per scan in a
    // flat table rather than a map (profiled 2026-10-01 at up to 7 ms a scan).
    constexpr std::size_t chunk = 4096;
    thread_local std::vector<Record> buffer(chunk);
    constexpr Address unknown = 1;
    std::vector<Address> types(std::min<std::size_t>((table_end - table) / 72, 65536), unknown);
    for (auto at = first; at < last; at += chunk * sizeof(Record)) {
        const auto count = std::min<std::size_t>(chunk, (last - at) / sizeof(Record));
        require(memory::peek_bytes(at, buffer.data(), count * sizeof(Record)), "Native menu model registry is unavailable.");
        for (std::size_t i = 0; i < count; ++i) {
            const auto& record = buffer[i];
            if (record.parent != UINT32_MAX || !record.value || record.type >= types.size()) continue;
            auto& cached = types[record.type];
            if (cached == unknown) {
                cached = 0;
                const auto entry = table + record.type * 72ULL;
                if (!read<Address>(entry + 8)) continue;
                const auto candidate = read<Address>(read<Address>(entry + 8));
                if (candidate && (!hashes.size() || std::find(hashes.begin(), hashes.end(),
                        read<std::uint32_t>(read<Address>(candidate))) != hashes.end()))
                    cached = candidate;
            }
            if (!cached) continue;
            const auto handle = record.root;
            if (type_of(handle) != cached) continue;
            result.push_back({{handle, cached}, game::native_data().models.value(manager, handle, 0, 0)});
        }
    }
    return result;
}
Field Context::member(Address type, std::uint32_t hash) const {
    require(kind(type) == 2, "Native menu field is not a structure.");
    const auto meta = read<Address>(type), fields = read<Address>(meta + 0x60);
    const auto count = read<std::uint16_t>(meta + 0x2a);
    require(count <= 128, "Native menu field table differs.");
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = fields + i * 24;
        if (read<std::uint32_t>(entry) != hash) continue;
        Field result{i, read<std::uint16_t>(entry + 8), read<Address>(entry + 16)};
        require(result.offset + size(result.type) <= size(type), "Native menu field exceeds its structure.");
        return result;
    }
    throw std::runtime_error("Native menu field schema differs.");
}
Value Context::field(Value value, std::uint32_t hash) const {
    address(value);
    const auto f = member(value.type, hash);
    const auto handle = game::native_data().models.field(manager, value.handle, f.index, UINT32_MAX, false);
    require(handle && type_of(handle) == f.type, "Native menu field handle differs.");
    return {handle, f.type};
}
Value Context::path(Value value, std::initializer_list<std::uint32_t> hashes) const {
    for (const auto hash : hashes) value = field(value, hash);
    return value;
}
Value Context::element(Value array, unsigned index) const {
    require(kind(array.type) == 4, "Native menu value is not an array.");
    const auto pointer = read<Address>(address(array));
    require(pointer && index < (read<std::uint32_t>(pointer - 4) & 0x7fffffff), "Native menu array index differs.");
    const auto type = read<Address>(read<Address>(array.type) + 0x30);
    const auto handle = game::native_data().models.field(manager, array.handle, UINT32_MAX, index, false);
    require(handle && type_of(handle) == type, "Native menu array element differs.");
    return {handle, type};
}
Value Context::create(Schema schema, std::uint64_t id) const {
    const auto t = type(schema);
    const auto h = game::native_data().models.create(manager, t, id, game::native_name_hash("ReSkate.NativeMultiplayerMenu"), false, 2);
    Value result{h, t}; address(result); return result;
}
void Context::destroy(Value value) const {
    // The engine may already have removed a previous pause-menu generation.
    // Never resolve an expired handle to a replacement model or destroy a type.
    if (!value.handle || type_of(value.handle) != value.type) return;
    require(game::native_data().models.destroy != nullptr, "Native menu cleanup is unavailable.");
    std::array<Address, 2> iterator{};
    // Same non-forced, client-model removal used by native UI owners. It
    // invalidates bindings and runs typed destructors while assets are live.
    game::native_data().models.destroy(manager, iterator.data(), value.handle, 0, false, false);
    require(type_of(value.handle) != value.type, "Native menu model could not be released.");
}
void Context::publish(Value target, const void* bytes) const {
    address(target);
    game::native_data().models.publish(manager, target.handle, target.type, bytes);
    address(target);
}
void Context::copy(Value target, Address source) const {
    std::vector<std::byte> bytes(size(target.type));
    require(memory::read_bytes(source, bytes.data(), bytes.size()), "Native menu template is unavailable.");
    publish(target, bytes.data());
}
void Context::text(Value target, const std::string& text) const {
    require(kind(target.type) == 7 && text.size() <= 2048, "Native menu text schema differs.");
    Address native{};
    game::native_data().values.assign(&native, text.c_str(), static_cast<std::uint32_t>(text.size()));
    struct Release { Address base; Address& value; ~Release() { if (value) reinterpret_cast<void (*)(Address*)>(base + engine::native_text_release)(&value); } } release{base, native};
    set(target, native);
}
std::string Context::text(Value value, std::size_t maximum) const {
    require(kind(value.type) == 7, "Native menu text field differs.");
    return string(read<Address>(address(value)), maximum);
}
std::vector<std::byte> Context::array(Value value, unsigned maximum, unsigned& count, unsigned& stride) const {
    require(kind(value.type) == 4, "Native menu list schema differs.");
    const auto at = read<Address>(address(value));
    count = at ? read<std::uint32_t>(at - 4) & 0x7fffffff : 0;
    stride = size(read<Address>(read<Address>(value.type) + 0x30));
    require(count <= maximum && stride && stride <= 8192, "Native menu list exceeds its limit.");
    std::vector<std::byte> result(static_cast<std::size_t>(count) * stride);
    require(result.empty() || memory::peek_bytes(at, result.data(), result.size()), "Native menu list is unavailable.");
    return result;
}
void Context::array(Value target, std::span<const std::byte> bytes, unsigned count) const {
    require(kind(target.type) == 4 && count <= 1024, "Native menu list is invalid.");
    const auto stride = size(read<Address>(read<Address>(target.type) + 0x30));
    require(bytes.size() == static_cast<std::size_t>(count) * stride, "Native menu list element size differs.");
    // The engine deep-copies strings, delegates, DataRefs and nested arrays
    // synchronously. No pointer into this temporary storage survives publish.
    std::vector<std::uint64_t> storage((bytes.size() + 15) / 8);
    const std::array<std::uint32_t, 2> header{count, count};
    std::memcpy(storage.data(), header.data(), 8);
    if (!bytes.empty()) std::memcpy(storage.data() + 1, bytes.data(), bytes.size());
    const auto pointer = storage.data() + 1;
    set(target, pointer);
}
Address named_asset(const Context& context, Address anchor, const char* name) {
    const auto find = game::native_data().find_asset;
    if (!find || !anchor) return 0;
    // DataContainer stores its asset-domain ID at +16. A map's UI assets live
    // in that domain and its parents, not the boot-only domain 1. Match the
    // native parent traversal without searching unrelated worlds.
    auto domain = read<std::uint16_t>(anchor + 0x16);
    std::set<std::uint16_t> visited;
    while (domain < 0xbbf && visited.size() < 32 && visited.insert(domain).second) {
        const auto owner = read<Address>(context.base + engine::domain_owners + domain * 8ULL);
        if (!owner) return 0;
        if (const auto asset = find(domain, name)) return asset;
        domain = read<std::uint16_t>(owner + 0x42);
    }
    return 0;
}
namespace {
constexpr std::array<std::string_view, 14> menu_assets{
    "UI/Foundations/Templates/Page/Page_Widget",
    "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget",
    "UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget",
    "UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget",
    "UI/Foundations/Components/Text/InputField/InputField_Widget",
    "UI/Foundations/Components/Text/Label/Widget/Label_Widget",
    "ButtonStyleList/TileButton.RoughPartial.Default",
    "ButtonStyleList/ButtonStyle.CTA.Hero",
    "UI/Foundations/Layers/Backgrounds/Background_Widget",
    "BackgroundStylesList/BackgroundStyle.Dark",
    "UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget",
    "UI/Foundations/Templates/Layouts/Partitions/Horizontal/HorizontalPartition_LinearFocus_Widget",
    "UI/Foundations/Templates/Layouts/Partitions/Vertical/VerticalPartition_LinearFocus_Widget",
    "BackgroundStylesList/BackgroundStyle.Black.Opaque"};
struct Walker {
    std::set<std::pair<Address, Address>> visited;
    std::map<std::string, Address, std::less<>> assets;
    bool complete() const {
        return std::all_of(menu_assets.begin(), menu_assets.end(), [&](auto name) { return assets.contains(name); });
    }
    void walk(Address type, Address value, unsigned depth = 0, bool object = false) {
        if (!value || depth > 18 || visited.size() >= 32768 || complete() || !visited.emplace(type, value).second) return;
        try {
            const auto meta = read<Address>(type);
            const auto k = kind(type);
            if (k == 3 && !object) {
                const auto asset = read<Address>(value);
                if (!asset) return;
                const auto actual = read<Address>(asset + 8), am = read<Address>(actual);
                if (kind(actual) != 3) return;
                const auto hash = read<std::uint32_t>(am);
                if (hash == 0x5a080e4d || hash == 0x5366f94c || hash == 0xa70af1a2)
                    assets.emplace(string(read<Address>(asset + 0x18)), asset);
                if (hash == 0x7aab0d43) {
                    assets.emplace(string(read<Address>(asset + 0x28)), asset);
                    walk(read<Address>(asset + 0x18), read<Address>(asset + 0x20), depth + 1);
                }
                else walk(actual, asset, depth + 1, true);
            } else if (k == 2 || object) {
                const auto count = read<std::uint16_t>(meta + 0x2a);
                if (count > 128) return;
                const auto fields = read<Address>(meta + (object ? 0x38 : 0x60));
                for (unsigned i = 0; i < count; ++i) {
                    const auto f = fields + i * 24;
                    walk(read<Address>(f + 16), value + read<std::uint16_t>(f + 8), depth + 1);
                }
            } else if (k == 4) {
                const auto at = read<Address>(value);
                if (!at) return;
                const auto count = read<std::uint32_t>(at - 4) & 0x7fffffff;
                const auto element = read<Address>(meta + 0x30);
                const auto stride = size(element);
                if (count > 4096 || !stride || stride > 8192) return;
                for (unsigned i = 0; i < std::min(count, 64U); ++i) walk(element, at + stride * i, depth + 1);
            }
        } catch (const std::exception&) { /* Optional asset branches may be unloaded. */ }
    }
};
void foundation_widgets(const Context& context, Walker& walker, std::string_view anchor, Address radius = 4 * 1024 * 1024) {
    if (walker.complete()) return;
    // Some shared widgets (notably InputField) are resident in the Foundations
    // partition but have no live view-model reference until first opened. Limit
    // this fallback to 4 MiB either side of a known Foundations blueprint,
    // and accept only exact native headers and asset names. The allocator splits
    // these neighboring widgets across separate 2 MiB reservations.
    // This is a bounded, one-time lookup, never a whole-process string scan.
    const auto known = walker.assets.find(anchor);
    if (known == walker.assets.end()) return;
    const auto origin = read<Address>(known->second + 8) == context.base + menu::asset_record_type ?
        read<Address>(known->second + 0x20) : known->second;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(origin), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return;
    // VirtualQuery's BaseAddress starts at the queried page. It cannot be used
    // as the lower bound: Label_Widget precedes LabelButton in this allocation.
    const auto begin = origin - std::min(origin, radius);
    const auto end = origin + radius;
    if (end <= begin || end - begin > 16 * 1024 * 1024) return;
    const auto vtable = context.base + engine::blueprint_vtable, native_type = context.base + menu::widget_blueprint_type;
    constexpr std::array<std::string_view, 16> wanted{
        "UI/Foundations/Templates/Page/Page_Widget",
        "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget",
        "UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget",
        "UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget",
        "UI/Foundations/Components/Text/InputField/InputField_Widget",
        "UI/Foundations/Components/Text/Label/Widget/Label_Widget",
        "ButtonStyleList/TileButton.RoughPartial.Default",
        "ButtonStyleList/ButtonStyle.CTA.Hero",
        "UI/Foundations/Layers/Backgrounds/Background_Widget",
        "UI/Foundations/Layers/Backgrounds/TextureBG_Widget",
        "BackgroundStylesList/BackgroundStyle.Dark",
        "UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget",
        "UI/Foundations/Templates/Layouts/Partitions/Horizontal/HorizontalPartition_GroupFocus_Widget",
        "UI/Foundations/Templates/Layouts/Partitions/Horizontal/HorizontalPartition_LinearFocus_Widget",
        "UI/Foundations/Templates/Layouts/Partitions/Vertical/VerticalPartition_GroupFocus_Widget",
        "UI/Foundations/Templates/Layouts/Partitions/Vertical/VerticalPartition_LinearFocus_Widget"};
    for (auto at = begin; at < end;) {
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region))) break;
        const auto next = std::min(end, reinterpret_cast<Address>(region.BaseAddress) + region.RegionSize);
        if (next <= at) break;
        if (region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            std::vector<std::byte> bytes(next - at);
            if (memory::read_bytes(at, bytes.data(), bytes.size())) {
                for (std::size_t off = (8 - (at & 7)) & 7; off + 48 <= bytes.size(); off += 8) {
                    std::array<Address, 6> header{};
                    std::memcpy(header.data(), bytes.data() + off, sizeof(header));
                    const bool widget = header[0] == vtable && header[1] == native_type;
                    const bool texture = header[0] == context.base + menu::texture_asset_vtable && header[1] == context.base + menu::texture_asset_type;
                    const bool image = header[0] == context.base + menu::image_asset_vtable && header[1] == context.base + menu::image_asset_type;
                    const bool record = header[1] == context.base + menu::asset_record_type;
                    if (!widget && !texture && !image && !record) continue;
                    try {
                        const auto name = string(header[record ? 5 : 3], 160);
                        if (std::find(wanted.begin(), wanted.end(), name) != wanted.end()) walker.assets.emplace(name, at + off);
                    } catch (const std::exception&) { }
                }
            }
        }
        at = next;
    }
}
}
std::map<std::string, Address, std::less<>> blueprints(const Context& context, Value core_value) {
    Walker walker;
    const auto social = context.field(context.element(context.path(core_value, {0x716496c8, 0x61742cb4}), 4), 0x716496c8);
    const auto anchor = read<Widget>(context.address(social)).blueprint;
    // Native named lookup avoids walking every live UI model and scanning the
    // neighboring heaps on first pause. Fall back only for unindexed records.
    for (const auto name : menu_assets) {
        const auto asset = named_asset(context, anchor, name.data());
        if (!asset) continue;
        const auto type = read<Address>(asset + 8);
        if (type != context.base + menu::widget_blueprint_type && type != context.base + menu::asset_record_type) continue;
        const auto actual = string(read<Address>(asset + (type == context.base + menu::asset_record_type ? 0x28 : 0x18)), 160);
        if (actual == name) walker.assets.emplace(actual, asset);
    }
    if (walker.complete()) return walker.assets;
    // Styles are exported records within these two small authored lists. Walk
    // those containers before considering unrelated live models or heaps.
    for (const auto* name : {
        "UI/Foundations/Styles/Lists/ButtonStyleList",
        "UI/Foundations/Layers/Backgrounds/BackgroundStylesList"}) {
        const auto asset = named_asset(context, anchor, name);
        if (!asset || read<Address>(asset + 8) != context.base + menu::asset_list_type) continue;
        const auto records = read<Address>(asset + 0x20);
        const auto count = records ? read<std::uint32_t>(records - 4) & 0x7fffffff : 0;
        if (count > 128) continue;
        for (unsigned i = 0; i < count; ++i) {
            const auto record = read<Address>(records + i * 8ULL);
            if (!record || read<Address>(record + 8) != context.base + menu::asset_record_type) continue;
            const auto record_name = string(read<Address>(record + 0x28), 160);
            if (std::find(menu_assets.begin(), menu_assets.end(), record_name) != menu_assets.end())
                walker.assets.emplace(record_name, record);
        }
    }
    if (walker.complete()) return walker.assets;
    walker.walk(core_value.type, context.address(core_value));
    if (walker.complete()) return walker.assets;
    // These roots contain the foundational button/list templates used by the
    // visible menu.
    // Take one registry snapshot, rather than scanning it again for each type.
    for (const auto& root : context.roots({page.hash, button.hash, linear_list.hash, presenter.hash, input.hash}))
        walker.walk(root.model.type, root.value);
    foundation_widgets(context, walker, "UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget");
    foundation_widgets(context, walker, "UI/Foundations/Templates/Page/Page_Widget");
    // Image resources in the small-object arena can straddle more reservations
    // on a fresh launch. Keep this lookup bounded around a resident style.
    foundation_widgets(context, walker, "ButtonStyleList/TileButton.RoughPartial.Default", 8 * 1024 * 1024);
    foundation_widgets(context, walker, "UI/Textures/Icons/Global/img_GlobalNav_Social_256");
    // The resident settings layouts provide a bounded anchor for the fixed
    // partition widgets, which live outside the basic-controls asset arena.
    for (const auto& root : context.roots({0xf0b42cc6})) {
        for (const auto field : {0xf750f0a2U, 0x29023866U}) {
            // Preserve the authored asset reference as well as a mounted
            // widget's effective value, which may have been cleared on teardown.
            for (const auto address : {root.value + context.member(root.model.type, field).offset,
                                      context.address(context.field(root.model, field))}) {
                const auto widget = read<Widget>(address);
                if (widget.blueprint && read<Address>(widget.blueprint + 8) == context.base + menu::widget_blueprint_type)
                    walker.assets.emplace(string(read<Address>(widget.blueprint + 0x18)), widget.blueprint);
            }
        }
    }
    foundation_widgets(context, walker, "UI/Foundations/Templates/Layouts/Partitions/Horizontal/HorizontalPartition_LinearFocus_Widget");
    foundation_widgets(context, walker, "UI/Foundations/Templates/Layouts/Partitions/Vertical/VerticalPartition_LinearFocus_Widget");
    foundation_widgets(context, walker, "UI/Foundations/Templates/Layouts/Partitions/Vertical/VerticalPartition_GroupFocus_Widget");
    foundation_widgets(context, walker, "UI/Foundations/Templates/Layouts/Partitions/Horizontal/HorizontalPartition_GroupFocus_Widget");
    return walker.assets;
}
namespace {
template<std::size_t N, class Publish>
bool with_texture_assets(const Context& context, const std::array<std::string_view, N>& names, Publish&& publish) {
    // RimeTextureAsset registry, shared with native compass icons. Assets from
    // DingoLevel_Root may be far from the current map's widget allocations.
    // Native registry functions verify this table and critical-section layout.
    const auto manager = read<Address>(context.base + menu::texture_registry);
    if (!manager) return false;
    auto* section = reinterpret_cast<CRITICAL_SECTION*>(manager + 0x188);
    EnterCriticalSection(section);
    struct Unlock { CRITICAL_SECTION* value; ~Unlock() { LeaveCriticalSection(value); } } unlock{section};
    if (read<Address>(context.base + menu::texture_registry) != manager) return false;
    const auto buckets = read<unsigned>(manager + 0xa0);
    const auto count = read<unsigned>(manager + 0xa4);
    const auto table = read<Address>(manager + 0x98);
    if (!table || !buckets || buckets > 65536 || count > 32768) return false;
    std::array<Address, N> assets{};
    unsigned visited{};
    const auto matches = [](Address text, std::string_view name) {
        std::array<char, 256> bytes{};
        return name.size() < bytes.size() && memory::read_bytes(text, bytes.data(), name.size() + 1) &&
            bytes[name.size()] == '\0' && std::memcmp(bytes.data(), name.data(), name.size()) == 0;
    };
    const auto complete = [&] { return std::all_of(assets.begin(), assets.end(), [](Address a) { return a != 0; }); };
    for (unsigned i = 0; i < buckets && !complete(); ++i) {
        for (auto node = read<Address>(table + i * 8); node; node = read<Address>(node + 16)) {
            if (++visited > count) return false;
            const auto asset = read<Address>(node + 8);
            if (!asset) continue;
            const auto vtable = read<Address>(asset), type = read<Address>(asset + 8);
            const bool image = vtable == context.base + menu::image_asset_vtable && type == context.base + menu::image_asset_type;
            const bool texture = vtable == context.base + menu::texture_asset_vtable && type == context.base + menu::texture_asset_type;
            if (!image && !texture) continue;
            const auto name = read<Address>(asset + 0x18);
            for (std::size_t n = 0; n < N; ++n)
                if (!assets[n] && matches(name, names[n])) assets[n] = asset;
        }
    }
    // Publish a complete pair, so missing art cannot mix unrelated idle/focus
    // images. No borrowed registry pointer survives this locked operation.
    if (!complete()) return false;
    publish(assets);
    return true;
}
}
bool publish_tab_icons(const Context& context, Value tab, std::string_view idle, std::string_view focused) {
    return with_texture_assets(context, std::array{idle, focused}, [&](const auto& assets) {
        context.set(context.field(tab, 0xd4f0f044), assets[0]); // Icon.
        context.set(context.field(tab, 0xbc92d1fa), assets[1]); // FocusIcon.
    });
}
bool publish_texture(const Context& context, Value target, std::string_view name) {
    return with_texture_assets(context, std::array{name}, [&](const auto& assets) { context.set(target, assets[0]); });
}
} // namespace dingosdk::multiplayer::menu_data
