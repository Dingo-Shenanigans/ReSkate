#include "park_editor_internal.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace dingosdk::overlay::park_editor_detail {
using namespace editor;
namespace {
constexpr ImU32 axes[]{IM_COL32(243, 100, 106, 255), IM_COL32(113, 220, 138, 255),
                       IM_COL32(91, 166, 255, 255)};
float length(ImVec2 a) {
    return std::sqrt(a.x * a.x + a.y * a.y);
}
float line_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 d{b.x - a.x, b.y - a.y};
    const float n = d.x * d.x + d.y * d.y;
    const float t = n > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / n, 0.0f, 1.0f) : 0;
    return length({p.x - a.x - t * d.x, p.y - a.y - t * d.y});
}
std::optional<ImVec2> project(const Camera &camera, ImVec2 origin, Vec3 point) {
    if (const auto p = camera.project(point))
        return plus(origin, {(*p)[0], (*p)[1]});
    return {};
}
void wire_preview(ImDrawList *draw, const Camera &camera, ImVec2 origin, Vec3 p, Quat q, float scale,
                  ImU32 color) {
    std::array<std::optional<ImVec2>, 8> points;
    for (unsigned i = 0; i < 8; ++i)
        points[i] =
            project(camera, origin,
                    add(p, rotated({((i & 1) ? .5f : -.5f) * scale,
                                    ((i & 2) ? .5f : -.5f) * scale,
                                    ((i & 4) ? .5f : -.5f) * scale}, q)));
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned bit = 1; bit <= 4; bit *= 2)
            if (!(i & bit) && points[i] && points[i | bit])
                draw->AddLine(*points[i], *points[i | bit], color, 1.8f);
}
void draw_grid(ImDrawList *draw, const Camera &camera, ImVec2 origin, float height, float spacing) {
    spacing = std::max(1.0f, spacing);
    const auto c = camera.origin();
    const float x = snapped(c[0], spacing), z = snapped(c[2], spacing), radius = spacing * 12;
    for (int i = -12; i <= 12; ++i) {
        const float offset = static_cast<float>(i) * spacing;
        for (unsigned axis = 0; axis < 2; ++axis) {
            Vec3 a = axis ? Vec3{x - radius, height, z + offset} : Vec3{x + offset, height, z - radius};
            Vec3 b = axis ? Vec3{x + radius, height, z + offset} : Vec3{x + offset, height, z + radius};
            const auto pa = project(camera, origin, a), pb = project(camera, origin, b);
            if (pa && pb)
                draw->AddLine(*pa, *pb, IM_COL32(130, 156, 188, 45));
        }
    }
}
} // namespace
void viewport(ParkEditorUI &ui, const Model &model, ImVec2 screen_origin, ImVec2 screen_size, bool blocked) {
    const Camera camera{model.debug.camera_transform, model.debug.camera_fov, screen_size.x, screen_size.y};
    auto *draw = ImGui::GetWindowDrawList();
    const auto start = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("world-viewport", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const auto mouse = ImGui::GetIO().MousePos;
    const bool usable = model.debug.camera_transform_valid && camera.valid() && !blocked;
    const bool mouse_in =
        mouse.x >= start.x && mouse.y >= start.y && mouse.x < start.x + size.x && mouse.y < start.y + size.y;
    if (!mouse_in || !ImGui::IsMouseDown(ImGuiMouseButton_Right) || blocked)
        ui.flying = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && usable && ui.dragging < 0)
        ui.flying = true;
    // Mouse look: no pointer while the camera turns.
    if (ui.flying)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    draw->PushClipRect(start, plus(start, size), true);
    if (!usable) {
        ui.surface_confirm = false;
        if (ui.dragging >= 0 || ui.preview_token)
            cancel_drag(ui, model);
        draw->AddText(plus(start, {22, 20}), muted,
                      model.debug.camera_transform_valid ? "Editor paused"
                                                         : "Waiting for the editor camera...");
        draw->PopClipRect();
        return;
    }
    if (!ui.surface_mode)
        draw_grid(draw, camera, screen_origin, ui.plane_height, ui.grid);
    const auto ray = camera.ray(mouse.x - screen_origin.x, mouse.y - screen_origin.y);
    const bool own_preview = ui.preview_token && model.editor.preview_token == ui.preview_token;
    const bool can_edit =
        !ui.pending && (!model.editor.busy || own_preview) && !model.editor.failed && model.editor.available;
    bool dropped = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const auto *payload =
                ImGui::AcceptDragDropPayload("PARK_ASSET", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            if (payload->DataSize > 0 && payload->DataSize < 1024) {
                ui.placing = static_cast<const char *>(payload->Data);
                ui.placing_set.clear();
                ui.selected = 0;
                ui.selection.clear();
                ui.angles = {};
                ui.scale = 1;
                dropped = payload->IsDelivery();
                if (dropped)
                    ui.asset_drag = false;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ui.asset_drag && !ImGui::GetDragDropPayload() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        cancel_drag(ui, model);
        ui.placing.clear();
    }
    if (!ui.placing.empty()) {
        if (ui.surface_mode) {
            const auto &hit = model.editor.surface;
            const bool previous_current =
                hit.id && hit.id == ui.surface_request.id && hit.generation == model.editor.generation;
            const bool confirm =
                can_edit && (dropped || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)));
            const float grid = ui.snapping ? ui.grid : 0;
            const bool changed = ui.surface_request.generation != model.editor.generation ||
                                 ui.surface_request.origin != camera.origin() ||
                                 ui.surface_request.direction != ray || ui.surface_request.grid != grid ||
                                 ui.surface_request.picking;
            if (mouse_in && !ui.flying && !ui.surface_confirm &&
                (confirm || !ui.surface_request.id ||
                 (changed && previous_current && ImGui::GetTime() >= ui.next_surface_request))) {
                if (const auto queue = surface_queue.load()) {
                    EditorSurfaceRequest request{model.editor.generation, ui.surface_request.id + 1,
                                                 camera.origin(), ray, grid};
                    if (queue(request)) {
                        ui.surface_request = request;
                        ui.next_surface_request = ImGui::GetTime() + .016;
                        if (confirm)
                            ui.surface_confirm = true;
                    }
                }
            }
            const bool current =
                hit.id && hit.id == ui.surface_request.id && hit.generation == model.editor.generation;
            ui.surface_valid =
                (current || (previous_current && !ui.surface_confirm)) && hit.available && hit.hit;
            if (ui.surface_valid) {
                if (mouse_in || ui.surface_confirm) {
                    ui.position = hit.position;
                    ui.position[1] += ui.surface_offset;
                }
                wire_preview(draw, camera, screen_origin, ui.position, rotation(ui.angles), ui.scale, accent);
                if ((mouse_in || ui.surface_confirm) && can_edit && !ui.flying)
                    preview(ui, model,
                            ui.preview_token ? EditorPreviewAction::update
                                             : EditorPreviewAction::begin_place);
            } else {
                draw->AddText(plus(start, {16, 38}), muted,
                              current ? (hit.available ? "No surface hit. Aim at solid ground."
                                                       : "Game collision query unavailable.")
                                      : "Finding ground surface...");
            }
            if (ui.surface_confirm && current) {
                ui.surface_confirm = false;
                if (ui.surface_valid && ui.preview_token && preview(ui, model, EditorPreviewAction::commit))
                    ui.placing.clear();
                else if (!ui.surface_valid && ui.preview_token)
                    cancel_drag(ui, model);
            }
        } else if (mouse_in && !ui.flying) {
            ui.position = plane_hit(camera.origin(), ray, {0, ui.plane_height, 0}, {0, 1, 0})
                              .value_or(add(camera.origin(), mul(ray, ui.distance)));
            if (ui.snapping)
                for (auto &value : ui.position)
                    value = snapped(value, ui.grid);
        }
        if (!ui.surface_mode)
            wire_preview(draw, camera, screen_origin, ui.position, rotation(ui.angles), ui.scale, accent);
        if (!ui.surface_mode && mouse_in && !ui.flying && can_edit)
            preview(ui, model,
                    ui.preview_token ? EditorPreviewAction::update : EditorPreviewAction::begin_place);
        if (!ui.surface_mode && can_edit &&
            (dropped || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
            if (ui.preview_token && preview(ui, model, EditorPreviewAction::commit))
                ui.placing.clear();
        }
    }
    if (!ui.placing_set.empty() && !placing_object_set(ui, model.editor))
        ui.placing_set.clear(); // deleted, or the list was read again without it
    if (const auto *set = placing_object_set(ui, model.editor)) {
        // A saved set follows the cursor as wire cubes, one per object, and a click pastes it:
        // no live preview, since the game previews one object at a time.
        auto &io = ImGui::GetIO();
        if (hovered && !ui.flying && io.MouseWheel != 0) {
            const float step = ui.snapping ? ui.angle_snap : 15;
            ui.angles[1] = std::fmod(ui.angles[1] + (io.MouseWheel > 0 ? step : -step), 360.0f);
        }
        const bool clicked = can_edit && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool show = true, commit = false;
        if (ui.surface_mode) {
            const auto &hit = model.editor.surface;
            const bool previous_current =
                hit.id && hit.id == ui.surface_request.id && hit.generation == model.editor.generation;
            const float grid = ui.snapping ? ui.grid : 0;
            const bool changed = ui.surface_request.generation != model.editor.generation ||
                                 ui.surface_request.origin != camera.origin() ||
                                 ui.surface_request.direction != ray || ui.surface_request.grid != grid ||
                                 ui.surface_request.picking;
            if (mouse_in && !ui.flying && !ui.surface_confirm &&
                (clicked || !ui.surface_request.id ||
                 (changed && previous_current && ImGui::GetTime() >= ui.next_surface_request))) {
                if (const auto queue = surface_queue.load()) {
                    EditorSurfaceRequest request{model.editor.generation, ui.surface_request.id + 1,
                                                 camera.origin(), ray, grid};
                    if (queue(request)) {
                        ui.surface_request = request;
                        ui.next_surface_request = ImGui::GetTime() + .016;
                        if (clicked)
                            ui.surface_confirm = true;
                    }
                }
            }
            const bool current =
                hit.id && hit.id == ui.surface_request.id && hit.generation == model.editor.generation;
            ui.surface_valid =
                (current || (previous_current && !ui.surface_confirm)) && hit.available && hit.hit;
            show = ui.surface_valid;
            if (ui.surface_valid && (mouse_in || ui.surface_confirm)) {
                ui.position = hit.position;
                ui.position[1] += ui.surface_offset;
            }
            if (!ui.surface_valid)
                draw->AddText(plus(start, {16, 38}), muted,
                              current ? (hit.available ? "No surface hit. Aim at solid ground."
                                                       : "Game collision query unavailable.")
                                      : "Finding ground surface...");
            if (ui.surface_confirm && current) {
                ui.surface_confirm = false;
                commit = ui.surface_valid;
            }
        } else {
            if (mouse_in && !ui.flying) {
                ui.position = plane_hit(camera.origin(), ray, {0, ui.plane_height, 0}, {0, 1, 0})
                                  .value_or(add(camera.origin(), mul(ray, ui.distance)));
                if (ui.snapping)
                    for (auto &value : ui.position)
                        value = snapped(value, ui.grid);
            }
            commit = clicked;
        }
        if (show)
            for (const auto &object : placed_set_objects(ui, *set))
                wire_preview(draw, camera, screen_origin, object.position, object.rotation, object.scale, accent);
        if (commit && can_edit)
            place_set(ui, model.editor);
    }
    int hover_axis = -1;
    const auto *selected = selected_object(ui, model.editor);
    if (selected && can_edit && !ui.flying) {
        const auto p = project(camera, screen_origin, ui.position);
        const float gizmo_size = std::max(
            .4f, std::sqrt(dot(sub(ui.position, camera.origin()), sub(ui.position, camera.origin()))) * .12f);
        const auto q = rotation(ui.angles);
        for (unsigned axis = 0; axis < 3; ++axis) {
            Vec3 unit{};
            unit[axis] = 1;
            if (ui.local_axes)
                unit = rotated(unit, ui.dragging >= 0 ? ui.drag_rotation : q);
            if (ui.mode != ParkTransformMode::rotate) {
                const auto end = project(camera, screen_origin, add(ui.position, mul(unit, gizmo_size)));
                if (p && end) {
                    draw->AddLine(*p, *end, axes[axis], 3);
                    if (ui.mode == ParkTransformMode::scale)
                        draw->AddRectFilled(plus(*end, {-5, -5}), plus(*end, {5, 5}), axes[axis], 1);
                    else
                        draw->AddCircleFilled(*end, 5, axes[axis]);
                    constexpr const char *labels[]{"X", "Y", "Z"};
                    draw->AddText(plus(*end, {7, -6}), axes[axis], labels[axis]);
                    if (length({mouse.x - end->x, mouse.y - end->y}) < 11 && hovered)
                        hover_axis = static_cast<int>(axis);
                }
            } else {
                Vec3 u{};
                u[(axis + 1) % 3] = 1;
                Vec3 v{};
                v[(axis + 2) % 3] = 1;
                if (ui.local_axes) {
                    u = rotated(u, q);
                    v = rotated(v, q);
                }
                std::optional<ImVec2> previous;
                for (unsigned n = 0; n <= 64; ++n) {
                    const float angle = static_cast<float>(n) * 2 * pi / 64;
                    const auto point =
                        project(camera, screen_origin,
                                add(ui.position,
                                    mul(add(mul(u, std::cos(angle)), mul(v, std::sin(angle))), gizmo_size * .7f)));
                    if (point && previous) {
                        draw->AddLine(*previous, *point, axes[axis], 2);
                        if (hovered && line_distance(mouse, *previous, *point) < 7)
                            hover_axis = static_cast<int>(axis);
                    }
                    previous = point;
                }
            }
        }
        wire_preview(draw, camera, screen_origin, ui.position, q, ui.scale, accent);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover_axis >= 0) {
            capture_drag(ui, model.editor);
            Vec3 axis{};
            axis[static_cast<unsigned>(hover_axis)] = 1;
            if (ui.local_axes)
                axis = rotated(axis, q);
            ui.drag_axis = axis;
            ui.drag_start = ui.position;
            ui.drag_rotation = q;
            ui.drag_gizmo_scale = gizmo_size;
            if (ui.mode != ParkTransformMode::rotate) {
                if (const auto t = axis_parameter(camera.origin(), ray, ui.position, axis)) {
                    ui.drag_parameter = *t;
                    ui.dragging = hover_axis;
                }
            } else if (const auto hit = plane_hit(camera.origin(), ray, ui.position, axis)) {
                ui.drag_start = normalized(sub(*hit, ui.position));
                ui.dragging = hover_axis;
            }
            if (ui.dragging >= 0 && !preview(ui, model, EditorPreviewAction::begin_move))
                ui.dragging = -1;
        }
        if (ui.dragging >= 0) {
            if (ui.dragging == 3) {
                const auto &hit = model.editor.surface;
                if (hit.id == ui.surface_request.id && hit.generation == model.editor.generation &&
                    hit.available && hit.hit && !ui.surface_request.picking) {
                    if (!ui.free_anchor) {
                        ui.free_offset = sub(ui.drag_pivot, hit.position);
                        ui.free_anchor = true;
                    }
                    ui.position = add(hit.position, ui.free_offset);
                    if (ui.snapping) {
                        ui.position[0] = snapped(ui.position[0], ui.grid);
                        ui.position[2] = snapped(ui.position[2], ui.grid);
                    }
                }
            } else if (ui.mode == ParkTransformMode::move) {
                if (const auto t = axis_parameter(camera.origin(), ray, ui.drag_start, ui.drag_axis)) {
                    float delta = *t - ui.drag_parameter;
                    if (ui.snapping)
                        delta = snapped(delta, ui.grid);
                    ui.position = add(ui.drag_start, mul(ui.drag_axis, delta));
                }
            } else if (ui.mode == ParkTransformMode::scale) {
                if (const auto t = axis_parameter(camera.origin(), ray, ui.drag_start, ui.drag_axis)) {
                    const float delta = (*t - ui.drag_parameter) / std::max(ui.drag_gizmo_scale, .01f);
                    ui.scale = std::clamp(ui.drag_scale + delta, .01f, 100.0f);
                    if (ui.snapping)
                        ui.scale = std::clamp(snapped(ui.scale, ui.scale_snap), .01f, 100.0f);
                }
            } else if (const auto hit = plane_hit(camera.origin(), ray, ui.position, ui.drag_axis)) {
                const auto next = normalized(sub(*hit, ui.position));
                float angle =
                    std::atan2(dot(ui.drag_axis, cross(ui.drag_start, next)), dot(ui.drag_start, next)) *
                    180 / pi;
                if (ui.snapping)
                    angle = snapped(angle, ui.angle_snap);
                const float half = angle * pi / 360;
                const Quat a{ui.drag_axis[0] * std::sin(half), ui.drag_axis[1] * std::sin(half),
                             ui.drag_axis[2] * std::sin(half), std::cos(half)};
                const auto b = ui.drag_rotation;
                ui.angles = angles({a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]});
            }
            preview(ui, model, EditorPreviewAction::update);
            if (ui.dragging != 3 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ui.dragging = -1;
                preview(ui, model, EditorPreviewAction::commit);
            }
        }
    }
    std::uint64_t hovered_object{};
    float best = 16;
    for (const auto &object : model.editor.objects) {
        if (const auto p = project(camera, screen_origin, object.position)) {
            const bool chosen =
                object.id == ui.selected ||
                std::find(ui.selection.begin(), ui.selection.end(), object.id) != ui.selection.end();
            draw->AddCircleFilled(*p, chosen ? 5.0f : 3.0f, chosen ? accent : muted);
            const float d = length({mouse.x - p->x, mouse.y - p->y});
            if (d < best) {
                best = d;
                hovered_object = object.id;
            }
        }
    }
    if (ui.placing.empty() && ui.placing_set.empty() && !ui.flying && can_edit &&
        (ui.dragging < 0 || ui.dragging == 3)) {
        const bool clicked =
            hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover_axis < 0 && ui.dragging < 0;
        const bool picking = ui.dragging != 3;
        const bool releasing = !picking && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ui.free_release;
        const bool changed = ui.surface_request.origin != camera.origin() ||
                             ui.surface_request.direction != ray || ui.surface_request.picking != picking ||
                             ui.surface_request.generation != model.editor.generation;
        const bool previous_current = model.editor.surface.id == ui.surface_request.id &&
                                      model.editor.surface.generation == model.editor.generation;
        if ((mouse_in || releasing) && !ui.pick_confirm && !ui.free_release &&
            (clicked || releasing || !ui.surface_request.id ||
             (changed && previous_current && (picking || ui.free_anchor) &&
              ImGui::GetTime() >= ui.next_surface_request))) {
            if (const auto queue = surface_queue.load()) {
                EditorSurfaceRequest request{
                    model.editor.generation, ui.surface_request.id + 1, camera.origin(), ray, 0, picking};
                if (queue(request)) {
                    ui.surface_request = request;
                    ui.next_surface_request = ImGui::GetTime() + .016;
                    if (releasing)
                        ui.free_release = true;
                    if (clicked) {
                        ui.pick_confirm = true;
                        ui.pick_shift = ImGui::GetIO().KeyShift;
                    }
                }
            }
        }
        const auto &hit = model.editor.surface;
        const bool current = hit.id == ui.surface_request.id && hit.generation == model.editor.generation;
        if (ui.free_release && current) {
            ui.free_release = false;
            ui.dragging = -1;
            if (hit.available && hit.hit)
                preview(ui, model, EditorPreviewAction::commit);
            else
                cancel_drag(ui, model);
        }
        if (picking && current && hit.object)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ui.pick_confirm && current) {
            ui.pick_confirm = false;
            const auto id = hit.available ? hit.object : hovered_object;
            const auto object = std::find_if(model.editor.objects.begin(), model.editor.objects.end(),
                                             [&](const auto &value) { return value.id == id; });
            if (object != model.editor.objects.end()) {
                if (ui.pick_shift ||
                    std::find(ui.selection.begin(), ui.selection.end(), id) == ui.selection.end())
                    choose(ui, model.editor, *object, ui.pick_shift);
                if (!ui.pick_shift && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    capture_drag(ui, model.editor);
                    ui.free_anchor = false;
                    ui.free_release = false;
                    if (preview(ui, model, EditorPreviewAction::begin_move)) {
                        ui.dragging = 3;
                        if (const auto queue = surface_queue.load()) {
                            auto request = ui.surface_request;
                            ++request.id;
                            request.picking = false;
                            if (queue(request))
                                ui.surface_request = request;
                        }
                    }
                }
            } else if (!ui.pick_shift) {
                ui.selection.clear();
                ui.selected = 0;
            }
        }
    }
    draw->AddText(plus(start, {16, 14}), muted,
                  !ui.placing_set.empty() ? "Click to place the set    |    Wheel turns it    |    Escape to cancel"
                  : ui.placing.empty()    ? "T move / R rotate / Y scale  |  Shift-click adds / RMB flies"
                                          : "Click to place    |    Escape to cancel");
    if (ui.preview_token && can_edit)
        preview(ui, model, EditorPreviewAction::update);
    draw->PopClipRect();
}
} // namespace dingosdk::overlay::park_editor_detail
