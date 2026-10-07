#include "Extension/Music/local_music_shelf.h"
#include "Extension/Music/local_music_shelf_lifetime.h"
#include "Extension/Music/local_music_assets.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <algorithm>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::profile_runtime {
namespace {
// Format model handles for shelf publication logs.
std::string music_diag_hex(std::uint64_t value, int width) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i, value >>= 4) out[static_cast<std::size_t>(i)] = digits[value & 15];
    return out;
}
namespace md = dingosdk::multiplayer::menu_data;
struct ModsShelf {
    std::uintptr_t manager{};
    md::Value anchor{}, tiles{}, featured_anchor{}, list{};
    ULONGLONG next_poll{};
    std::vector<std::byte> published_rows;
};
ModsShelf mods_shelf;
md::MenuLifetime music_shelf_lifetime;
md::OwnedMenuModels<md::Value> music_shelf_models;
bool music_shelf_cleaning{};

void music_release_shelf(std::uintptr_t base) {
    if (music_shelf_models.empty()) { mods_shelf = {}; return; }
    // The provider can disappear before this callback. Use the common UI model
    // manager, as native-menu cleanup does, rather than requiring a live provider.
    std::uintptr_t ui{}, manager{};
    md::require(read(base + addr::engine::ui_manager, ui) && ui &&
        read(ui + 0x140, manager) && manager, "Music shelf cleanup manager is unavailable.");
    if (manager != mods_shelf.manager) {
        music_shelf_models.manager_replaced();
        mods_shelf = {};
        return;
    }
    const bool was_cleaning = music_shelf_cleaning;
    music_shelf_cleaning = true;
    struct Restore { bool previous; ~Restore() { music_shelf_cleaning = previous; } } restore{was_cleaning};
    game::ModelWriteLock lock(manager);
    const md::Context context{base, manager};
    music_shelf_models.release([&] {
        if (mods_shelf.list.handle && context.type_of(mods_shelf.list.handle) == mods_shelf.list.type) {
            const auto items = context.field(mods_shelf.list, 0x67223da7U);
            unsigned count{}, stride{};
            const auto bytes = context.array(items, 64, count, stride);
            md::require(stride == sizeof(md::Ref), "Music shelf cleanup list differs.");
            std::vector<md::Ref> refs(count);
            if (!bytes.empty()) std::memcpy(refs.data(), bytes.data(), bytes.size());
            const auto retained = music_shelf_detached(std::span<const md::Ref>{refs}, mods_shelf.anchor.handle);
            if (retained.size() != refs.size()) context.array(items, retained);
        }
        // Unbind the child before reverse-creation-order destruction. Both roots
        // must release their widget references while the outgoing assets live.
        if (mods_shelf.anchor.handle && context.type_of(mods_shelf.anchor.handle) == mods_shelf.anchor.type)
            context.set(context.path(mods_shelf.anchor, {0x214d4984U, 0x25e4d6c8U}), md::Ref{});
    }, [&](md::Value value) { context.destroy(value); });
    mods_shelf = {};
}
// Read inline typed fields without creating child handles during authored construction.
std::uintptr_t music_inline_path(const md::Context& context, std::uintptr_t data,
    std::uintptr_t type, std::initializer_list<std::uint32_t> hashes) {
    for (auto hash : hashes) {
        const auto field = context.member(type, hash);
        data += field.offset;
        type = field.type;
    }
    return data;
}
md::Ref music_tile_ref(const md::Context& context, md::Value anchor) {
    return md::read<md::Ref>(music_inline_path(context, context.address(anchor), anchor.type,
        {0x214d4984U, 0x25e4d6c8U}));
}
void music_sync_mods(const md::Context& context) {
    if (mods_shelf.manager != context.manager || !mods_shelf.tiles.handle) return;
    const auto ref = music_tile_ref(context, mods_shelf.featured_anchor);
    const auto handle = ref.handle & ~std::uint64_t{1};
    if (!handle) return;
    const md::Value featured{handle, context.type_of(handle)};
    md::require(featured.type == mods_shelf.tiles.type, "Featured tile schema differs.");
    const auto source_items = context.field(featured, 0x67223da7U);
    unsigned count{}, stride{};
    const auto source = context.array(source_items, 1024, count, stride);
    const auto row_type = md::read<std::uintptr_t>(md::read<std::uintptr_t>(source_items.type) + 0x30);
    md::require(stride == 1200 && md::read<std::uint32_t>(md::read<std::uintptr_t>(row_type)) == 0xdeb20e0fU,
        "Music tile row schema differs.");
    std::vector<std::byte> filtered;
    unsigned mods{};
    for (unsigned i = 0; i < count; ++i) {
        const auto at = reinterpret_cast<std::uintptr_t>(source.data() + std::size_t{i} * stride);
        const auto tile = md::read<md::Ref>(music_inline_path(context, at, row_type,
            {0x214d4984U, 0x0fb0d794U, 0xa704272aU, 0xc52416efU, 0x0abf7c31U}));
        const auto tile_handle = tile.handle & ~std::uint64_t{1};
        if (!tile_handle) continue;
        const md::Value tile_model{tile_handle, context.type_of(tile_handle)};
        const auto playlist_handle = md::read<std::uint64_t>(music_inline_path(context,
            context.address(tile_model), tile_model.type, {0xeb4f9b4cU})) & ~std::uint64_t{1};
        if (!playlist_handle) continue;
        const md::Value playlist{playlist_handle, context.type_of(playlist_handle)};
        const auto id = context.text(context.field(playlist, 0xa5b84b8aU), 2048);
        if (!id.starts_with("mod:")) continue;
        filtered.insert(filtered.end(), source.begin() + std::size_t{i} * stride,
            source.begin() + std::size_t{i + 1} * stride);
        ++mods;
    }
    const auto destination = context.field(mods_shelf.tiles, 0x67223da7U);
    unsigned previous{}, previous_stride{};
    context.array(destination, 1024, previous, previous_stride);
    md::require(previous_stride == stride, "Mods tile row schema differs.");
    if (previous == mods && mods_shelf.published_rows == filtered) return;
    context.array(destination, filtered, mods);
    mods_shelf.published_rows = std::move(filtered);
    dingosdk::logging::event(dingosdk::logging::Channel::music,
        dingosdk::Json{{"event", "music_mods_shelf_fill"}, {"count", mods}, {"featured", count}}.dump().c_str());
}
void music_poll_mods(std::uintptr_t base) {
    if (music_shelf_lifetime.blocked() || music_shelf_cleaning ||
        !mods_shelf.manager || GetTickCount64() < mods_shelf.next_poll) return;
    mods_shelf.next_poll = GetTickCount64() + 250;
    std::uintptr_t provider{}, manager{};
    if (!read(base + addr::local_music::ui_manager, provider) || !provider ||
        !read(provider + 0x80, manager) || manager != mods_shelf.manager) return;
    try {
        game::ModelWriteLock lock(mods_shelf.manager);
        music_sync_mods(md::Context{base, mods_shelf.manager});
    } catch (const std::exception& error) {
        static std::string last_error;
        if (last_error != error.what()) {
            last_error = error.what();
            dingosdk::logging::event(dingosdk::logging::Channel::music,
                dingosdk::Json{{"event", "music_mods_shelf_waiting"}, {"reason", last_error}}.dump().c_str());
        }
    }
}
// Publish the independent Mods shelf before native widgets bind.
std::string music_shelf_append_one(std::uintptr_t base, std::uintptr_t model, std::uint64_t exact_list = 0) {
    namespace md = dingosdk::multiplayer::menu_data;
    game::ModelWriteLock lock(model);
    md::Context shelf{base, model};
    std::string report = "shelf_append";
    unsigned candidates = 0;
    const auto roots = exact_list
        ? std::vector<md::Root>{{md::Value{exact_list, shelf.type_of(exact_list)}, 0}}
        : shelf.roots({0x48455d84U});
    for (const auto& root : roots) {
        try {
            const auto items = shelf.field(root.model, 0x67223da7U);
            unsigned count = 0, stride = 0;
            const auto bytes = shelf.array(items, 64, count, stride);
            if (stride != 16 || count < 1 || count > 8) continue;
            const auto first = shelf.element(items, 0);
            const auto element_schema = first.type ? md::read<std::uint32_t>(md::read<std::uintptr_t>(first.type)) : 0U;
            if (element_schema != 0x62088281U) continue;
            std::uint64_t target = 0;
            std::memcpy(&target, bytes.data() + 8, sizeof(target));
            std::uint32_t target_schema = 0;
            if (target) { try { const auto type = shelf.type_of(target & ~std::uint64_t{1}); target_schema = type ? md::read<std::uint32_t>(md::read<std::uintptr_t>(type)) : 0U; } catch (const std::exception&) {} }
            ++candidates;
            if (target_schema != 0x0e3be640U) continue;
            report += " list=0x" + music_diag_hex(root.model.handle, 16) + " count=" + std::to_string(count);
            if (count >= 4) { report += " already"; break; }
            if (count < 2) { report += " too_few"; break; }
            std::uint64_t record_ref = 0, source_vm = 0;
            std::memcpy(&record_ref, bytes.data() + stride, 8);
            std::memcpy(&source_vm, bytes.data() + stride + 8, 8);
            const auto source_type = shelf.type_of(source_vm & ~std::uint64_t{1});
            // A screen can be recreated in the same manager without changing maps.
            // Detach and release the old generation, rather than dropping its handles.
            music_release_shelf(base);
            mods_shelf.manager = model;
            mods_shelf.list = root.model;
            auto new_vm = shelf.create(md::Schema{0x0e3be640U, 192}, (GetTickCount64() << 12) | 0x4dU);
            mods_shelf.anchor = new_vm;
            music_shelf_models.track(new_vm);
            md::Value new_tiles{};
            try {
                const md::Value source_anchor{source_vm, source_type};
                shelf.copy(new_vm, shelf.address(source_anchor));
                const auto tile_ref = music_tile_ref(shelf, source_anchor);
                auto tile_handle = tile_ref.handle & ~std::uint64_t{1};
                const auto tile_type = shelf.type(md::Schema{0x74b1ea1dU, 392});
                std::uintptr_t tile_data{};
                if (tile_handle && shelf.type_of(tile_handle) == tile_type)
                    tile_data = shelf.address(md::Value{tile_handle, tile_type});
                else if (tile_ref.record && md::read<std::uintptr_t>(tile_ref.record + 0x18) == tile_type)
                    tile_data = md::read<std::uintptr_t>(tile_ref.record + 0x20);
                md::require(tile_data != 0, "Music tile template is not available.");
                new_tiles = shelf.create(md::Schema{0x74b1ea1dU, 392}, (GetTickCount64() << 12) | 0x4eU);
                mods_shelf.tiles = new_tiles;
                music_shelf_models.track(new_tiles);
                shelf.copy(new_tiles, tile_data);
                shelf.text(shelf.path(new_tiles, {0xfaec2d05U, 0xcb79482dU, 0x4d8e01b9U}), "Mods");
                // An independent list retains each native row's actions and artwork on publication.
                shelf.array(shelf.field(new_tiles, 0x67223da7U), std::span<const std::byte>{}, 0);
                shelf.set(shelf.path(new_vm, {0x214d4984U, 0x25e4d6c8U}), md::Ref{0, new_tiles.handle});
            } catch (...) {
                music_release_shelf(base);
                throw;
            }
            std::vector<std::byte> element(stride);
            std::memcpy(element.data(), &record_ref, 8);
            std::memcpy(element.data() + 8, &new_vm.handle, 8);
            auto merged = bytes;
            merged.insert(merged.begin(), element.begin(), element.end());
            shelf.array(items, merged, count + 1);
            mods_shelf.featured_anchor = md::Value{target, shelf.type_of(target)};
            try { music_sync_mods(shelf); } catch (const std::exception&) {}
            unsigned after = 0, after_stride = 0;
            shelf.array(items, 64, after, after_stride);
            report += " new=0x" + music_diag_hex(new_vm.handle, 16) + " appended=" + std::to_string(count + 1) +
                      " after=" + std::to_string(after);
            break;
        } catch (const std::exception& error) { report += " error=" + std::string(error.what()); continue; }
    }
    report += " candidates=" + std::to_string(candidates);
    return report;
}
}


// CONFIRMED: the authored loader calls the shared constructor at RVA 0x1912670.
// The return path exposes the shelf list before its native widgets bind.
MusicModelConstruct music_model_construct_original{};
std::uint64_t music_model_construct_hook(std::uintptr_t manager, std::uint8_t mode,
    std::uint64_t id, std::uintptr_t type, bool flag, std::uintptr_t record) {
    const auto handle = music_model_construct_original(manager, mode, id, type, flag, record);
    PreserveError preserve;
    if (music_shelf_lifetime.blocked() || music_shelf_cleaning) return handle;
    try {
        if (!handle || !type || md::read<std::uint32_t>(md::read<std::uintptr_t>(type)) != 0x48455d84U)
            return handle;
        bool is_music_shelf = false;
        {
            game::ModelWriteLock lock(manager);
            md::Context context{local_runtime().base, manager};
            const auto items = context.field(md::Value{handle, type}, 0x67223da7U);
            unsigned count{}, stride{};
            const auto bytes = context.array(items, 1024, count, stride);
            if (count != 3 || stride != 16) return handle;
            constexpr const char* records[] = {
                "MusicPlaylistManager_Base_ContentResources/Featured_AnchoredContent_VM",
                "MusicPlaylistManager_Base_ContentResources/Liked_AnchoredContent_VM",
                "MusicPlaylistManager_Base_ContentResources/NewlyDiscovered_AnchoredContent_VM"};
            is_music_shelf = true;
            for (unsigned item = 0; item < 3; ++item) {
                std::uintptr_t item_record{};
                std::memcpy(&item_record, bytes.data() + item * stride, sizeof(item_record));
                // Named record header +0x28 is also used by the native UI dumper.
                if (!item_record || md::string(md::read<std::uintptr_t>(item_record + 0x28), 256) != records[item]) {
                    is_music_shelf = false;
                    break;
                }
            }
        }
        if (!is_music_shelf) return handle;
        // Only add our shelf when an enabled mod actually declares a playlist with songs. Otherwise
        // leave the native three shelves untouched instead of publishing an empty Mods shelf.
        bool has_mod_playlists = false;
        for (const auto& playlist : mod_music_playlists())
            if (!playlist.songs.empty()) { has_mod_playlists = true; break; }
        if (!has_mod_playlists) {
            music_release_shelf(local_runtime().base);
            dingosdk::logging::event(dingosdk::logging::Channel::music, "{\"event\":\"music_mods_shelf_skipped\"}");
            return handle;
        }
        const auto report = music_shelf_append_one(local_runtime().base, manager, handle);
        dingosdk::logging::event(dingosdk::logging::Channel::music,
            dingosdk::Json{{"event", "music_shelf_append"}, {"report", report}}.dump().c_str());
    } catch (...) {} // A shelf failure must not interrupt the native constructor.
    return handle;
}

void music_shelf_before_level_transition(unsigned next) noexcept {
    try {
        music_shelf_lifetime.before_transition(next, [&] {
            music_release_shelf(local_runtime().base);
            dingosdk::logging::event(dingosdk::logging::Channel::music,
                "{\"event\":\"music_mods_shelf_released\"}");
        });
    } catch (...) {
        try { dingosdk::logging::event(dingosdk::logging::Channel::music,
            "{\"event\":\"music_mods_shelf_cleanup_failed\"}"); } catch (...) {}
    }
}

void update_music_shelf() {
    auto& runtime = local_runtime();
    if (!runtime.active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    music_poll_mods(runtime.base);
}
} // namespace dingosdk::profile_runtime
