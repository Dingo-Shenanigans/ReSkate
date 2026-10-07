#include "park_editor_runtime.h"
#include "park_editor_internal.h"
#include "Extension/Objects/local_placements_runtime.h"

namespace dingosdk::profile_runtime::park_editor_detail {
void update_preview_target(Transaction &tx) {
    if (tx.steps.empty())
        return;
    auto &preview = *tx.preview;
    auto &step = tx.steps.front();
    if (step.kind == Kind::create) {
        if (!tx.sent) {
            if (preview.cancelled)
                tx.steps.clear();
            else
                step.object = preview.target;
        }
        return; // A sent create must acknowledge its original pose first.
    }
    if (preview.cancelled && preview.placing) {
        if (step.kind != Kind::erase) {
            step.kind = Kind::erase;
            step.acknowledged = false;
            tx.sent = false;
            tx.waiting_since = GetTickCount64();
        }
    } else {
        for (auto &move : tx.steps) {
            const auto *target = find_object(tx.change.after, move.object.id);
            if (target && !placement_same_pose(move.object, *target)) {
                move.object = *target;
                move.acknowledged = false;
                move.live_dirty = true;
            }
        }
    }
}
void cancel_preview(Transaction &tx) {
    auto &preview = *tx.preview;
    preview.ending = preview.cancelled = true;
    tx.change.after = tx.change.before;
    if (!preview.placing)
        preview.target = *find_object(tx.change.before, preview.target.id);
    update_preview_target(tx);
    editor_state().status = "Restoring the park after cancelling the preview...";
}
void expire_preview() {
    auto &e = editor_state();
    if (e.transaction && e.transaction->preview && !e.transaction->preview->ending &&
        GetTickCount64() > e.transaction->preview->last_sample + 2000)
        cancel_preview(*e.transaction);
}
} // namespace dingosdk::profile_runtime::park_editor_detail
namespace dingosdk {
using namespace profile_runtime;
using namespace profile_runtime::park_editor_detail;
bool queue_local_park_paste(const EditorPasteRequest &request) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &e = editor_state();
    auto &r = placements_runtime();
    try {
        if (!local_runtime().active || !placement_session_ready() || !idle() ||
            request.generation != e.generation || request.revision != e.revision || request.objects.empty() ||
            request.objects.size() > 1024 || !e.assets)
            return false;
        auto target = r.document.maps[r.map];
        if (target.size() + request.objects.size() > 1024 || r.next_id > UINT64_MAX - request.objects.size())
            return false;
        // The session's own limit on each player: copies that would pass it are not made.
        if (const auto limit = lobby_object_limit(); limit && target.size() + request.objects.size() > limit) {
            e.status = object_limit_notice;
            return false;
        }
        auto next_id = r.next_id;
        for (const auto &copy : request.objects) {
            if (std::none_of(e.assets->begin(), e.assets->end(),
                             [&](const auto &asset) { return asset.key == copy.item; }))
                return false;
            profile::PlacedObject object{next_id++, copy.item, copy.position, copy.rotation, copy.scale};
            if (!profile::valid_placed_object(object))
                return false;
            target.push_back(std::move(object));
        }
        // One validated layout change gives the entire pasted group one undo step.
        if (!begin(std::move(target), 0))
            return false;
        r.next_id = next_id;
        return true;
    } catch (...) {
        return false;
    }
}
bool queue_local_park_preview(const EditorPreviewRequest &request) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &e = editor_state();
    auto &r = placements_runtime();
    try {
        if (!lobby_object_placement_allowed() && request.action != EditorPreviewAction::cancel) return false;
        if (!local_runtime().active || !placement_session_ready() || request.generation != e.generation ||
            !request.token || !request.sequence)
            return false;
        const bool placing = request.action == EditorPreviewAction::begin_place;
        const bool starting = placing || request.action == EditorPreviewAction::begin_move;
        if (starting) {
            if (!idle() || request.revision != e.revision || request.token <= e.last_preview_token)
                return false;
            const auto &layout = r.document.maps[r.map];
            profile::PlacedObject object;
            std::uint64_t entity{};
            if (placing) {
                if (const auto limit = lobby_object_limit(); limit && layout.size() >= limit) {
                    e.status = object_limit_notice;
                    return false;
                }
                if (layout.size() >= 1024 || r.next_id == UINT64_MAX || !e.assets ||
                    std::none_of(e.assets->begin(), e.assets->end(),
                                 [&](const auto &asset) { return asset.key == request.item; }))
                    return false;
                object.id = r.next_id;
                object.item = request.item;
            } else {
                const auto *saved = find_object(layout, request.object);
                const auto row = std::find_if(r.rows.begin(), r.rows.end(), [&](const auto &candidate) {
                    return candidate.saved_id == request.object && candidate.spawned;
                });
                if (!saved || row == r.rows.end())
                    return false;
                object = *saved;
                entity = row->entity;
            }
            object.position = request.position;
            object.rotation = request.rotation;
            object.scale = request.scale;
            if (!profile::valid_placed_object(object))
                return false;
            Transaction tx;
            tx.change = {layout, layout};
            if (placing) {
                tx.change.after.push_back(object);
                ++r.next_id;
            } else {
                for (auto &row : tx.change.after)
                    if (row.id == object.id)
                        row = object;
            }
            tx.steps.push_back({placing ? Kind::create : Kind::move, object, entity});
            if (!placing && !request.transforms.empty()) {
                if (request.transforms.size() > 1024)
                    return false;
                tx.steps.clear();
                tx.change.after = layout;
                std::set<std::uint64_t> seen;
                for (const auto &transform : request.transforms) {
                    const auto *saved = find_object(layout, transform.id);
                    const auto row = std::find_if(r.rows.begin(), r.rows.end(), [&](const auto &candidate) {
                        return candidate.saved_id == transform.id && candidate.spawned;
                    });
                    if (!saved || row == r.rows.end() || !seen.insert(transform.id).second)
                        return false;
                    auto next = *saved;
                    next.position = transform.position;
                    next.rotation = transform.rotation;
                    next.scale = transform.scale;
                    if (!profile::valid_placed_object(next))
                        return false;
                    for (auto &value : tx.change.after)
                        if (value.id == next.id)
                            value = next;
                    tx.steps.push_back({Kind::move, next, row->entity});
                }
                if (!seen.contains(request.object))
                    return false;
            }
            tx.waiting_since = GetTickCount64();
            tx.preview =
                Preview{request.token, request.revision, request.sequence, GetTickCount64(), object, placing};
            for (const auto &step : tx.steps)
                tx.preview->selected.push_back(step.object.id);
            e.transaction = std::move(tx);
            e.last_preview_token = request.token;
            e.status = placing ? "Placing object. Release or click to keep; Escape cancels."
                               : "Moving object. Release to keep; Escape restores.";
            ++e.revision;
            return true;
        }
        if (!e.transaction || !e.transaction->preview)
            return false;
        auto &tx = *e.transaction;
        auto &preview = *tx.preview;
        if (preview.token != request.token || preview.revision != request.revision || preview.ending ||
            request.sequence <= preview.sequence)
            return false;
        if (request.action == EditorPreviewAction::cancel)
            cancel_preview(tx);
        else if (request.action == EditorPreviewAction::update ||
                 request.action == EditorPreviewAction::commit) {
            auto after = tx.change.after;
            auto target = preview.target;
            if (!preview.placing && !request.transforms.empty()) {
                if (request.transforms.size() != preview.selected.size())
                    return false;
                std::set<std::uint64_t> seen;
                for (const auto &transform : request.transforms) {
                    if (std::find(preview.selected.begin(), preview.selected.end(), transform.id) ==
                            preview.selected.end() ||
                        !seen.insert(transform.id).second)
                        return false;
                    for (auto &object : after) {
                        if (object.id != transform.id)
                            continue;
                        object.position = transform.position;
                        object.rotation = transform.rotation;
                        object.scale = transform.scale;
                        if (!profile::valid_placed_object(object))
                            return false;
                    }
                }
                target = *find_object(after, target.id);
            } else {
                if (preview.selected.size() != 1)
                    return false;
                target.position = request.position;
                target.rotation = request.rotation;
                target.scale = request.scale;
                if (!profile::valid_placed_object(target))
                    return false;
                for (auto &object : after)
                    if (object.id == target.id)
                        object = target;
            }
            preview.target = target;
            preview.ending = request.action == EditorPreviewAction::commit;
            tx.change.after = std::move(after);
            update_preview_target(tx);
        } else
            return false;
        preview.sequence = request.sequence;
        preview.last_sample = GetTickCount64();
        return true;
    } catch (...) {
        return false;
    }
}
bool queue_local_park_selection(const EditorSelectionRequest &request) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &e = editor_state();
    if ((!lobby_object_placement_allowed() && !request.objects.empty()) || !local_runtime().active || request.generation != e.generation || request.objects.size() > 1024)
        return false;
    e.selection = request.objects;
    e.selection_time = GetTickCount64();
    return true;
}
bool queue_local_park_surface(const EditorSurfaceRequest &request) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto &e = editor_state();
    if (!lobby_object_placement_allowed() || !local_runtime().active || request.generation != e.generation || !request.id)
        return false;
    e.probe = request; // Latest cursor sample replaces older, unprocessed samples.
    return true;
}
} // namespace dingosdk
