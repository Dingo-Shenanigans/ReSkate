#pragma once
#include "Extension/Objects/ParkEditor/editor_math.h"
#include "overlay.h"
#include <imgui.h>

namespace dingosdk::overlay {
enum class ParkTransformMode { move, rotate, scale };
struct ParkEditorUI {
    std::uint64_t generation{}, selected{}, draft_revision{}, queued_revision{};
    std::array<char, 128> search{};
    // PARK PROJECT dialog: the selected mod folder ("" = a new mod) and the
    // form, filled from the selection whenever it changes.
    bool project_open{};
    std::string project_selected, project_fields_for;
    std::array<char, 65> mod_title{}, mod_author{};
    std::array<char, 33> mod_version{};
    std::array<char, 513> mod_description{};
    std::string category, placing, feedback, queued_status;
    editor::Vec3 position{}, angles{}, drag_start{}, drag_axis{};
    editor::Quat drag_rotation{0, 0, 0, 1};
    bool scaling_allowed{true}; // off: the server shares objects at their own size, so none is resized here
    float scale{1}, grid{.5f}, angle_snap{15}, scale_snap{.1f}, plane_height{}, distance{10},
        drag_parameter{}, drag_angle{}, drag_scale{1}, drag_gizmo_scale{1};
    ParkTransformMode mode{ParkTransformMode::move};
    bool snapping{true}, local_axes{}, pending{}, inspector_dirty{}, initialized{}, flying{},
        exit_pending{};
    int dragging{-1};
    double queued_at{}, exit_at{}, feedback_until{};
    EditorSurfaceRequest surface_request;
    bool surface_mode{true}, surface_confirm{}, surface_valid{};
    float surface_offset{};
    double next_surface_request{};
    std::vector<std::uint64_t> selection;
    std::vector<EditorTransform> drag_objects;
    editor::Vec3 drag_pivot{}, free_offset{};
    editor::Quat drag_orientation{0, 0, 0, 1};
    std::uint64_t preview_token{}, preview_sequence{}, preview_revision{}, next_preview_token{};
    double preview_heartbeat{};
    editor::Vec3 preview_position{}, preview_angles{};
    float preview_scale{1};
    bool preview_place{}, asset_drag{}, pick_confirm{}, pick_shift{}, free_anchor{}, free_release{};
    double selection_heartbeat{};
    std::vector<std::uint64_t> highlighted;
    std::vector<EditorObject> clipboard, paste_expected;
    std::vector<std::uint64_t> paste_before;
    unsigned paste_serial{};
    // Look shared with the ReSkate menu, set by the overlay before drawing.
    // Null fonts fall back to the current font (the UI tests pass none).
    ImFont *title_font{}, *bold_font{}, *heading_font{};
    float ui_scale{1};
    std::string close_key{"INS"};
};
// Render thread only. Returns whether the viewport currently owns flight input.
bool draw_park_editor(ParkEditorUI &, const Model &, const CallbacksV3 &, bool console_open,
                      bool exit_requested, bool &menu_visible);
} // namespace dingosdk::overlay
