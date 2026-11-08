#include "park_editor_internal.h"
#include <algorithm>

// The PARK PROJECT dialog: create, open or save a park mod. Both layouts open the same one.
namespace dingosdk::overlay::park_editor_detail {
namespace {
const EditorParkMod *find_mod(const ParkEditorModel &state, const std::string &folder) {
    for (const auto &mod : state.park_mods)
        if (mod.folder == folder)
            return &mod;
    return nullptr;
}
} // namespace
void project_dialog(ParkEditorUI &ui, EditorFrame &frame) {
    const auto &model = frame.model;
    const auto &callbacks = frame.callbacks;
    const auto &state = model.editor;
    const float k = frame.k;
    const auto P = [k](float value) { return value * k; };
    const bool busy = frame.busy;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {P(22), P(18)});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0)); // the rough tile below is its background
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(4, 6, 9, 150));
    if (ui.project_open) {
        ImGui::OpenPopup("Park project");
        ui.project_open = false;
    }
    const ImVec2 size{std::min(P(860), frame.size.x - P(40)), std::min(P(580), frame.size.y - P(40))};
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowPos({frame.origin.x + frame.size.x * .5f, frame.origin.y + frame.size.y * .5f}, ImGuiCond_Always,
                            {.5f, .5f});
    if (ImGui::BeginPopupModal("Park project", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput)
            ImGui::CloseCurrentPopup();
        auto *draw = ImGui::GetWindowDrawList();
        const auto at = ImGui::GetWindowPos();
        draw->PushClipRect(plus(at, {-2, -2}), plus(at, {size.x + 2, size.y + 2}), false);
        theme::tile_rect(draw, at, plus(at, size), IM_COL32(18, 18, 19, 252), 60, k);
        draw->PopClipRect();
        draw->AddRectFilled(at, {at.x + size.x, at.y + P(3)}, blue);
        // The brushed title, tilted like the editor's own.
        {
            ImFont *title = font_or(ui.title_font);
            const auto from = ImGui::GetCursorScreenPos();
            const int start = draw->VtxBuffer.Size;
            const auto extent = title->CalcTextSizeA(P(34), FLT_MAX, 0, "PARK PROJECT");
            draw->AddText(title, P(34), plus(from, {P(2), P(3)}), IM_COL32(0, 0, 0, 160), "PARK PROJECT");
            draw->AddText(title, P(34), from, paper, "PARK PROJECT");
            skate_theme::rotate_since(draw, start, -2.0f, plus(from, {extent.x * .5f, extent.y * .5f}));
            ImGui::Dummy({extent.x, extent.y + P(4)});
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Parks are saved as mods in the Mods folder, so you can share them. Others install them "
                            "with the launcher and load them from Build > Park mods.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const auto *selected_mod = find_mod(state, ui.project_selected);
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
        const float footer = P(34) + P(18);
        const float body = ImGui::GetContentRegionAvail().y - footer;
        const float list_width = std::min(P(300), ImGui::GetContentRegionAvail().x * .4f);

        // left: projects
        {
            const auto list_at = ImGui::GetCursorScreenPos();
            theme::tile_rect(draw, list_at, {list_at.x + list_width, list_at.y + body}, skate_theme::tile, 61, k);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {P(10), P(10)});
            ImGui::BeginChild("##projects", {list_width, body}, ImGuiChildFlags_AlwaysUseWindowPadding);
            if (chip(ui, "+  NEW PARK MOD##new", creating ? Chip::white : Chip::tile, P(32), 62, true,
                     ImGui::GetContentRegionAvail().x))
                ui.project_selected.clear();
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
            ImGui::TextDisabled("%s", label);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, buffer.data(), buffer.size());
        };
        field("Name", ui.mod_title, "My skatepark");
        field("Author", ui.mod_author, "Your name");
        field("Version", ui.mod_version, "1.0");
        ImGui::TextDisabled("Description");
        ImGui::InputTextMultiline("##description", ui.mod_description.data(), ui.mod_description.size(), {-1, P(90)});
        ImGui::Spacing();
        std::string description = ui.mod_description.data();
        std::replace(description.begin(), description.end(), '\n', ' ');
        const std::vector<std::string> texts{ui.mod_title.data(), ui.mod_author.data(), ui.mod_version.data(),
                                             description};
        const bool named = ui.mod_title[0] != 0;
        const float wide = ImGui::GetContentRegionAvail().x;
        ImGui::BeginDisabled(busy);
        if (creating) {
            if (chip(ui, "CREATE AND SAVE LAYOUT##create", Chip::blue, P(38), 63, named, wide) &&
                send(ui, model, callbacks, "mod-create", {}, texts))
                ImGui::CloseCurrentPopup();
        } else {
            const bool can_open = selected_mod->has_map && state.can_load_parks;
            if (chip(ui, "OPEN IN EDITOR##open", Chip::blue, P(38), 64, can_open, wide) &&
                send(ui, model, callbacks, "mod-open", selected_mod->folder))
                ImGui::CloseCurrentPopup();
            if (!can_open && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", !state.can_load_parks ? "In multiplayer only the host can load park mods."
                                                              : "This mod has no park for this map yet.");
            const float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
            if (chip(ui, "SAVE LAYOUT HERE##save-here", Chip::tile, P(32), 65, true, half) &&
                send(ui, model, callbacks, "mod-save", selected_mod->folder))
                ImGui::CloseCurrentPopup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(selected_mod->has_map ? "Replace this mod's park for %s with the current layout."
                                                        : "Add the current layout to this mod as its park for %s.",
                                  state.map.c_str());
            ImGui::SameLine();
            if (chip(ui, "SAVE DETAILS##details", Chip::tile, P(32), 66, named, half))
                send(ui, model, callbacks, "mod-details", selected_mod->folder, texts);
        }
        ImGui::EndDisabled();
        ImGui::EndChild();

        // footer
        ImGui::Dummy({0, P(10)});
        ImGui::BeginDisabled(busy);
        if (chip(ui, "CLEAR LAYOUT##clear", Chip::danger, P(32), 67))
            send(ui, model, callbacks, "new");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove every object and start an unsaved layout. Undo brings it back.");
        ImGui::SameLine();
        if (chip(ui, "REFRESH##refresh", Chip::plain, P(32), 68))
            send(ui, model, callbacks, "mods-refresh");
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - P(120));
        if (chip(ui, "CLOSE##close", Chip::tile, P(32), 69, true, P(120)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}
} // namespace dingosdk::overlay::park_editor_detail
