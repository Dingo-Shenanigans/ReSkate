#include "Engine/Core/Json/json.h"
#include "Extension/UI/Overlay/park_editor.h"
#include "Extension/UI/Overlay/park_editor_internal.h"
#include "Extension/UI/Overlay/park_previews.h"
#include "Engine/Vfs/content_catalogs.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <imgui_internal.h>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dingosdk;
using namespace dingosdk::overlay;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void preview_checks() {
    ImFontAtlas atlas;
    atlas.AddFontDefault();
    vfs::ItemThumbnail thumbnail{"own_bk_test", {128, 128, {}}};
    for (unsigned i = 0; i < 128 * 128; ++i)
        thumbnail.image.rgba.insert(thumbnail.image.rgba.end(), {35, 75, 150, 255});
    check(load_park_previews(atlas, std::span(&thumbnail, 1)) == 1, "Valid thumbnail was rejected");
    const auto *rect = atlas.GetCustomRectByIndex(0);
    const auto offset = (static_cast<std::size_t>(rect->Y) * atlas.TexWidth + rect->X) * 4;
    const auto *pixels = reinterpret_cast<const unsigned char *>(atlas.TexPixelsRGBA32);
    check(rect->IsPacked() && pixels[offset] == 35 && pixels[offset + 2] == 150,
          "Thumbnail pixels were not copied into the uploaded ImGui atlas");
    const int rectangles = atlas.CustomRects.Size;
    auto truncated = thumbnail;
    truncated.image.rgba.pop_back();
    check(load_park_previews(atlas, std::span(&truncated, 1)) == 0 && atlas.CustomRects.Size == rectangles,
          "Truncated thumbnail data modified the font atlas");
    auto renamed = thumbnail;
    renamed.item = "Own_BK_Test";
    check(load_park_previews(atlas, std::span(&renamed, 1)) == 0 && atlas.CustomRects.Size == rectangles,
          "An invalid thumbnail key reached the atlas allocator");
    const std::array twice{thumbnail, thumbnail};
    check(load_park_previews(atlas, twice) == 0 && atlas.CustomRects.Size == rectangles,
          "Duplicate thumbnail keys reached the atlas allocator");
    check(load_park_previews(atlas, std::chrono::milliseconds(0)) == 0,
          "Thumbnails were loaded without a background read");
    clear_park_previews();
}
struct Fixture {
    inline static Fixture *active{};
    std::optional<EditorSurfaceRequest> probe;
    ParkEditorUI ui;
    Model model;
    CallbacksV3 callbacks;
    bool visible = true;
    bool flight{};
    std::vector<std::string> commands;
    std::vector<EditorPreviewRequest> previews;
    std::vector<EditorSelectionRequest> selections;
    std::vector<EditorPasteRequest> pastes;
    std::vector<EditorObjectSetRequest> set_requests;
    std::vector<EditorDeleteRequest> deletes;
    Fixture() {
        active = this;
        set_park_delete_queue([](const EditorDeleteRequest &request) {
            active->deletes.push_back(request);
            active->model.editor.busy = true;
            ++active->model.editor.revision;
            return true;
        });
        set_park_object_set_queue([](const EditorObjectSetRequest &request) {
            active->set_requests.push_back(request);
            return true;
        });
        set_park_paste_queue([](const EditorPasteRequest &request) {
            active->pastes.push_back(request);
            active->model.editor.busy = true;
            ++active->model.editor.revision;
            return true;
        });
        set_park_preview_queue([](const EditorPreviewRequest &request) {
            active->previews.push_back(request);
            auto &model = active->model.editor;
            if (request.action == EditorPreviewAction::begin_move ||
                request.action == EditorPreviewAction::begin_place) {
                model.busy = true;
                model.preview_token = request.token;
                ++model.revision;
            } else if (request.action == EditorPreviewAction::commit ||
                       request.action == EditorPreviewAction::cancel) {
                model.busy = false;
                model.preview_token = 0;
                ++model.revision;
            }
            return true;
        });
        set_park_selection_queue([](const EditorSelectionRequest &request) {
            active->selections.push_back(request);
            return true;
        });
        set_park_surface_queue([](const EditorSurfaceRequest &request) {
            active->probe = request;
            return true;
        });
        callbacks.user = this;
        callbacks.queue_console_command = [](void *context, const char *command, char *, std::size_t) {
            static_cast<Fixture *>(context)->commands.emplace_back(command);
            return true;
        };
        model.debug.park_editor = true;
        model.debug.camera_transform_valid = true;
        model.debug.camera_fov = 60;
        model.debug.camera_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 4, 16, 1};
        model.debug.skater_position_valid = true;
        model.debug.skater_position = {0, 0, 0};
        model.editor.available = true;
        model.editor.map = "bam";
        model.editor.generation = 3;
        model.editor.revision = 7;
        auto assets = std::make_shared<std::vector<EditorAsset>>();
        for (const auto &category : {"Ramps", "Rails", "Quarter Pipes", "Props"})
            for (unsigned i = 0; i < 30; ++i)
                assets->push_back({"own_bk_" + std::string(category) + std::to_string(i),
                                   std::string(category) + " piece " + std::to_string(i + 1), category});
        model.editor.assets = assets;
        model.editor.objects = {{1, "own_bk_Ramps0", {0, 0, 0}, {0, 0, 0, 1}, true},
                                {2, "own_bk_Rails0", {4, 0, 0}, {0, 0, 0, 1}, true}};
        model.editor.park_mods = {{"Warehouse lines", "Warehouse lines", "Zee", "1.0", "", true, true, {"bam"}},
                                  {"Rooftop session", "Rooftop session", "", "", "", true, false, {"grom"}}};
        model.editor.can_load_parks = true;
    }
    void frame(bool console = false, bool text_input = false) {
        ImGui::NewFrame();
        if (text_input)
            ImGui::GetIO().WantTextInput = true;
        const auto snapshot = model;
        flight = draw_park_editor(ui, snapshot, callbacks, console, false, visible);
        ImGui::Render();
        check(ImGui::GetDrawData()->TotalVtxCount > 0, "Editor produced no UI");
    }
    std::size_t count(EditorPreviewAction action) const {
        return static_cast<std::size_t>(std::count_if(
            previews.begin(), previews.end(), [&](const auto &request) { return request.action == action; }));
    }
};
// Small deterministic software rasterizer for layout inspection; no game or GPU.
void capture(const std::filesystem::path &path, const ImDrawData &data, unsigned char *atlas, int atlas_width,
             int atlas_height) {
    const int width = static_cast<int>(data.DisplaySize.x), height = static_cast<int>(data.DisplaySize.y);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto n = (static_cast<std::size_t>(y) * width + x) * 3;
            pixels[n] = 37;
            pixels[n + 1] = 43;
            pixels[n + 2] = 48;
        }
    for (const auto *list : data.CmdLists)
        for (const auto &command : list->CmdBuffer) {
            if (command.UserCallback)
                continue;
            const int min_clip_x = std::max(0, static_cast<int>(command.ClipRect.x)),
                      max_clip_x = std::min(width, static_cast<int>(command.ClipRect.z));
            const int min_clip_y = std::max(0, static_cast<int>(command.ClipRect.y)),
                      max_clip_y = std::min(height, static_cast<int>(command.ClipRect.w));
            for (unsigned index = 0; index + 2 < command.ElemCount; index += 3) {
                const auto &a = list->VtxBuffer[static_cast<int>(
                    command.VtxOffset + list->IdxBuffer[static_cast<int>(command.IdxOffset + index)])];
                const auto &b = list->VtxBuffer[static_cast<int>(
                    command.VtxOffset + list->IdxBuffer[static_cast<int>(command.IdxOffset + index + 1)])];
                const auto &c = list->VtxBuffer[static_cast<int>(
                    command.VtxOffset + list->IdxBuffer[static_cast<int>(command.IdxOffset + index + 2)])];
                const float area =
                    (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
                if (std::abs(area) < .0001f)
                    continue;
                const int x0 = std::max(min_clip_x,
                                        static_cast<int>(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})))),
                          x1 = std::min(max_clip_x,
                                        static_cast<int>(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))));
                const int y0 = std::max(min_clip_y,
                                        static_cast<int>(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})))),
                          y1 = std::min(max_clip_y,
                                        static_cast<int>(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const float px = static_cast<float>(x) + .5f, py = static_cast<float>(y) + .5f;
                        const float wa =
                            ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
                        const float wb = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) /
                                         area,
                                    wc = 1 - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0)
                            continue;
                        const float u = a.uv.x * wa + b.uv.x * wb + c.uv.x * wc,
                                    v = a.uv.y * wa + b.uv.y * wb + c.uv.y * wc;
                        const int tx = std::clamp(static_cast<int>(u * static_cast<float>(atlas_width)), 0,
                                                  atlas_width - 1),
                                  ty = std::clamp(static_cast<int>(v * static_cast<float>(atlas_height)), 0,
                                                  atlas_height - 1);
                        const auto tex = static_cast<std::size_t>(ty * atlas_width + tx) * 4;
                        const auto component = [&](unsigned shift) {
                            return static_cast<float>((a.col >> shift) & 255) * wa +
                                   static_cast<float>((b.col >> shift) & 255) * wb +
                                   static_cast<float>((c.col >> shift) & 255) * wc;
                        };
                        const float alpha = component(24) * static_cast<float>(atlas[tex + 3]) / (255 * 255);
                        const auto out = (static_cast<std::size_t>(y) * width + x) * 3;
                        for (unsigned channel = 0; channel < 3; ++channel)
                            pixels[out + channel] = static_cast<unsigned char>(
                                std::clamp(component(channel * 8) * static_cast<float>(atlas[tex + channel]) /
                                                   255 * alpha +
                                               static_cast<float>(pixels[out + channel]) * (1 - alpha),
                                           0.0f, 255.0f));
                    }
            }
        }
    std::ofstream stream(path, std::ios::binary);
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    stream.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}
} // namespace
int main(int argc, char **argv) {
    try {
        ImGui::CreateContext();
        preview_checks();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime = 1.0f / 60;
        io.DisplaySize = {1280, 720};
        ImFontConfig font_config;
        font_config.SizePixels = 17;
        io.Fonts->AddFontDefault(&font_config);
        // Optional: the installed game's folder, to lay out every real thumbnail.
        if (argc > 2 && std::filesystem::exists(argv[2])) {
            start_park_previews(argv[2]);
            const auto loaded = load_park_previews(*io.Fonts, std::chrono::seconds(30));
            check(loaded == 518, "The game's thumbnails were not loaded completely");
        }
        unsigned char *atlas{};
        int atlas_width{}, atlas_height{};
        io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
        io.Fonts->SetTexID(1);
        Fixture fixture;
        fixture.frame();
        fixture.frame();
        const auto press_mode = [&](ImGuiKey key, ParkTransformMode expected) {
            io.AddKeyEvent(key, true);
            fixture.frame();
            io.AddKeyEvent(key, false);
            fixture.frame();
            check(fixture.ui.mode == expected, "Transform-mode keybind selected the wrong gizmo");
        };
        press_mode(ImGuiKey_R, ParkTransformMode::rotate);
        press_mode(ImGuiKey_Y, ParkTransformMode::scale);
        press_mode(ImGuiKey_T, ParkTransformMode::move);
        const auto click_card = [&](float x) {
            io.AddMousePosEvent(x, 320);
            fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            fixture.frame();
        };
        click_card(210);
        check(fixture.ui.placing == "own_bk_Ramps1", "Second grid column did not select its own asset");
        const std::string filter = "Rails piece 8";
        std::copy(filter.begin(), filter.end(), fixture.ui.search.begin());
        fixture.frame();
        click_card(60);
        check(fixture.ui.placing == "own_bk_Rails7",
              "Grid search did not preserve the filtered asset identity");
        fixture.ui.search = {};
        fixture.ui.category = "Props";
        fixture.frame();
        click_card(60);
        check(fixture.ui.placing == "own_bk_Props0", "Grid category filter selected the wrong object");
        fixture.ui.category.clear();
        fixture.ui.placing.clear();
        fixture.frame();
        io.AddMousePosEvent(640, 360);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        fixture.frame();
        check(fixture.flight, "RMB inside viewport did not grant flight input");
        fixture.frame(true);
        check(!fixture.flight, "Console failed to inhibit editor flight");
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        fixture.frame();
        io.AddMousePosEvent(90, 200);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        fixture.frame();
        check(!fixture.flight, "RMB over library enabled camera motion");
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        fixture.frame();
        fixture.ui.selected = 1;
        fixture.ui.position = {1, 2, 3};
        fixture.ui.inspector_dirty = true;
        fixture.ui.dragging = 0;
        io.AddFocusEvent(false);
        fixture.frame();
        check(fixture.ui.dragging == -1 && fixture.ui.position == fixture.model.editor.objects[0].position,
              "Focus loss did not cancel transform drag");
        io.AddFocusEvent(true);
        fixture.frame();
        fixture.ui.selected = 2;
        fixture.ui.pending = true;
        fixture.ui.placing = "own_bk_Ramps0";
        fixture.model.editor.generation++;
        fixture.frame();
        check(!fixture.ui.selected && !fixture.ui.pending && fixture.ui.placing.empty(),
              "World change retained a stale selection or pending placement");
        fixture.ui.selected = 1;
        fixture.frame();
        io.AddMousePosEvent(717, 516);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        check(fixture.ui.dragging == 0, "X translation handle did not begin a drag");
        io.AddMousePosEvent(760, 516);
        fixture.frame();
        fixture.frame();
        check(fixture.count(EditorPreviewAction::update) > 0 &&
                  fixture.count(EditorPreviewAction::commit) == 0,
              "Held drag did not send live updates or committed before release");
        check(fixture.ui.position[0] > 0, "X drag did not change the transform preview");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        check(fixture.count(EditorPreviewAction::commit) == 1 && fixture.commands.empty() &&
                  fixture.ui.pending && fixture.ui.dragging == -1,
              "Releasing a drag did not submit exactly one move");
        fixture.frame();
        check(fixture.count(EditorPreviewAction::commit) == 1, "Released drag submitted a duplicate move");
        fixture.model.editor.revision++;
        fixture.frame();
        io.AddMousePosEvent(717, 516);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        check(fixture.ui.dragging == 0, "Second drag did not begin");
        io.AddMousePosEvent(760, 516);
        fixture.frame();
        io.AddKeyEvent(ImGuiKey_Escape, true);
        fixture.frame();
        check(fixture.ui.dragging == -1 && fixture.ui.position == fixture.model.editor.objects[0].position,
              "Escape did not discard the drag preview");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        fixture.frame();
        check(fixture.count(EditorPreviewAction::commit) == 1 &&
                  fixture.count(EditorPreviewAction::cancel) == 1,
              "Cancelled drag committed instead of restoring");
        fixture.ui.selected = 0;
        fixture.ui.selection.clear();
        fixture.ui.placing = "own_bk_Ramps0";
        io.AddMousePosEvent(640, 420);
        fixture.frame();
        check(fixture.probe.has_value() && fixture.count(EditorPreviewAction::begin_place) == 0,
              "Surface placement did not request real collision before creating an object");
        const auto old_probe = *fixture.probe;
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        check(fixture.ui.surface_confirm, "Placement click did not wait for its collision result");
        fixture.model.editor.surface = {old_probe.generation, old_probe.id, true, true, {90, 90, 90}};
        fixture.frame();
        check(fixture.count(EditorPreviewAction::begin_place) == 0,
              "Stale cursor collision result placed an object");
        fixture.model.editor.surface = {
            fixture.probe->generation, fixture.probe->id, true, true, {4, 2.3f, 6}};
        fixture.frame();
        check(fixture.count(EditorPreviewAction::begin_place) == 1 &&
                  fixture.count(EditorPreviewAction::commit) == 2 &&
                  std::abs(fixture.ui.position[1] - 2.3f) < .001f,
              "Surface placement did not use the real hit height");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        check(fixture.count(EditorPreviewAction::begin_place) == 1,
              "Surface placement submitted a duplicate create");
        fixture.model.editor.revision++;
        fixture.frame();
        fixture.ui.placing = "own_bk_Ramps0";
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, false, {}};
        fixture.frame();
        check(fixture.count(EditorPreviewAction::begin_place) == 1 && !fixture.ui.surface_confirm &&
                  !fixture.ui.surface_valid,
              "Missed collision silently placed on the fake plane");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        fixture.ui.placing.clear();
        fixture.ui.selection = {1};
        fixture.ui.selected = 1;
        fixture.ui.draft_revision = 0;
        fixture.frame();
        fixture.ui.mode = ParkTransformMode::scale;
        io.AddMousePosEvent(717, 516);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        check(fixture.ui.dragging == 0, "X scale handle did not begin a drag");
        io.AddMousePosEvent(760, 516);
        fixture.frame();
        check(fixture.ui.scale > 1 && !fixture.previews.back().transforms.empty() &&
                  fixture.previews.back().transforms[0].scale > 1,
              "Scale gizmo did not submit a live uniform-scale transform");
        const auto scale_commits = fixture.count(EditorPreviewAction::commit);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        check(fixture.count(EditorPreviewAction::commit) == scale_commits + 1,
              "Scale gizmo release did not commit exactly once");
        fixture.model.editor.revision++;
        fixture.frame();
        fixture.ui.mode = ParkTransformMode::move;
        fixture.ui.selected = 0;
        fixture.ui.selection.clear();
        fixture.frame();
        const auto select_hit = [&](std::uint64_t id, bool shift, bool release) {
            io.AddKeyEvent(ImGuiMod_Shift, shift);
            io.AddMousePosEvent(680, 516); // On the X shaft, away from its tip: body clicks must move freely.
            fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            fixture.frame();
            check(fixture.ui.pick_confirm && fixture.probe->picking,
                  "Object body click did not query collision");
            fixture.model.editor.surface = {
                fixture.probe->generation, fixture.probe->id, true, true, {4, 0, 0}, 100, id};
            fixture.frame();
            if (release) {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                fixture.frame();
            }
        };
        select_hit(1, true, true);
        select_hit(2, true, true);
        check(fixture.ui.selection.size() == 2 && fixture.ui.position == editor::Vec3{2, 0, 0} &&
                  fixture.selections.back().objects == fixture.ui.selection,
              "Shift-click did not select and highlight a group around its shared pivot");
        select_hit(2, true, true);
        check(fixture.ui.selection == std::vector<std::uint64_t>{1},
              "Shift-click did not remove a group member");
        select_hit(2, true, true);
        const auto committed_before_free = fixture.count(EditorPreviewAction::commit);
        select_hit(1, false, false);
        check(fixture.ui.dragging == 3 && fixture.previews.back().transforms.size() == 2 &&
                  !fixture.probe->picking,
              "Dragging a selected object's body did not begin a free group move");
        fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {4, 0, 0}};
        fixture.frame();
        check(fixture.ui.free_anchor, "Free drag did not preserve its initial grab offset");
        io.AddMousePosEvent(790, 430);
        fixture.frame();
        fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {7, 2, 1}};
        fixture.frame();
        check(fixture.previews.back().transforms[0].position == editor::Vec3{3, 2, 1} &&
                  fixture.previews.back().transforms[1].position == editor::Vec3{7, 2, 1},
              "Free dragging did not preserve group spacing and surface height");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        check(fixture.ui.free_release && fixture.count(EditorPreviewAction::commit) == committed_before_free,
              "Free release committed an old collision result");
        fixture.model.editor.surface = {
            fixture.probe->generation, fixture.probe->id, true, true, {8, 2.5f, 2}};
        fixture.frame();
        check(fixture.count(EditorPreviewAction::commit) == committed_before_free + 1 &&
                  fixture.previews.back().transforms[0].position == editor::Vec3{4, 2.5f, 2},
              "Free release failed to use its final ground hit");
        fixture.frame();
        // Group rotation must orbit the shared pivot as well as rotate each body.
        fixture.ui.mode = ParkTransformMode::rotate;
        fixture.ui.local_axes = true; // These objects are unrotated: the group's local axes are the world's.
        const editor::Camera camera{fixture.model.debug.camera_transform, 60, 1280, 720};
        const editor::Vec3 pivot{2, 0, 0};
        const float radius =
            std::sqrt(editor::dot(editor::sub(pivot, camera.origin()), editor::sub(pivot, camera.origin()))) *
            .12f * .7f;
        const auto ring_start = camera.project(editor::add(pivot, {radius * .7071f, radius * .7071f, 0}));
        const auto ring_end = camera.project(editor::add(pivot, {-radius * .7071f, radius * .7071f, 0}));
        io.AddMousePosEvent((*ring_start)[0], (*ring_start)[1]);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        check(fixture.ui.dragging == 2, "Group rotation ring did not start a drag");
        io.AddMousePosEvent((*ring_end)[0], (*ring_end)[1]);
        fixture.frame();
        const auto &rotated_group = fixture.previews.back().transforms;
        check(rotated_group.size() == 2 && std::abs(rotated_group[0].position[0] - 2) < .001f &&
                  std::abs(rotated_group[0].position[1] + 2) < .001f &&
                  std::abs(rotated_group[1].position[1] - 2) < .001f,
              "Group rotation changed orientations without orbiting the shared pivot");
        io.AddKeyEvent(ImGuiKey_Escape, true);
        fixture.frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        fixture.ui.mode = ParkTransformMode::move;
        // Asset enters the viewport while the mouse is still held.
        fixture.ui.selection.clear();
        fixture.ui.selected = 0;
        fixture.ui.placing = "own_bk_Ramps0";
        fixture.ui.surface_request = {};
        const auto spawned_previews = fixture.count(EditorPreviewAction::begin_place);
        io.AddMousePosEvent(740, 420);
        fixture.frame();
        fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {1, 2, 3}};
        fixture.frame();
        check(fixture.count(EditorPreviewAction::begin_place) == spawned_previews + 1 &&
                  fixture.ui.preview_token,
              "Asset did not become a live native preview before placement");
        io.AddMousePosEvent(780, 440);
        fixture.frame();
        fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {4, 5, 6}};
        fixture.frame();
        check(fixture.previews.back().position == editor::Vec3{4, 5, 6} &&
                  fixture.count(EditorPreviewAction::begin_place) == spawned_previews + 1,
              "Live asset did not follow ground without spawning another copy");
        fixture.frame(true);
        check(!fixture.ui.preview_token && fixture.previews.back().action == EditorPreviewAction::cancel,
              "Console/focus inhibition left a live preview behind");
        fixture.ui.placing.clear();
        fixture.frame();
        // Exercise ImGui's actual source/target delivery while holding the mouse.
        const auto dragdrop_creates = fixture.count(EditorPreviewAction::begin_place);
        // Inside the first library card (below the panel heading, search and filters).
        io.AddMousePosEvent(100, 340);
        fixture.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.frame();
        io.AddMousePosEvent(130, 345);
        fixture.frame();
        check(fixture.ui.asset_drag, "Library row did not start an ImGui drag payload");
        io.AddMousePosEvent(700, 420);
        fixture.frame();
        fixture.frame();
        check(!fixture.ui.placing.empty() && fixture.probe,
              "Viewport did not accept the asset payload before release");
        fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {6, 1, 2}};
        fixture.frame();
        check(fixture.count(EditorPreviewAction::begin_place) == dragdrop_creates + 1 &&
                  fixture.ui.preview_token,
              "Dragging a library row did not spawn an object before mouse release");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.frame();
        check(fixture.ui.surface_confirm, "Asset payload delivery did not request the final surface");
        fixture.model.editor.surface = {
            fixture.probe->generation, fixture.probe->id, true, true, {7, 1.5f, 3}};
        fixture.frame();
        check(fixture.ui.placing.empty() && fixture.previews.back().action == EditorPreviewAction::commit &&
                  fixture.previews.back().position == editor::Vec3{7, 1.5f, 3} &&
                  fixture.count(EditorPreviewAction::begin_place) == dragdrop_creates + 1,
              "Asset drop respawned the preview or lost its final position");
        fixture.frame();
        const auto directory = std::filesystem::absolute(argc > 1 ? argv[1] : "build/editor-preview");
        std::filesystem::create_directories(directory);
        fixture.ui.selection = {1, 2};
        fixture.ui.selected = 2;
        fixture.frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_C, true);
        fixture.frame();
        check(fixture.ui.clipboard.size() == 2 && fixture.pastes.empty(),
              "Ctrl+C did not copy the selection without creating objects");
        io.AddKeyEvent(ImGuiKey_C, false);
        fixture.frame();
        const auto copied = fixture.ui.clipboard;
        fixture.model.editor.objects[0].position[0] += 12;
        io.AddKeyEvent(ImGuiKey_V, true);
        fixture.frame(false, true);
        check(fixture.pastes.empty(), "Ctrl+V in a text field pasted game objects");
        io.AddKeyEvent(ImGuiKey_V, false);
        fixture.frame();
        io.AddKeyEvent(ImGuiKey_V, true);
        fixture.frame();
        check(fixture.pastes.size() == 1 && fixture.pastes.back().objects.size() == 2,
              "Ctrl+V did not paste the complete copied group");
        const auto pasted = fixture.pastes.back().objects;
        for (std::size_t i = 0; i < copied.size(); ++i)
            check(pasted[i].item == copied[i].item && pasted[i].rotation == copied[i].rotation &&
                      pasted[i].scale == copied[i].scale &&
                      pasted[i].position ==
                          editor::add(copied[i].position, {fixture.ui.grid, 0, fixture.ui.grid}),
                  "Clipboard did not preserve the copied snapshot, spacing, or rotation");
        for (unsigned i = 0; i < 30; ++i)
            fixture.frame();
        check(fixture.pastes.size() == 1, "Holding Ctrl+V repeatedly created objects");
        io.AddKeyEvent(ImGuiKey_V, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        for (auto object : pasted) {
            object.id = 100 + fixture.model.editor.objects.size();
            object.spawned = true;
            fixture.model.editor.objects.push_back(object);
        }
        fixture.model.editor.busy = false;
        ++fixture.model.editor.revision;
        fixture.frame();
        check(fixture.ui.selection == std::vector<std::uint64_t>{102, 103},
              "Completed paste did not select the newly created objects for group movement");
        ++fixture.model.editor.generation;
        fixture.frame();
        check(fixture.ui.clipboard.size() == 2 && fixture.ui.paste_expected.empty(),
              "Level change lost the clipboard or retained a stale paste selection");
        fixture.model.editor.objects.resize(2);
        fixture.model.editor.objects[0].position = {0, 0, 0};
        fixture.ui.selection.clear();
        {
            // The Delete key removes a whole multi-selection in one request (one undo step).
            fixture.ui.selection = {1, 2};
            fixture.ui.selected = 2;
            io.AddMousePosEvent(700, 420);
            io.AddKeyEvent(ImGuiKey_Delete, true);
            fixture.frame();
            io.AddKeyEvent(ImGuiKey_Delete, false);
            fixture.frame();
            check(fixture.deletes.size() == 1 && fixture.deletes[0].objects == std::vector<std::uint64_t>{1, 2} &&
                      fixture.ui.selection.empty(),
                  "Delete with several objects selected did not delete them all");
            fixture.model.editor.busy = false;
            ++fixture.model.editor.revision;
            fixture.frame();
        }
        {
            // Saved object sets: saving sends the selected objects; placing follows the surface
            // as wire cubes, turns with the wheel and pastes the whole set once, with no preview.
            using namespace park_editor_detail;
            fixture.ui.library_sets = true;
            fixture.frame();
            check(!save_set(fixture.ui, fixture.model.editor) && fixture.set_requests.empty(),
                  "A set was saved with nothing selected");
            fixture.ui.selection = {1, 2};
            fixture.ui.selected = 2;
            check(!save_set(fixture.ui, fixture.model.editor) && fixture.set_requests.empty(),
                  "A set was saved without a name");
            const std::string name = "  Combo ";
            std::copy(name.begin(), name.end(), fixture.ui.set_name.begin());
            check(save_set(fixture.ui, fixture.model.editor) && fixture.set_requests.size() == 1 &&
                      fixture.set_requests[0].action == EditorObjectSetAction::save &&
                      fixture.set_requests[0].name == "Combo" && fixture.set_requests[0].objects.size() == 2 &&
                      fixture.set_requests[0].objects[1].position == fixture.model.editor.objects[1].position &&
                      fixture.ui.set_name[0] == 0,
                  "Saving a set did not send the trimmed name and the selected objects");
            auto sets = std::make_shared<std::vector<EditorObjectSet>>();
            sets->push_back({"Combo", "bam",
                             {{1, "own_bk_Ramps0", {-1, 0, 0}, {0, 0, 0, 1}, true, 1},
                              {2, "own_bk_Ramps1", {1, .5f, 0}, {0, 0, 0, 1}, true, 2}}});
            fixture.model.editor.object_sets = sets;
            fixture.frame();
            begin_set_placement(fixture.ui, fixture.model, "Combo");
            check(fixture.ui.placing_set == "Combo" && fixture.ui.selection.empty() && fixture.ui.placing.empty(),
                  "Placing a set kept the selection or a single-object placement");
            const auto previews_before = fixture.previews.size();
            const auto pastes_before = fixture.pastes.size();
            io.AddMousePosEvent(700, 420);
            fixture.frame();
            check(fixture.probe.has_value(), "Set placement did not ask for the surface under the cursor");
            fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {10, 2, 5}};
            fixture.frame();
            check(fixture.ui.position == editor::Vec3{10, 2, 5}, "Set placement did not follow the surface");
            io.AddMouseWheelEvent(0, 1);
            fixture.frame();
            check(std::abs(fixture.ui.angles[1] - fixture.ui.angle_snap) < .001f,
                  "The mouse wheel did not turn the set by the angle step");
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            fixture.frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            check(fixture.pastes.size() == pastes_before && fixture.ui.surface_confirm,
                  "Set placement pasted before the clicked surface came back");
            fixture.model.editor.surface = {fixture.probe->generation, fixture.probe->id, true, true, {10, 2, 5}};
            fixture.frame();
            check(fixture.pastes.size() == pastes_before + 1 && fixture.pastes.back().objects.size() == 2 &&
                      fixture.ui.placing_set.empty(),
                  "Clicking did not paste the whole set once");
            check(fixture.previews.size() == previews_before, "Set placement sent a single-object live preview");
            {
                // Once the game has the set, it is selected with the move gizmo.
                fixture.ui.mode = ParkTransformMode::rotate;
                const auto layout = fixture.model.editor.objects;
                for (auto object : fixture.pastes.back().objects) {
                    object.id = 500 + fixture.model.editor.objects.size();
                    object.spawned = false; // a lost world link must not stop the selection
                    fixture.model.editor.objects.push_back(object);
                }
                fixture.model.editor.busy = false;
                ++fixture.model.editor.revision;
                fixture.frame();
                check(fixture.ui.selection.size() == 2 && fixture.ui.mode == ParkTransformMode::move,
                      "A placed set was not selected with the move gizmo");
                // Its own orientation is the rotation it was placed with: the local axes turn with it.
                check(std::abs(fixture.ui.angles[1] - fixture.ui.angle_snap) < .01f &&
                          std::abs(fixture.ui.angles[0]) < .01f && std::abs(fixture.ui.angles[2]) < .01f,
                      "A placed set did not take the rotation it was placed with as its own");
                // Moving it along its local X: every object goes along the set's turned X axis.
                fixture.ui.local_axes = true;
                park_editor_detail::capture_drag(fixture.ui, fixture.model.editor);
                const auto local_x = editor::rotated({1, 0, 0}, editor::rotation(fixture.ui.angles));
                const auto before_move = fixture.ui.drag_objects;
                fixture.ui.position = editor::add(fixture.ui.position, editor::mul(local_x, 2));
                check(park_editor_detail::preview(fixture.ui, fixture.model, EditorPreviewAction::begin_move),
                      "The placed set did not start a group move");
                const auto &moved = fixture.previews.back().transforms;
                for (std::size_t i = 0; i < moved.size(); ++i)
                    for (unsigned axis = 0; axis < 3; ++axis)
                        check(std::abs(moved[i].position[axis] - before_move[i].position[axis] - 2 * local_x[axis]) < .001f &&
                                  moved[i].rotation == before_move[i].rotation,
                              "Moving the set along its local axis turned it or went another way");
                park_editor_detail::preview(fixture.ui, fixture.model, EditorPreviewAction::cancel);
                fixture.ui.local_axes = false;
                fixture.model.editor.objects = layout;
                fixture.ui.selection.clear();
                fixture.ui.selected = 0;
                fixture.model.editor.busy = true;
            }
            const auto q = editor::rotation({0, fixture.ui.angle_snap, 0});
            const auto &placed = fixture.pastes.back().objects;
            for (std::size_t i = 0; i < 2; ++i) {
                const auto expected = editor::add({10, 2, 5}, editor::rotated((*sets)[0].objects[i].position, q));
                for (unsigned axis = 0; axis < 3; ++axis)
                    check(std::abs(placed[i].position[axis] - expected[axis]) < .001f,
                          "A set object was not placed relative to the set's anchor");
                check(placed[i].item == (*sets)[0].objects[i].item && placed[i].scale == (*sets)[0].objects[i].scale,
                      "A set object lost its item or scale");
            }
            fixture.model.editor.busy = false;
            ++fixture.model.editor.revision;
            fixture.frame();
            begin_set_placement(fixture.ui, fixture.model, "Combo");
            check(delete_set(fixture.ui, fixture.model.editor, "Combo") && fixture.ui.placing_set.empty() &&
                      fixture.set_requests.back().action == EditorObjectSetAction::remove,
                  "Deleting the set being placed did not stop its placement");
            {
                // Duplicate copies every selected object with its rotation and scale, as one paste.
                auto &objects = fixture.model.editor.objects;
                objects[0].rotation = editor::rotation({0, 30, 0});
                objects[1].scale = 3;
                fixture.ui.selection = {objects[0].id, objects[1].id};
                fixture.ui.selected = objects[1].id;
                fixture.frame();
                const auto pastes = fixture.pastes.size();
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_D, true);
                fixture.frame();
                io.AddKeyEvent(ImGuiKey_D, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
                fixture.frame();
                check(fixture.pastes.size() == pastes + 1 && fixture.pastes.back().objects.size() == 2,
                      "Ctrl+D did not duplicate every selected object in one paste");
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto &copy = fixture.pastes.back().objects[i];
                    check(copy.item == objects[i].item && copy.rotation == objects[i].rotation &&
                              copy.scale == objects[i].scale &&
                              copy.position == editor::add(objects[i].position, {fixture.ui.grid, 0, 0}),
                          "A duplicate lost its rotation or scale, or was not one grid step along X");
                }
                fixture.model.editor.busy = false;
                ++fixture.model.editor.revision;
                fixture.ui.paste_expected.clear();
                fixture.ui.pending = false;
                fixture.frame();
                // On a server with object scaling off, the copies are made at normal size.
                fixture.model.multiplayer.object_scaling = false;
                fixture.ui.selection = {objects[0].id, objects[1].id};
                fixture.ui.selected = objects[1].id;
                fixture.frame();
                check(park_editor_detail::duplicate_selection(fixture.ui, fixture.model.editor) &&
                          fixture.pastes.back().objects[1].scale == 1 &&
                          fixture.pastes.back().objects[0].rotation == objects[0].rotation,
                      "With scaling off, a duplicate kept its size or lost its rotation");
                fixture.model.multiplayer.object_scaling = true;
                fixture.model.editor.busy = false;
                ++fixture.model.editor.revision;
                fixture.ui.paste_expected.clear();
                fixture.ui.pending = false;
                objects[0].rotation = {0, 0, 0, 1};
                objects[1].scale = 1;
                fixture.frame();
            }
            fixture.model.editor.object_sets.reset();
            fixture.ui.library_sets = false;
            fixture.ui.paste_expected.clear();
            fixture.ui.selection.clear();
            fixture.ui.angles = {};
            fixture.frame();
        }
        // With the game's thumbnails loaded, show real items, titled from the
        // installed content cache when there is one.
        if (argc > 2 && std::filesystem::exists(argv[2])) {
            const auto &items = content_cache::catalogs().items;
            auto assets = std::make_shared<std::vector<EditorAsset>>();
            for (const auto &[key, value] : items.items()) {
                ParkPreviewImage image;
                if (park_preview_image(key, image))
                    assets->push_back({key, value.value("title", key), "Build Kit"});
            }
            if (!assets->empty())
                fixture.model.editor.assets = assets;
        }
        for (const ImVec2 size : std::array{ImVec2{1280, 720}, ImVec2{1920, 1080}, ImVec2{1024, 768}}) {
            io.DisplaySize = size;
            fixture.ui.selected = 1;
            fixture.ui.position = {0, 0, 0};
            fixture.ui.angles = {};
            fixture.ui.mode = ParkTransformMode::move;
            fixture.frame();
            fixture.frame();
            capture(directory / (std::to_string(static_cast<int>(size.x)) + ".ppm"), *ImGui::GetDrawData(),
                    atlas, atlas_width, atlas_height);
        }
        clear_park_previews();
        ImGui::DestroyContext();
        std::cout
            << "Park editor UI layout, input focus, transform drag and world-transition tests passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
