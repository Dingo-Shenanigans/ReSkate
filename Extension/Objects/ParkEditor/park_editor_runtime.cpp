#include "park_editor_runtime.h"
#include "park_editor_internal.h"
#include "Extension/Customization/local_customization.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Objects/object_categories.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/park_editor.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "park_document.h"
#include "park_mods.h"
#include "park_object_sets.h"
#include <chrono>
#include <future>

namespace dingosdk::profile_runtime {
namespace park_editor_detail {
EditorRuntime &editor_state() {
    static auto *value = new EditorRuntime;
    return *value;
}
bool same_layout(const profile::ObjectLayout &a, const profile::ObjectLayout &b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || !placement_same_pose(a[i], b[i]))
            return false;
    return true;
}
const profile::PlacedObject *find_object(const profile::ObjectLayout &objects, std::uint64_t id) {
    const auto it = std::find_if(objects.begin(), objects.end(), [&](const auto &x) { return x.id == id; });
    return it == objects.end() ? nullptr : &*it;
}
std::atomic<bool> park_mod_list_visible{true};
// What is in the Mods folder: every mod's manifest is read to find the ones with parks, which
// with many mods takes ten milliseconds and more.
struct ParkScan {
    std::vector<editor::ParkMod> mods;
    std::vector<std::string> legacy;
};
ParkScan scan_parks(const std::filesystem::path &data_root, const std::filesystem::path &directory) {
    ParkScan found;
    try {
        if (!data_root.empty()) found.mods = editor::list_park_mods(data_root);
        if (!directory.empty()) found.legacy = editor::list_parks(directory);
    } catch (...) { /* An unreadable Mods folder just lists nothing. */ }
    return found;
}
void use_park_scan(ParkScan found) {
    auto &e = editor_state();
    e.park_mods = std::move(found.mods);
    e.legacy_parks = std::move(found.legacy);
    if (!e.project.empty() && std::none_of(e.park_mods.begin(), e.park_mods.end(),
                                           [&](const auto &mod) { return mod.folder == e.project; }))
        e.project.clear(); // The project was removed or renamed outside the game.
}
// The look every few seconds runs on a thread of its own and is taken up when it is done
// (poll_park_mods): on the game's thread it was a hitch every five seconds. `park_scans` counts
// the looks asked for, so one that a later look has overtaken is dropped.
std::future<ParkScan> park_scan;
std::uint64_t park_scans{}, park_scan_running{};
// At once: after the player made, renamed or removed a park, when the list has to be right.
void refresh_park_mods() {
    auto &e = editor_state();
    e.next_park_scan = GetTickCount64() + 5000;
    ++park_scans;
    use_park_scan(scan_parks(e.data_root, e.directory));
}
void refresh_object_sets() {
    auto &e = editor_state();
    e.object_sets = std::make_shared<const std::vector<EditorObjectSet>>(editor::load_object_sets(e.sets_directory));
}
void poll_park_mods() {
    auto &e = editor_state();
    if (park_scan.valid()) {
        if (park_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto found = park_scan.get();
        if (park_scan_running == park_scans) use_park_scan(std::move(found));
        return;
    }
    if (GetTickCount64() < e.next_park_scan) return;
    e.next_park_scan = GetTickCount64() + 5000;
    park_scan_running = ++park_scans;
    park_scan = std::async(std::launch::async, [data_root = e.data_root, directory = e.directory] { return scan_parks(data_root, directory); });
}
// Multiplayer guests play on the host's objects; only the host swaps layouts.
bool is_lobby_guest() {
    return placements_runtime().personal_document.has_value();
}
bool idle() {
    const auto &r = placements_runtime();
    const auto &e = editor_state();
    return lobby_object_placement_allowed() && e.native_ready && !e.failed && !e.transaction && !network_objects_inflight() && r.enabled &&
           !r.clearing && !r.inflight && r.restore.empty() && !r.restore_failed;
}
void fail(std::string reason) {
    auto &e = editor_state();
    if (e.transaction && e.transaction->sent && !e.transaction->steps.empty() &&
        e.transaction->steps.front().kind == Kind::create)
        e.late_create = e.transaction->steps.front();
    e.failed = true;
    e.transaction.reset();
    ++e.revision;
    e.status =
        std::move(reason) + " Reload the level before editing again. The last committed layout is retained.";
    logging::write(logging::Level::error, logging::Channel::objects, e.status);
}
bool begin(profile::ObjectLayout target, int history) {
    auto &r = placements_runtime();
    auto &e = editor_state();
    const auto &before = r.document.maps[r.map];
    if (same_layout(before, target)) {
        e.status = "No changes.";
        ++e.revision;
        return true;
    }
    // Validate the entire layout before sending even its first delete.
    profile::PlacementSnapshot check;
    check.maps.emplace(r.map, target);
    (void)profile::encode_placements(check);
    for (const auto &object : target) {
        if (object.id == UINT64_MAX)
            throw std::runtime_error("Park object identity is out of range.");
        const auto item = cosmetic_runtime().items.find(object.item);
        if (item == cosmetic_runtime().items.end() || !item->second.build_kit)
            throw std::runtime_error("This park needs an object that is not installed: " + object.item);
    }
    Transaction tx;
    tx.change = {before, std::move(target)};
    tx.history = history;
    tx.waiting_since = GetTickCount64();
    for (const auto &old : before) {
        const auto *next = find_object(tx.change.after, old.id);
        if (!next || next->item != old.item) {
            const auto row = std::find_if(r.rows.begin(), r.rows.end(),
                                          [&](const auto &x) { return x.saved_id == old.id && x.spawned; });
            // A saved object that is already absent needs no native delete; its
            // record can still be removed or replaced by this transaction.
            if (row != r.rows.end()) tx.steps.push_back({Kind::erase, old, row->entity});
        }
    }
    for (const auto &next : tx.change.after) {
        const auto *old = find_object(before, next.id);
        if (!old || old->item != next.item)
            tx.steps.push_back({Kind::create, next});
        else if (!placement_same_pose(*old, next)) {
            const auto row = std::find_if(r.rows.begin(), r.rows.end(),
                                          [&](const auto &x) { return x.saved_id == old->id && x.spawned; });
            if (row == r.rows.end())
                throw std::runtime_error("The selected object is not currently spawned.");
            tx.steps.push_back({Kind::move, next, row->entity});
        }
    }
    e.transaction = std::move(tx);
    e.status = "Applying park edit...";
    ++e.revision;
    return true;
}
} // namespace park_editor_detail
using namespace park_editor_detail;
namespace {
void refresh_catalog() {
    auto &e = editor_state();
    auto &c = cosmetic_runtime();
    if (GetTickCount64() < e.next_catalog)
        return;
    e.next_catalog = GetTickCount64() + 1000;
    const auto snapshot_shared = local_runtime().store->shared_snapshot();
    const auto& snapshot = *snapshot_shared;
    if (e.assets && e.catalog_revision == snapshot.revision && e.catalog_manager == c.item_manager)
        return;
    const auto config = snapshot.extensions.value("object_dropper", Json::object());
    const auto catalog = config.value("catalog", Json::object());
    const auto &cache = content_cache::catalogs();
    const auto groups = objects::object_groups(cache);
    auto assets = std::make_shared<std::vector<EditorAsset>>();
    for (const auto &[key, item] : c.items) {
        if (!item.build_kit || !profile_runtime::installed_item_owned(snapshot, key, item.hash, true) || assets->size() >= 4096)
            continue;
        std::string title = key.starts_with("own_") ? key.substr(4) : key;
        std::replace(title.begin(), title.end(), '_', ' ');
        const auto metadata = profile::item_display_metadata(key, catalog.value(key, Json::object()));
        // The game's category (Ramp, Rail, ...) when the content cache places it.
        const auto placement = objects::object_placement(key, cache, groups);
        assets->push_back({key, metadata.value("title", title),
                           placement.category ? placement.category->title : item.category});
    }
    std::sort(assets->begin(), assets->end(), [](const auto &a, const auto &b) {
        return a.category == b.category ? a.title < b.title : a.category < b.category;
    });
    e.assets = std::move(assets);
    e.catalog_revision = snapshot.revision;
    e.catalog_manager = c.item_manager;
}
bool read_object_pose(std::uint64_t entity, std::array<std::uint64_t, 3> &query,
                      std::array<float, 12> &pose) {
    auto &r = placements_runtime();
    alignas(16) std::array<std::uint64_t, 2> reference{};
    // Resolve identity without the native grab-eligibility checks. Those reject
    // resting/static bodies (flag 0x800), which the editor must still position.
    if (r.resolve(reference.data(), entity, false) != reference.data() || reference[0] != entity ||
        !r.valid(reference.data()) || r.query(query.data(), entity) != query.data() ||
        r.pose(pose.data(), query.data()) != pose.data())
        return false;
    // The native position W lane can contain 0xffc00000 (a NaN marker). Only
    // scale XYZ, quaternion XYZW and position XYZ are numeric transform data.
    // Preserve both padding lanes verbatim when passing the pose back to native.
    for (const auto index : {0, 1, 2, 4, 5, 6, 7, 8, 9, 10})
        if (!std::isfinite(pose[index]))
            return false;
    return true;
}
bool object_has_pose(std::uint64_t entity, const profile::PlacedObject &expected) {
    alignas(16) std::array<std::uint64_t, 3> query{};
    alignas(16) std::array<float, 12> pose{};
    if (!read_object_pose(entity, query, pose)) return false;
    auto actual = expected;
    std::copy_n(pose.begin() + 4, 4, actual.rotation.begin());
    std::copy_n(pose.begin() + 8, 3, actual.position.begin());
    actual.scale = pose[0];
    return placement_same_pose(expected, actual) &&
           std::abs(pose[1] - expected.scale) < .0001f &&
           std::abs(pose[2] - expected.scale) < .0001f;
}
bool move_created_duplicate(const profile::PlacedObject &object,
                            const std::set<std::uint64_t> &before,
                            const std::set<std::uint64_t> &live) {
    return std::any_of(live.begin(), live.end(), [&](const auto entity) {
        return !before.contains(entity) && object_has_pose(entity, object);
    });
}
void move_existing(std::uint64_t entity, const profile::PlacedObject &object) {
    auto &e = editor_state();
    alignas(16) std::array<std::uint64_t, 3> query{};
    alignas(16) std::array<float, 12> pose{};
    if (!read_object_pose(entity, query, pose))
        throw std::runtime_error("Selected object's native transform is unavailable.");
    std::fill_n(pose.begin(), 3, object.scale);
    std::copy(object.rotation.begin(), object.rotation.end(), pose.begin() + 4);
    std::copy(object.position.begin(), object.position.end(), pose.begin() + 8);
    alignas(16) std::array<std::uint64_t, 5> physics{};
    if (e.physics_query(query.data(), physics.data()) != physics.data())
        throw std::runtime_error("Selected object's physics component is unavailable.");
    alignas(16) std::array<std::uint64_t, 2> body{};
    if (physics[2] && physics[3] && !read(physics[3], body))
        throw std::runtime_error("Selected object's physics body could not be read.");
    std::uintptr_t source{};
    if (!read(local_runtime().base + addr::park_editor::transform_notification_source, source))
        throw std::runtime_error("Native transform notification source is unavailable.");
    if (physics[2] && physics[3]) {
        // Native body setter expects position XYZ + one uniform scale followed by quaternion.
        alignas(16) std::array<float, 8> rigid{pose[8], pose[9], pose[10], object.scale,
                                               pose[4], pose[5], pose[6],  pose[7]};
        e.physics_move(body.data(), rigid.data());
    }
    e.transform_move(query.data(), pose.data(), source);
}
} // namespace
void initialize_park_editor(const std::filesystem::path &directory) {
    auto &e = editor_state();
    e.directory = directory / L"parks";
    e.sets_directory = directory / L"object_sets";
    refresh_object_sets();
    e.surface_api = editor::surface_api(local_runtime().base);
    e.picking_api = editor::picking_api(local_runtime().base);
    e.highlight_api = editor::highlight_api(local_runtime().base);
    const auto &movement_contracts = addr::park_editor::movement_contracts;
    e.native_ready =
        std::all_of(movement_contracts.begin(), movement_contracts.end(), [](const auto &contract) {
            std::array<unsigned char, 32> bytes{};
            return memory::read_bytes(local_runtime().base + contract.rva, bytes.data(), bytes.size()) &&
                   bytes == contract.bytes;
        });
    if (e.native_ready) {
        e.physics_query =
            reinterpret_cast<decltype(e.physics_query)>(local_runtime().base + movement_contracts[0].rva);
        e.physics_move =
            reinterpret_cast<decltype(e.physics_move)>(local_runtime().base + movement_contracts[1].rva);
        e.transform_move =
            reinterpret_cast<decltype(e.transform_move)>(local_runtime().base + movement_contracts[2].rva);
    }
    refresh_park_mods();
}
void set_park_mods_root(const std::filesystem::path &data_root) {
    std::lock_guard lock(local_runtime().native_mutex);
    editor_state().data_root = data_root;
    refresh_park_mods();
}
void reset_park_editor() {
    auto &e = editor_state();
    ++e.generation;
    ++e.revision;
    e.transaction.reset();
    e.failed = false;
    e.undo.clear();
    e.redo.clear();
    e.status.clear();
    e.probe.reset();
    e.surface = {};
    e.last_preview_token = 0;
    e.selection.clear();
    e.selection_time = 0;
    e.highlights.clear(); // The old realm is gone; its entity IDs must never reach the new realm.
    e.native_drops.clear();
    e.client_roots.clear();
    e.late_create.reset();
}
bool network_object_native_ready() {
    return editor_state().native_ready;
}
void move_network_object(std::uint64_t entity, const profile::PlacedObject &object) {
    move_existing(entity, object);
}
bool park_editor_owns_placements() {
    // A failed editor has no transaction to own. Treating the error latch as
    // ownership swallowed every later native placement acknowledgement and
    // permanently starved the regular place/delete updater.
    return editor_state().transaction.has_value();
}
const profile::ObjectLayout *park_editor_pending_layout() {
    const auto &e = editor_state();
    return e.transaction ? &e.transaction->change.after : nullptr;
}
void park_editor_external_change() {
    auto &e = editor_state();
    if (park_editor_owns_placements())
        return;
    ++e.revision;
    e.undo.clear();
    e.redo.clear();
}
bool park_editor_observe(std::uint64_t entity, const profile::PlacedObject &object) {
    auto &e = editor_state();
    if (!e.transaction) {
        if (!e.late_create) return false;
        auto observed = object;
        observed.scale = e.late_create->object.scale;
        if (e.late_create->before_entities.contains(entity) ||
            !placement_same_pose(e.late_create->object, observed)) return false;
        // Consume only the delayed create that belonged to the failed editor;
        // unrelated native placements must continue through normal persistence.
        e.late_create.reset();
        return true;
    }
    if (!e.transaction || e.transaction->steps.empty() || !e.transaction->sent)
        return true;
    auto &step = e.transaction->steps.front();
    auto observed = object;
    // The native create recipe acknowledgement exposes position/rotation but not
    // the matrix scale. The following move/readback stage verifies the requested
    // scale against the actual world transform before committing it.
    observed.scale = step.object.scale;
    if (step.kind == Kind::create && !step.before_entities.contains(entity) &&
        placement_same_pose(step.object, observed)) {
        step.entity = entity;
        step.acknowledged = true;
    }
    return true;
}
void update_park_editor_moves() noexcept {
    // After the native server park tick, under native_mutex. Client and render
    // threads must not change authoritative world components.
    auto &e = editor_state();
    try {
        expire_preview();
        if (!e.transaction || !placement_session_ready() || !e.transaction || e.transaction->steps.empty())
            return;
        auto &tx = *e.transaction;
        auto &step = tx.steps.front();
        if (tx.preview && step.kind == Kind::move) {
            auto &preview = *tx.preview;
            const auto live = placement_live_entities(placements_runtime().manager);
            if (!live) return; // A single concurrent table read is not an edit failure.
            if (!preview.identities_captured) {
                preview.identities = *live;
                preview.identities_captured = true;
            }
            for (auto &move : tx.steps) {
                if (!live->contains(move.entity)) {
                    // Wait for another stable server snapshot. The preview's
                    // existing timeout remains the bounded failure path.
                    move.acknowledged = false;
                    continue;
                }
                if (move.live_dirty) {
                    move_existing(move.entity, move.object);
                    move.live_dirty = false;
                    move.acknowledged = false;
                    tx.sent_at = GetTickCount64();
                } else if (!move.acknowledged) {
                    move.acknowledged = object_has_pose(move.entity, move.object);
                    if (!move.acknowledged &&
                        move_created_duplicate(move.object, preview.identities, *live)) {
                        fail("Moving an object created a duplicate. The edit was not saved.");
                        return;
                    }
                }
            }
            tx.sent = true;
            return;
        }
        if (step.kind != Kind::move || step.acknowledged)
            return;
        const auto live = placement_live_entities(placements_runtime().manager);
        if (!live || !live->contains(step.entity)) return;
        if (!step.identities_captured) {
            step.before_entities = *live;
            step.identities_captured = true;
        }
        if (!tx.sent) {
            tx.sent = true;
            tx.sent_at = GetTickCount64();
            move_existing(step.entity, step.object);
            return; // Read back next tick, after native systems run.
        }
        step.acknowledged = object_has_pose(step.entity, step.object);
        if (!step.acknowledged &&
            move_created_duplicate(step.object, step.before_entities, *live))
            fail("Moving an object created a duplicate. The edit was not saved.");
    } catch (const std::exception &error) {
        fail(error.what());
    } catch (...) {
        fail("Native object movement failed.");
    }
}
void update_park_editor() {
    auto &e = editor_state();
    auto &r = placements_runtime();
    refresh_catalog();
    if (!placement_session_ready())
        return;
    expire_preview();
    update_highlights();
    if (const auto probe = std::exchange(e.probe, std::nullopt); probe && probe->generation == e.generation) {
        if (e.transaction && e.transaction->preview && e.transaction->sent && !e.transaction->steps.empty() &&
            e.transaction->steps.front().kind == Kind::create && !e.transaction->steps.front().entity) {
            // The preview may have collision before its creation callback arrives.
            // Keep the cursor sample pending until we know which body to exclude.
            e.probe = probe;
        } else {
            if (const auto table = placement_manager_ids(r.manager)) {
                std::map<std::uint32_t, std::uint64_t> native_drops;
                for (const auto &row : *table)
                    native_drops.emplace(row.id, row.entity);
                e.native_drops = std::move(native_drops);
            }
            // The native dropper queries the client physics world. The server map
            // context has the same component type but its world at +0x40 is null.
            // Resolve the current client TLS context here, on the client update.
            std::array<std::uintptr_t, 2> context{};
            if (r.context)
                r.context(context.data());
            std::vector<std::uint64_t> ignored;
            std::vector<editor::SurfaceBody> ignored_bodies;
            if (!probe->picking && e.transaction && e.transaction->preview)
                for (const auto &step : e.transaction->steps)
                    if (step.entity) {
                        ignored.push_back(step.entity);
                        const auto client = client_root(step.entity);
                        if (client && e.native_ready && e.physics_query) {
                            alignas(16) std::array<std::uint64_t, 2> root{};
                            alignas(16) std::array<std::uint64_t, 5> component{};
                            r.resolve(root.data(), client, false);
                            if (root[0] != client || !r.valid(root.data()))
                                continue;
                            e.physics_query(root.data(), component.data());
                            editor::SurfaceBody body{};
                            if (component[2] && component[3] && memory::read(component[3], body) &&
                                e.picking_api.body_valid(body.data()))
                                ignored_bodies.push_back(body);
                        }
                    }
            e.surface = editor::probe_surface(e.surface_api, context[0], *probe, &surface_owner, ignored,
                                              ignored_bodies, &surface_body);
            if (probe->picking)
                for (const auto &row : r.rows)
                    if (row.spawned && row.entity == e.surface.entity)
                        e.surface.object = row.saved_id;
            if (probe->picking && !e.picking_api.ready)
                e.surface.available = false;
        }
    }
    // placement_session_ready may have reset the editor after a world transition.
    if (!e.transaction)
        return;
    auto &tx = *e.transaction;
    if (tx.steps.empty()) {
        if (tx.preview && same_layout(tx.change.before, tx.change.after)) {
            const bool cancelled = tx.preview->cancelled;
            e.transaction.reset();
            ++e.revision;
            e.status = cancelled ? "Preview cancelled." : "No changes.";
            return; // Cancel/no-op preserves autosave and undo/redo history.
        }
        r.document.maps[r.map] = tx.change.after;
        for (const auto &object : tx.change.after)
            r.next_id = std::max(r.next_id, object.id + 1);
        if (tx.history < 0) {
            e.redo.push_back(e.undo.back());
            e.undo.pop_back();
        } else if (tx.history > 0) {
            e.undo.push_back(e.redo.back());
            e.redo.pop_back();
        } else {
            e.undo.push_back(tx.change);
            e.redo.clear();
            if (e.undo.size() > 64)
                e.undo.pop_front();
        }
        e.transaction.reset();
        ++e.revision;
        e.status = r.personal_document ? "Park edit applied for this session."
                                      : "Park edit applied. Autosave queued.";
        if (const auto live = placement_live_entities(r.manager))
            reconcile_placement_rows(*live);
        queue_placement_save();
        return;
    }
    auto &step = tx.steps.front();
    if (tx.preview && step.kind == Kind::move) {
        if (tx.preview->ending && std::all_of(tx.steps.begin(), tx.steps.end(), [](const auto &move) {
                return !move.live_dirty && move.acknowledged;
            })) {
            const auto live = placement_live_entities(r.manager);
            if (!live) return;
            if (std::any_of(tx.steps.begin(), tx.steps.end(), [&](const auto &move) {
                    return !live->contains(move.entity);
                })) return;
            for (const auto &move : tx.steps) {
                r.tracked[move.entity] = move.object.id;
                observe_placement(move.entity, move.object);
            }
            tx.steps.clear();
        } else if (tx.preview->ending && GetTickCount64() > (tx.sent ? tx.sent_at : tx.waiting_since) + 10000)
            fail("The game did not acknowledge the final preview transform.");
        return;
    }
    const auto table = placement_manager_ids(r.manager);
    if (!table) {
        const auto since = tx.sent ? tx.sent_at : tx.waiting_since;
        if (GetTickCount64() > since + 10000)
            fail("The local object table remained unavailable.");
        return;
    }
    std::optional<std::uint32_t> native_id;
    for (const auto &row : *table)
        if (row.entity == step.entity)
            native_id = row.id;
    if (tx.sent) {
        const bool complete =
            step.kind == Kind::erase ? !native_id : step.acknowledged && native_id.has_value();
        if (complete) {
            if (tx.preview && step.kind == Kind::create) {
                const auto entity = step.entity;
                auto identities = step.before_entities;
                identities.insert(entity);
                step = {Kind::move, tx.preview->target, entity};
                step.before_entities = std::move(identities);
                step.identities_captured = true;
                tx.preview->identities = step.before_entities;
                tx.preview->identities_captured = true;
                tx.sent = false;
                tx.waiting_since = GetTickCount64();
                update_preview_target(tx);
                return;
            }
            if (tx.preview && !tx.preview->ending)
                return; // Live poses remain temporary until mouse release.
            if (step.kind == Kind::create && step.object.scale != 1.0f && lobby_object_scaling_allowed()) {
                // The create recipe has no scale, so the object spawned at 1x (a paste, a set, an
                // undo or a loaded park). Resize it with a move, verified by the server tick's
                // readback before the object is committed, as a placement preview does. Not where
                // the session has scaling off: the server would not take it, and the edit would
                // wait on it; the object stays 1x and keeps its scale in the layout.
                step = {Kind::move, step.object, step.entity};
                tx.sent = false;
                tx.waiting_since = GetTickCount64();
                return;
            }
            if (step.kind == Kind::erase) {
                r.tracked.erase(step.entity);
                std::erase_if(r.rows, [&](const auto &row) { return row.entity == step.entity; });
            } else {
                r.tracked[step.entity] = step.object.id;
                observe_placement(step.entity, step.object);
            }
            tx.steps.pop_front();
            tx.sent = false;
            tx.waiting_since = GetTickCount64();
            return;
        }
        if (GetTickCount64() > tx.sent_at + 10000)
            fail("The game did not acknowledge the edit.");
        return;
    }
    if (step.kind == Kind::move) {
        if (GetTickCount64() > tx.waiting_since + 10000)
            fail("The local world update became unavailable.");
        return; // The server tick owns movement and readback.
    }
    const auto channel = placement_client_channel();
    if (!channel) {
        if (GetTickCount64() > tx.waiting_since + 10000)
            fail("The local placement channel became unavailable.");
        return;
    }
    if (step.kind == Kind::erase && !native_id) {
        // Another native path already removed it. Completing the erase here
        // keeps the saved layout and editor transaction in agreement.
        r.tracked.erase(step.entity);
        std::erase_if(r.rows, [&](const auto &row) { return row.entity == step.entity; });
        tx.steps.pop_front();
        tx.waiting_since = GetTickCount64();
        return;
    }
    if (step.kind == Kind::move && !native_id) {
        if (GetTickCount64() > tx.waiting_since + 10000)
            fail("The selected object no longer exists.");
        return;
    }
    tx.sent = true;
    tx.sent_at = GetTickCount64();
    if (step.kind == Kind::erase)
        send_placement_delete(channel, {*native_id});
    else {
        for (const auto &row : *table)
            step.before_entities.insert(row.entity);
        if (!r.creation.source()) { fail("Object request IDs exhausted; restart the game."); return; }
        if (!queue_placement_create(step.object, cosmetic_runtime().items.at(step.object.item).hash,
                                    r.creation.source())) tx.sent = false;
    }
}
} // namespace dingosdk::profile_runtime
namespace dingosdk {
using namespace profile_runtime;
void tick_local_park_editor() noexcept {
    PreserveError preserve;
    try {
        std::lock_guard lock(local_runtime().native_mutex);
        if (!local_runtime().active || cosmetic_runtime().update_thread != GetCurrentThreadId())
            return;
        update_placement_restore();
    } catch (...) {
        std::lock_guard lock(local_runtime().native_mutex);
        fail("Park editing stopped after a native update error.");
    }
}
void set_park_mod_list_visible(bool visible) noexcept { park_mod_list_visible.store(visible, std::memory_order_relaxed); }
ParkEditorModel local_park_editor() {
    std::lock_guard lock(local_runtime().native_mutex);
    const auto &r = placements_runtime();
    auto &e = editor_state();
    ParkEditorModel model;
    model.available = lobby_object_placement_allowed() && local_runtime().active && r.store && !r.failed && !r.save_failed && !r.map.empty() &&
                      e.native_ready && r.enabled;
    model.busy = e.transaction.has_value() || r.clearing || r.inflight.has_value() || !r.restore.empty();
    model.failed = e.failed || r.restore_failed;
    model.generation = e.generation;
    model.revision = e.revision;
    model.map = r.map;
    model.can_undo = idle() && !e.undo.empty();
    model.can_redo = idle() && !e.redo.empty();
    model.assets = e.assets;
    if (park_mod_list_visible.load(std::memory_order_relaxed)) poll_park_mods();
    for (const auto &mod : e.park_mods)
        model.park_mods.push_back({mod.folder, mod.details.title, mod.details.author, mod.details.version,
                                   mod.details.description, mod.enabled,
                                   std::find(mod.maps.begin(), mod.maps.end(), r.map) != mod.maps.end(), mod.maps});
    model.legacy_parks = e.legacy_parks;
    model.project = e.project;
    model.can_load_parks = !is_lobby_guest();
    model.surface = e.surface;
    model.object_sets = e.object_sets;
    if (e.transaction && e.transaction->preview)
        model.preview_token = e.transaction->preview->token;
    model.status = e.status;
    if (!model.available)
        model.status = !r.enabled ? "Enable object persistence to edit parks."
                                  : "Load a supported local level to edit parks.";
    if (!e.native_ready)
        model.status = "Park editor move contract is unavailable for this build.";
    if (r.save_failed)
        model.status = "Object autosave failed. Check objects.sqlite3 before editing again.";
    if (!lobby_object_placement_allowed())
        model.status = "The host has disabled object placement for you in this session.";
    std::set<std::uint64_t> spawned;
    for (const auto &row : r.rows)
        if (row.spawned)
            spawned.insert(row.saved_id);
    if (const auto it = r.document.maps.find(r.map); it != r.document.maps.end())
        for (const auto &object : it->second) {
            model.objects.push_back(
                {object.id, object.item, object.position, object.rotation, spawned.contains(object.id), object.scale});
        }
    return model;
}
} // namespace dingosdk
