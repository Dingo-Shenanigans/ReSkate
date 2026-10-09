#include "park_editor_internal.h"
#include <algorithm>

namespace dingosdk::overlay {
using namespace park_editor_detail;
namespace {
using theme::keycap;
// A tile like the menu's tabs: blue with black text when selected.
bool mode_tile(ImDrawList *draw, ImFont *font, const char *label, bool on, float k, unsigned seed) {
    const float text_size = 14 * k;
    const auto extent = font->CalcTextSizeA(text_size, FLT_MAX, 0, label);
    const ImVec2 size{extent.x + 28 * k, ImGui::GetFrameHeight()};
    const auto at = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##mode", size);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    skate_theme::rough_rect(draw, at, {at.x + size.x, at.y + size.y},
                            on ? blue : hovered ? skate_theme::tile_light : skate_theme::tile_grey, seed, k);
    draw->AddText(font, text_size, {at.x + 14 * k, at.y + (size.y - extent.y) * .5f},
                  on ? skate_theme::black : paper, label);
    return pressed;
}
void leave(ParkEditorUI &ui, const CallbacksV3 &callbacks, bool &visible, bool play) {
    if (ui.exit_pending || !callbacks.queue_debug)
        return;
    std::array<char, 512> result{};
    if (callbacks.queue_debug(callbacks.user, {DebugAction::set_park_editor, false}, result.data(),
                              result.size())) {
        ui.exit_pending = true;
        ui.exit_at = ImGui::GetTime();
        visible = !play;
        ui.flying = false;
        ui.dragging = -1;
        ui.placing.clear();
    }
    ui.feedback = result.data();
    ui.feedback_until = ImGui::GetTime() + 5;
}
} // namespace
void set_park_surface_queue(ParkSurfaceQueue queue) noexcept {
    surface_queue.store(queue);
}
void set_park_preview_queue(ParkPreviewQueue queue) noexcept {
    preview_queue.store(queue);
}
void set_park_selection_queue(ParkSelectionQueue queue) noexcept {
    selection_queue.store(queue);
}
void set_park_paste_queue(ParkPasteQueue queue) noexcept {
    paste_queue.store(queue);
}
bool draw_park_editor(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks, bool console_open,
                      bool exit_requested, bool &visible) {
    const auto &state = model.editor;
    auto &io = ImGui::GetIO();
    // Esc cancels a drag or placement in progress; with nothing in progress it
    // closes the editor and returns to the game. An open dialog closes itself.
    bool escape_closes = false;
    if (!console_open && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        const bool in_progress = ui.dragging >= 0 || !ui.placing.empty() || ui.preview_token || ui.surface_confirm ||
                                 ui.inspector_dirty;
        if (in_progress) {
            cancel_drag(ui, model);
            ui.placing.clear();
            ui.dragging = -1;
            ui.inspector_dirty = false;
            ui.draft_revision = 0;
            ui.surface_confirm = false;
        } else
            escape_closes = true;
    }
    if (ui.generation != state.generation) {
        ui.generation = state.generation;
        ui.selected = 0;
        ui.selection.clear();
        ui.paste_expected.clear();
        ui.paste_before.clear();
        ui.preview_token = 0;
        ui.pick_confirm = ui.asset_drag = false;
        ui.dragging = -1;
        ui.placing.clear();
        ui.pending = false;
        ui.inspector_dirty = false;
        ui.initialized = false;
        ui.surface_confirm = false;
        ui.surface_request = {};
    }
    if (!ui.initialized) {
        ui.plane_height = model.debug.skater_position_valid ? model.debug.skater_position[1]
                                                            : model.debug.camera_position[1] - 2;
        ui.initialized = true;
    }
    if (ui.exit_pending && ImGui::GetTime() > ui.exit_at + 3) {
        ui.exit_pending = false;
        ui.feedback = model.debug.status;
        ui.feedback_until = ImGui::GetTime() + 5;
    }
    if (ui.pending && !state.busy &&
        (state.revision != ui.queued_revision || state.status != ui.queued_status ||
         ImGui::GetTime() > ui.queued_at + 12))
        ui.pending = false;
    if (!ui.pending && !state.busy && !ui.paste_expected.empty())
        select_pasted(ui, state);
    if (ui.preview_token && (state.failed || (!state.busy && state.revision > ui.preview_revision))) {
        ui.preview_token = 0;
        ui.dragging = -1;
        ui.placing.clear();
        ui.free_release = ui.surface_confirm = false;
        refresh_selection(ui, state);
    }
    if ((ui.selected || !ui.selection.empty()) && !ui.inspector_dirty && ui.dragging < 0 && !ui.pending &&
        ui.draft_revision != state.revision)
        refresh_selection(ui, state);
    if (exit_requested || escape_closes) {
        if (state.busy || ui.pending || ui.preview_token) {
            ui.feedback = "Wait for the current edit before closing the editor.";
            ui.feedback_until = ImGui::GetTime() + 5;
        } else
            leave(ui, callbacks, visible, escape_closes); // Esc goes straight back to the game
    }
    // A server that has turned object scaling off shares everything at its own size: nothing is
    // resized here either, so what this player sees is what everyone else does.
    ui.scaling_allowed = model.multiplayer.object_scaling;
    if (!ui.scaling_allowed) {
        ui.scale = 1;
        if (ui.mode == ParkTransformMode::scale) ui.mode = ParkTransformMode::move;
    }
    namespace skate = dingosdk::skate_theme;
    const float k = std::clamp(ui.ui_scale, .5f, 3.0f);
    const auto P = [k](float value) { return value * k; };
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = k;
    const auto *main = ImGui::GetMainViewport();
    const auto origin = main->Pos, size = main->Size;
    const float left = std::clamp(size.x * .25f, P(250), P(400)),
                right = std::clamp(size.x * .23f, P(250), P(330)), top = P(104), bottom = P(40), gap = P(10);
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {P(9), P(6)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {P(8), P(6)});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0);
    const int colours = skate::push_widget_colours();
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("ReSkate Park Editor", nullptr, flags);
    auto *frame = ImGui::GetWindowDrawList();
    ImFont *title_font = font_or(ui.title_font), *bold_font = font_or(ui.bold_font);

    // ---- header band: brushed title, map, and the Test / Exit actions
    frame->AddRectFilled(origin, {origin.x + size.x, origin.y + top}, IM_COL32(10, 10, 11, 245));
    {
        const int start = frame->VtxBuffer.Size;
        const ImVec2 at{origin.x + P(20), origin.y + P(8)};
        frame->AddText(title_font, P(40), {at.x + P(2), at.y + P(3)}, IM_COL32(0, 0, 0, 160), "PARK EDITOR");
        frame->AddText(title_font, P(40), at, paper, "PARK EDITOR");
        const auto extent = title_font->CalcTextSizeA(P(40), FLT_MAX, 0, "PARK EDITOR");
        skate::rotate_since(frame, start, -3.0f, {at.x + extent.x * .5f, at.y + extent.y * .5f});
        frame->AddText(ImGui::GetFont(), P(14), {at.x + extent.x + P(18), at.y + P(20)}, muted,
                       state.map.empty() ? "Build your own park." : state.map.c_str());
    }
    const bool busy =
        state.busy || state.failed || ui.pending || ui.preview_token || ui.exit_pending || !state.available;
    ImGui::PushFont(bold_font);
    {
        const ImVec2 test{P(130), P(34)}, exit{P(96), P(34)};
        ImGui::SetCursorPos({size.x - P(20) - exit.x - P(8) - test.x, P(14)});
        ImGui::BeginDisabled(state.busy || ui.pending || ui.preview_token);
        skate::push_primary_button();
        if (ImGui::Button("TEST PARK", test))
            leave(ui, callbacks, visible, true);
        skate::pop_primary_button();
        ImGui::SameLine(0, P(8));
        if (ImGui::Button("EXIT", exit))
            leave(ui, callbacks, visible, false);
        ImGui::EndDisabled();
    }
    // ---- tool row: file, history, transform mode
    ImGui::SetCursorPos({P(20), P(62)});
    ImGui::BeginDisabled(busy);
    const auto project = std::find_if(state.park_mods.begin(), state.park_mods.end(),
                                      [&](const auto &mod) { return mod.folder == state.project; });
    const bool has_project = project != state.park_mods.end();
    const auto project_label = "PROJECT:  " + (has_project ? (project->title.empty() ? project->folder : project->title)
                                                           : std::string("UNSAVED")) + "##project";
    if (ImGui::Button(project_label.c_str())) {
        ui.project_selected = has_project ? project->folder : std::string();
        ui.project_open = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create, open or save a park mod.");
    ImGui::SameLine();
    if (ImGui::Button("SAVE")) {
        if (has_project)
            send(ui, model, callbacks, "mod-save", project->folder);
        else {
            ui.project_selected.clear();
            ui.project_open = true;
        }
    }
    ImGui::SameLine(0, P(20));
    ImGui::BeginDisabled(!state.can_undo);
    if (ImGui::Button("UNDO"))
        send(ui, model, callbacks, "undo");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.can_redo);
    if (ImGui::Button("REDO"))
        send(ui, model, callbacks, "redo");
    ImGui::EndDisabled();
    ImGui::SameLine(0, P(20));
    const std::array<std::pair<const char *, ParkTransformMode>, 3> modes{
        {{"MOVE  T", ParkTransformMode::move}, {"ROTATE  R", ParkTransformMode::rotate},
         {"SCALE  Y", ParkTransformMode::scale}}};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        if (i)
            ImGui::SameLine(0, P(6));
        if (modes[i].second == ParkTransformMode::scale && !ui.scaling_allowed) continue; // the server has it off
        if (mode_tile(frame, bold_font, modes[i].first, ui.mode == modes[i].second, k, static_cast<unsigned>(41 + i))) {
            ui.mode = modes[i].second;
            ui.dragging = -1;
        }
    }
    ImGui::PopFont();
    ImGui::SameLine(0, P(14));
    if (ui.selection.size() > 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("World axes");
    } else
        ImGui::Checkbox("Local axes", &ui.local_axes);
    ImGui::EndDisabled();

    // ---- dialogs
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {P(20), P(18)});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 18, 19, 252));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(4, 6, 9, 150));
    if (ui.project_open) {
        ImGui::OpenPopup("Park project");
        ui.project_open = false;
    }
    ImGui::SetNextWindowSize({std::min(P(860), size.x - P(40)), std::min(P(580), size.y - P(40))});
    ImGui::SetNextWindowPos({origin.x + size.x * .5f, origin.y + size.y * .5f}, ImGuiCond_Always, {.5f, .5f});
    if (ImGui::BeginPopupModal("Park project", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                            ImGuiWindowFlags_NoMove)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.WantTextInput)
            ImGui::CloseCurrentPopup();
        auto *draw = ImGui::GetWindowDrawList();
        const auto at = ImGui::GetWindowPos();
        draw->AddRectFilled(at, {at.x + ImGui::GetWindowWidth(), at.y + P(3)}, blue);
        ImGui::PushFont(font_or(ui.heading_font));
        ImGui::TextUnformatted("PARK PROJECT");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Parks are saved as mods in the Mods folder, so you can share them. Others install them "
                            "with the launcher and load them from Build > Park mods.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const auto *selected_mod = [&]() -> const EditorParkMod * {
            for (const auto &mod : state.park_mods)
                if (mod.folder == ui.project_selected)
                    return &mod;
            return nullptr;
        }();
        const bool creating = !selected_mod;
        // Fill the form from the selection once, then leave it to the user.
        const std::string fields_for = creating ? std::string("\n") : ui.project_selected;
        if (ui.project_fields_for != fields_for) {
            ui.project_fields_for = fields_for;
            const auto copy = [](auto &buffer, const std::string &text) {
                buffer.fill(0);
                std::copy_n(text.begin(), std::min(text.size(), buffer.size() - 1), buffer.begin());
            };
            copy(ui.mod_title, creating ? std::string() : selected_mod->title);
            copy(ui.mod_author, creating ? std::string() : selected_mod->author);
            copy(ui.mod_version, creating ? std::string("1.0") : selected_mod->version);
            copy(ui.mod_description, creating ? std::string() : selected_mod->description);
        }
        const float footer = ImGui::GetFrameHeightWithSpacing() + P(14);
        const float body = ImGui::GetContentRegionAvail().y - footer;
        const float list_width = P(300);

        // left: projects
        {
            const auto list_at = ImGui::GetCursorScreenPos();
            skate::rough_rect(draw, list_at, {list_at.x + list_width, list_at.y + body}, skate::tile, 61, k);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {P(10), P(10)});
            ImGui::BeginChild("##projects", {list_width, body}, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PushFont(bold_font);
            if (ImGui::Selectable("+  NEW PARK MOD", creating))
                ui.project_selected.clear();
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::TextDisabled("PARK MODS");
            if (state.park_mods.empty())
                ImGui::TextDisabled("None yet.");
            for (const auto &mod : state.park_mods) {
                ImGui::PushID(mod.folder.c_str());
                const bool current = mod.folder == state.project;
                const auto label = (current ? "> " : "") + (mod.title.empty() ? mod.folder : mod.title);
                if (ImGui::Selectable(label.c_str(), ui.project_selected == mod.folder))
                    ui.project_selected = mod.folder;
                std::string detail = mod.author.empty() ? std::string() : "by " + mod.author + "  /  ";
                detail += mod.has_map ? "park for " + state.map : "no park for " + state.map + " yet";
                if (!mod.enabled)
                    detail += "  /  disabled";
                ImGui::TextDisabled("  %s", detail.c_str());
                ImGui::PopID();
            }
            if (!state.legacy_parks.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("SAVED BEFORE PARK MODS");
                for (const auto &name : state.legacy_parks) {
                    ImGui::PushID(name.c_str());
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - P(76));
                    ImGui::BeginDisabled(busy);
                    if (ImGui::Button("Convert", {P(76), 0}))
                        send(ui, model, callbacks, "mod-convert", name);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Copy this park into a new park mod. The old file is kept.");
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        // right: details and actions
        ImGui::SameLine(0, P(16));
        ImGui::BeginChild("##project-details", {0, body});
        ImGui::PushFont(font_or(ui.heading_font));
        ImGui::TextUnformatted(creating ? "NEW PARK MOD"
                                        : (selected_mod->title.empty() ? selected_mod->folder : selected_mod->title).c_str());
        ImGui::PopFont();
        if (creating)
            ImGui::TextDisabled("Saves the current layout on %s into a new mod.", state.map.c_str());
        else {
            std::string maps;
            for (const auto &map : selected_mod->maps)
                maps += (maps.empty() ? "" : ", ") + map;
            ImGui::TextDisabled("Mods\\%s  /  parks: %s", selected_mod->folder.c_str(), maps.empty() ? "none" : maps.c_str());
        }
        ImGui::Spacing();
        const auto field = [&](const char *label, auto &buffer, const char *hint) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, buffer.data(), buffer.size());
        };
        field("Name", ui.mod_title, "My skatepark");
        field("Author", ui.mod_author, "Your name");
        field("Version", ui.mod_version, "1.0");
        ImGui::TextUnformatted("Description");
        ImGui::InputTextMultiline("##description", ui.mod_description.data(), ui.mod_description.size(),
                                  {-1, P(90)});
        ImGui::Spacing();
        std::string description = ui.mod_description.data();
        std::replace(description.begin(), description.end(), '\n', ' ');
        const std::vector<std::string> texts{ui.mod_title.data(), ui.mod_author.data(), ui.mod_version.data(),
                                             description};
        const bool named = ui.mod_title[0] != 0;
        ImGui::BeginDisabled(busy);
        ImGui::PushFont(bold_font);
        if (creating) {
            ImGui::BeginDisabled(!named);
            skate::push_primary_button();
            if (ImGui::Button("CREATE AND SAVE LAYOUT", {-1, P(38)}) &&
                send(ui, model, callbacks, "mod-create", {}, texts))
                ImGui::CloseCurrentPopup();
            skate::pop_primary_button();
            ImGui::EndDisabled();
        } else {
            const bool can_open = selected_mod->has_map && state.can_load_parks;
            ImGui::BeginDisabled(!can_open);
            skate::push_primary_button();
            if (ImGui::Button("OPEN IN EDITOR", {-1, P(38)}) &&
                send(ui, model, callbacks, "mod-open", selected_mod->folder))
                ImGui::CloseCurrentPopup();
            skate::pop_primary_button();
            ImGui::EndDisabled();
            if (!can_open && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", !state.can_load_parks ? "In multiplayer only the host can load park mods."
                                                              : "This mod has no park for this map yet.");
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
            if (ImGui::Button("SAVE LAYOUT HERE", {half, 0}) &&
                send(ui, model, callbacks, "mod-save", selected_mod->folder))
                ImGui::CloseCurrentPopup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(selected_mod->has_map ? "Replace this mod's park for %s with the current layout."
                                                        : "Add the current layout to this mod as its park for %s.",
                                  state.map.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!named);
            if (ImGui::Button("SAVE DETAILS", {-1, 0}))
                send(ui, model, callbacks, "mod-details", selected_mod->folder, texts);
            ImGui::EndDisabled();
        }
        ImGui::PopFont();
        ImGui::EndDisabled();
        ImGui::EndChild();

        // footer
        ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Clear layout"))
            send(ui, model, callbacks, "new");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove every object and start an unsaved layout. Undo brings it back.");
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
            send(ui, model, callbacks, "mods-refresh");
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - P(120));
        if (ImGui::Button("Close", {P(120), 0}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // ---- side panels on rough-cut tiles
    const float content_height = std::max(P(100), size.y - top - bottom - gap * 2);
    const auto panel = [&](const char *id, float x, float width, unsigned seed, auto &&body) {
        const ImVec2 at{origin.x + x, origin.y + top + gap};
        skate::rough_rect(frame, at, {at.x + width, at.y + content_height}, IM_COL32(20, 20, 21, 242), seed, k);
        ImGui::SetCursorPos({x, top + gap});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {P(14), P(12)});
        ImGui::BeginChild(id, {width, content_height}, ImGuiChildFlags_AlwaysUseWindowPadding);
        body();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    };
    panel("library-panel", gap, left - gap, 52, [&] { library(ui, model); });
    panel("inspector-panel", size.x - right, right - gap, 53, [&] { inspector(ui, model, callbacks); });
    ImGui::SetCursorPos({left, top});
    ImGui::BeginChild("viewport-panel", {std::max(20.0f, size.x - left - right), size.y - top - bottom});
    const bool blocked =
        console_open || ui.exit_pending ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        io.AppFocusLost;
    viewport(ui, model, origin, size, blocked);
    const auto highlight = blocked || ui.exit_pending ? std::vector<std::uint64_t>{} : ui.selection;
    if (highlight != ui.highlighted || ImGui::GetTime() >= ui.selection_heartbeat) {
        if (const auto queue = selection_queue.load(); queue && queue({state.generation, highlight})) {
            ui.highlighted = highlight;
            ui.selection_heartbeat = ImGui::GetTime() + .25;
        }
    }
    ImGui::EndChild();

    // ---- status bar: the latest message and keycap hints
    {
        const ImVec2 at{origin.x, origin.y + size.y - bottom};
        frame->AddRectFilled(at, {at.x + size.x, at.y + bottom}, IM_COL32(10, 10, 11, 245));
        frame->AddRectFilled(at, {at.x + P(4), at.y + bottom}, blue);
        const std::string status = ui.pending ? "Waiting for the game to apply the edit..."
                                   : !ui.feedback.empty() && ImGui::GetTime() < ui.feedback_until ? ui.feedback
                                                                                                  : state.status;
        const auto text = status.empty() ? std::string("Current layout autosaves. Save a named park to keep a reusable copy.")
                                         : status;
        // Hints on the right; the message is clipped to the space left of them.
        float hints = 0;
        const std::array<std::pair<const char *, const char *>, 4> keys{
            {{"RMB", "Fly"}, {"WASD", "Move"}, {"CTRL Z", "Undo"}, {"ESC", "Close"}}};
        std::array<float, 4> widths{};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            widths[i] = bold_font->CalcTextSizeA(P(12), FLT_MAX, 0, keys[i].first).x +
                        bold_font->CalcTextSizeA(P(14), FLT_MAX, 0, keys[i].second).x + P(20);
            hints += widths[i] + P(18);
        }
        const bool show_hints = size.x - hints > P(360);
        float x = at.x + size.x - P(16) - (show_hints ? hints - P(18) : 0);
        if (show_hints)
            for (std::size_t i = 0; i < keys.size(); ++i) {
                keycap(frame, bold_font, k, {x, at.y + (bottom - P(20)) * .5f}, keys[i].first, keys[i].second);
                x += widths[i] + P(18);
            }
        const float message_right = at.x + size.x - P(16) - (show_hints ? hints + P(8) : 0);
        frame->PushClipRect({at.x + P(16), at.y}, {message_right, at.y + bottom}, true);
        frame->AddText(ImGui::GetFont(), P(15), {at.x + P(18), at.y + (bottom - P(15)) * .5f},
                       status.empty() ? muted : paper, text.c_str());
        frame->PopClipRect();
        ImGui::SetCursorPos({0, size.y - bottom});
        ImGui::InvisibleButton("##status", {std::max(1.0f, message_right - origin.x), bottom});
        if (!status.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", status.c_str());
    }
    if (!console_open && !io.WantTextInput && !ImGui::IsAnyItemActive() && !blocked) {
        if (!busy) {
            if (!io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
                if (ImGui::IsKeyPressed(ImGuiKey_T, false))
                    ui.mode = ParkTransformMode::move;
                if (ImGui::IsKeyPressed(ImGuiKey_R, false))
                    ui.mode = ParkTransformMode::rotate;
                if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && ui.scaling_allowed)
                    ui.mode = ParkTransformMode::scale;
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
                copy_selection(ui, state);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                paste_objects(ui, state);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                if (!state.project.empty())
                    send(ui, model, callbacks, "mod-save", state.project);
                else {
                    ui.project_selected.clear();
                    ui.project_open = true;
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
                send(ui, model, callbacks, io.KeyShift ? "redo" : "undo");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
                send(ui, model, callbacks, "redo");
            if (ui.selected && ui.selection.size() <= 1 && ImGui::IsKeyPressed(ImGuiKey_Delete))
                send(ui, model, callbacks, "delete");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(9);
    io.FontGlobalScale = restore_font_scale;
    return ui.flying && !blocked && !io.WantTextInput && ui.dragging < 0;
}
} // namespace dingosdk::overlay
