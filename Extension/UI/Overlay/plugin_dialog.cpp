#include "overlay_internal.h"
#include "Extension/Plugins/plugin_game.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include <commdlg.h>
#include <fstream>
#pragma comment(lib, "comdlg32.lib")

// reskate.ui.dialog: a plugin's modal over the game (a name field, a list of rows with buttons, a
// file picker). Plugins write it from their thread and the overlay draws it on the render thread,
// so the dialog lives under a lock; what the player does goes to the plugin as kit events, posted
// outside it. The file picker blocks the render thread while it is up: the game is behind a modal
// anyway.
namespace dingosdk::overlay {
namespace {
constexpr std::size_t most_file_text = 64u << 10;
struct Dialog {
    std::string plugin;
    plugins::DialogSpec spec;
    std::array<char, 257> text{}; // the field as typed
    std::string given;            // the field's text as the plugin last gave it
    int selected{};               // the row last picked, from 1
    bool fresh = true;            // focus goes to the field or the first row once
};
struct Dialogs {
    std::mutex mutex;
    std::optional<Dialog> open;
};
Dialogs& dialogs() {
    static auto* d = new Dialogs; // the render thread may draw during shutdown
    return *d;
}
void set_text(Dialog& d, const std::string& text) {
    d.text = {};
    const auto most = std::min<std::size_t>({text.size(), d.spec.field.max, d.text.size() - 1});
    std::copy_n(text.data(), most, d.text.data());
    d.given = text;
}
// "*.json;*.txt" as GetOpenFileNameW wants it: description, pattern, then All files.
std::wstring filter_of(const std::string& patterns) {
    std::wstring wide(patterns.begin(), patterns.end());
    std::wstring filter = L"Files (" + wide + L")";
    filter.push_back(L'\0');
    filter += wide;
    filter.push_back(L'\0');
    filter += L"All files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}
std::optional<std::filesystem::path> choose_file(const std::string& patterns, const std::string& title) {
    std::array<wchar_t, 32768> filename{};
    const auto filter = filter_of(patterns);
    const std::wstring caption(title.begin(), title.end());
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = detail::state().window.load();
    picker.lpstrFilter = filter.c_str();
    picker.lpstrFile = filename.data();
    picker.nMaxFile = static_cast<DWORD>(filename.size());
    picker.lpstrTitle = caption.c_str();
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&picker)) return std::nullopt;
    return std::filesystem::path(filename.data());
}
struct Answer {
    std::string plugin, id, button, text;
    int row{};
    std::string file_text;
};
void send(const Answer& a) {
    throwdowns::kit::post_to(a.plugin, "dialog", {}, {a.id, a.button, a.text, std::to_string(a.row), a.file_text});
}
} // namespace

std::string open_plugin_dialog(std::string_view plugin, const plugins::DialogSpec& spec) {
    auto& d = dialogs();
    {
        std::lock_guard lock(d.mutex);
        if (d.open && d.open->plugin != plugin) return d.open->plugin + "'s dialog is open";
        if (d.open && d.open->spec.id == spec.id) {
            auto& open = *d.open;
            const bool new_text = spec.field.text != open.given;
            open.spec = spec;
            if (new_text) set_text(open, spec.field.text);
            if (open.selected > static_cast<int>(spec.rows.size())) open.selected = 0;
        } else {
            Dialog fresh;
            fresh.plugin = std::string(plugin);
            fresh.spec = spec;
            set_text(fresh, spec.field.text);
            d.open = std::move(fresh);
        }
    }
    // The menu's input capture, which the route library's dialog used (input_capture.cpp).
    set_plugin_dialog_active(true);
    return {};
}
void close_plugin_dialog(std::string_view plugin, std::string_view id) {
    auto& d = dialogs();
    bool closed = false;
    {
        std::lock_guard lock(d.mutex);
        if (d.open && d.open->plugin == plugin && (id.empty() || d.open->spec.id == id)) {
            d.open.reset();
            closed = true;
        }
    }
    if (closed) set_plugin_dialog_active(false);
}

namespace detail {
void draw_plugin_dialog() {
    auto& d = dialogs();
    std::vector<Answer> answers;
    std::optional<Answer> pick; // a file to choose, once the lock is let go
    std::string pick_filter, pick_title;
    bool closed = false;
    {
        std::lock_guard lock(d.mutex);
        if (!d.open) return;
        auto& dialog = *d.open;
        const auto& spec = dialog.spec;
        // Kept on each frame: another caller of the shared capture may have let it go.
        state().plugin_dialog_visible.store(true);
        const auto answer = [&](std::string button, int row) {
            answers.push_back({dialog.plugin, spec.id, std::move(button), dialog.text.data(), row, {}});
        };
        const auto* viewport = ImGui::GetMainViewport();
        const float scale = std::clamp(std::min(viewport->WorkSize.x / 1600.f, viewport->WorkSize.y / 900.f), .65f, 1.4f);
        const float width = std::min(800.f * scale, viewport->WorkSize.x - 32.f);
        const float height = std::min((spec.list ? 640.f : 430.f) * scale, viewport->WorkSize.y - 32.f);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f, viewport->WorkPos.y + viewport->WorkSize.y * .5f), ImGuiCond_Always,
                                ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.f * scale, 24.f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f * scale, 14.f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f * scale);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.055f, .06f, .07f, .97f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.72f, .19f, .2f, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.42f, .1f, .12f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.63f, .16f, .18f, 1));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(.44f, .12f, .14f, 1));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.36f, .14f, .16f, 1));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(.53f, .15f, .17f, 1));
        ImGui::PushFont(state().menu.body ? state().menu.body : ImGui::GetFont());
        const auto window = "##plugin-dialog-" + dialog.plugin + "-" + spec.id;
        if (ImGui::Begin(window.c_str(), nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::SetWindowFontScale(1.1f * scale);
            if (!spec.title.empty()) {
                ImGui::PushFont(state().menu.heading ? state().menu.heading : ImGui::GetFont());
                ImGui::TextUnformatted(spec.title.c_str());
                ImGui::PopFont();
                ImGui::Separator();
            }
            if (!spec.message.empty()) ImGui::TextWrapped("%s", spec.message.c_str());
            bool entered = false;
            if (spec.field.present) {
                ImGui::SetNextItemWidth(-1);
                if (dialog.fresh) ImGui::SetKeyboardFocusHere();
                const auto size = std::min<std::size_t>(spec.field.max + 1, dialog.text.size());
                entered = ImGui::InputText("##plugin-dialog-field", dialog.text.data(), size, ImGuiInputTextFlags_EnterReturnsTrue);
            }
            if (spec.list) {
                const float footer = 110.f * scale;
                if (ImGui::BeginChild("##plugin-dialog-rows", ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - footer)), true)) {
                    if (spec.rows.empty() && !spec.empty.empty()) ImGui::TextWrapped("%s", spec.empty.c_str());
                    for (std::size_t i = 0; i < spec.rows.size(); ++i) {
                        const auto& row = spec.rows[i];
                        const int number = static_cast<int>(i + 1);
                        ImGui::PushID(number);
                        const float buttons_width = static_cast<float>(row.buttons.size()) * 112.f * scale;
                        const float row_height = (row.sub.empty() ? 52.f : 88.f) * scale;
                        const auto at = ImGui::GetCursorScreenPos();
                        const float row_width = std::max(80.f, ImGui::GetContentRegionAvail().x - buttons_width);
                        if (dialog.fresh && i == 0 && !spec.field.present) ImGui::SetKeyboardFocusHere();
                        if (ImGui::Selectable("##row", dialog.selected == number, ImGuiSelectableFlags_None, ImVec2(row_width, row_height))) {
                            dialog.selected = number;
                            answer("row", number);
                        }
                        auto* draw = ImGui::GetWindowDrawList();
                        draw->PushClipRect(at, ImVec2(at.x + row_width, at.y + row_height), true);
                        const auto ink = ImGui::GetColorU32(ImGuiCol_Text);
                        draw->AddText(ImVec2(at.x + 8.f * scale, at.y + 5.f * scale), ink, row.text.c_str());
                        if (!row.sub.empty()) draw->AddText(ImVec2(at.x + 8.f * scale, at.y + 44.f * scale), ImGui::GetColorU32(ImGuiCol_TextDisabled), row.sub.c_str());
                        draw->PopClipRect();
                        for (const auto& name : row.buttons) {
                            ImGui::SameLine();
                            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, at.y + (row_height - 40.f * scale) * .5f));
                            if (ImGui::Button(name.c_str(), ImVec2(100.f * scale, 40.f * scale))) {
                                dialog.selected = number;
                                answer(name, number);
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
            bool first = true;
            for (const auto& button : spec.buttons) {
                if (!first) ImGui::SameLine();
                first = false;
                ImGui::PushID(button.id.c_str());
                if (ImGui::Button(button.label.c_str(), ImVec2(180.f * scale, 44.f * scale))) answer(button.id, dialog.selected);
                ImGui::PopID();
            }
            // Enter in the field is the first button.
            if (entered && !spec.buttons.empty()) answer(spec.buttons.front().id, dialog.selected);
            if (spec.file.present) {
                if (!first) ImGui::SameLine();
                if (ImGui::Button(spec.file.label.c_str(), ImVec2(200.f * scale, 44.f * scale))) {
                    pick = Answer{dialog.plugin, spec.id, "file", dialog.text.data(), dialog.selected, {}};
                    pick_filter = spec.file.filter;
                    pick_title = spec.title.empty() ? spec.file.label : spec.title;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                answer("cancel", dialog.selected);
                closed = true;
            }
            dialog.fresh = false;
        }
        ImGui::End();
        ImGui::PopFont();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(3);
        if (closed) d.open.reset();
    }
    if (closed) set_plugin_dialog_active(false);
    for (const auto& a : answers) send(a);
    if (!pick) return;
    const auto file = choose_file(pick_filter, pick_title);
    if (!file) return; // the player shut the picker: nothing to say
    std::error_code error;
    const auto size = std::filesystem::file_size(*file, error);
    if (error || size > most_file_text) {
        pick->button = "file_error";
        pick->text = error ? "the file cannot be read" : "the file is larger than 64 KB";
    } else {
        std::ifstream in(*file, std::ios::binary);
        pick->file_text.resize(static_cast<std::size_t>(size));
        if (!in.read(pick->file_text.data(), static_cast<std::streamsize>(size))) {
            pick->button = "file_error";
            pick->text = "the file cannot be read";
            pick->file_text.clear();
        }
    }
    send(*pick);
}
} // namespace detail
} // namespace dingosdk::overlay
