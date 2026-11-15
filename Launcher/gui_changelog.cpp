#include "gui_internal.h"

#include "Engine/Core/Log/logging.h"

#include <format>

// The CHANGELOGS panel: what each ReSkate release changed, as its GitHub release says.
namespace dingosdk::launcher_gui::detail {
namespace {

void start_fetch(Changelog& log) {
    if (log.loading.exchange(true)) return;
    if (log.worker.joinable()) log.worker.join();
    log.error.clear();
    log.fetched_prereleases = log.prereleases;
    log.worker = std::thread([&log, prereleases = log.prereleases] {
        std::vector<update::ReleaseNote> notes;
        std::string error;
        try {
            notes = update::fetch_release_notes(prereleases);
        } catch (const std::exception& failure) {
            error = failure.what();
            logging::write(logging::Level::warning, logging::Channel::launcher,
                std::string("Could not get the changelogs: ") + failure.what());
        }
        std::lock_guard lock(log.mutex);
        log.incoming = std::move(notes);
        log.incoming_error = std::move(error);
        log.arrived = true;
        log.loading = false;
    });
}

} // namespace

// A short line of text with a list straight under it is that list's heading, however it was
// written (the notes name their sections in bold as often as with a #).
void draw_notes(const Fonts& fonts, const thunderstore::Readme& readme, const std::string& tag, bool leads) {
    using Kind = thunderstore::ReadmeLine::Kind;
    auto* draw = ImGui::GetWindowDrawList();
    const float indent = S(20);
    const auto heads_list = [&](std::size_t at) {
        const auto& text = readme.lines[at].text;
        if (text.empty() || text.size() > 60 || text.back() == '.') return false;
        for (std::size_t next = at + 1; next < readme.lines.size(); ++next)
            if (readme.lines[next].kind != Kind::gap) return readme.lines[next].kind == Kind::bullet;
        return false;
    };
    bool first = true;
    ImGui::PushTextWrapPos(0);
    for (std::size_t index = 0; index < readme.lines.size(); ++index) {
        const auto& line = readme.lines[index];
        const bool named = first && line.kind == Kind::text && !tag.empty() && line.text.find(tag) != std::string::npos && line.text.size() < tag.size() + 24;
        if (line.kind != Kind::gap) first = false;
        if (named) continue;
        switch (line.kind == Kind::text && heads_list(index) ? Kind::heading : line.kind) {
        case Kind::heading: {
            ImGui::Dummy(ImVec2(0, S(10)));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            draw->AddRectFilled(ImVec2(at.x, at.y + S(2)), ImVec2(at.x + S(4), at.y + fonts.heading->FontSize - S(1)), color::blue);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(14));
            ImGui::PushFont(fonts.heading);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, S(2)));
            break;
        }
        case Kind::bullet: {
            // Its first sentence, when there is more after it and it is short enough to be a lead.
            auto stop = line.text.find(". ");
            if (stop != std::string::npos && (stop > 110 || stop + 2 >= line.text.size())) stop = std::string::npos;
            const auto lead = stop == std::string::npos ? line.text : line.text.substr(0, stop + 1);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            draw->AddCircleFilled(ImVec2(at.x + S(7), at.y + ImGui::GetFontSize() * 0.5f + S(1)), S(2.5f), leads ? color::muted : color::blue);
            ImGui::Indent(indent);
            if (!leads) {
                // A plain list: each line as it is.
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(rgba(222, 226, 233)));
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(indent);
                ImGui::Dummy(ImVec2(0, S(1)));
                break;
            }
            ImGui::PushFont(fonts.bold);
            ImGui::TextUnformatted(lead.c_str());
            ImGui::PopFont();
            if (stop != std::string::npos) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(rgba(176, 183, 194)));
                ImGui::TextUnformatted(line.text.c_str() + stop + 2);
                ImGui::PopStyleColor();
            }
            ImGui::Unindent(indent);
            ImGui::Dummy(ImVec2(0, S(4)));
            break;
        }
        case Kind::code:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            break;
        case Kind::rule: ImGui::Separator(); break;
        case Kind::gap: ImGui::Spacing(); break;
        case Kind::text:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(rgba(206, 211, 220)));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            break;
        }
    }
    ImGui::PopTextWrapPos();
}

void pump_changelog(Changelog& log) {
    {
        std::lock_guard lock(log.mutex);
        if (log.arrived) {
            log.arrived = false;
            log.error = std::move(log.incoming_error);
            if (log.error.empty()) {
                log.notes = std::move(log.incoming);
                log.texts.clear();
                for (const auto& note : log.notes) log.texts.push_back(thunderstore::readme_lines(note.notes));
                log.shown = 0;
                log.loaded = true;
            }
            log.incoming.clear();
        }
    }
    // (The pre-release setting changed since: the notes are fetched again for it.)
    if (log.loaded && !log.loading && log.fetched_prereleases != log.prereleases) log.loaded = false;
    if (!log.loaded && !log.loading && log.error.empty()) start_fetch(log);
}

void changelog_window(const Fonts& fonts, ImVec2 size, Ui& ui, const std::string& installed) {
    auto& log = ui.notes;
    pump_changelog(log);

    const auto panel = begin_panel("##changelog_panel", size, ImVec2(std::min(S(1180), size.x - S(120)), std::min(S(740), size.y - S(70))));
    panel_title(fonts, "CHANGELOGS");
    const float footer = ImGui::GetFrameHeight() + S(24) + S(12);
    const float tall = panel.y - ImGui::GetCursorPosY() - footer;
    if (log.loading && !log.loaded) {
        ImGui::TextDisabled("Loading the changelogs...");
    } else if (!log.loaded) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Could not load the changelogs: %s", log.error.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Try again", ImVec2(S(120), 0))) start_fetch(log);
    } else {
        log.shown = std::clamp(log.shown, 0, static_cast<int>(log.notes.size()) - 1);
        // ------------------------------------------------ the releases, newest first
        const float list = S(230);
        ImGui::BeginChild("##changelog_list", ImVec2(list, tall), ImGuiChildFlags_NavFlattened);
        auto* draw = ImGui::GetWindowDrawList();
        const float row = S(54);
        for (int index = 0; index < static_cast<int>(log.notes.size()); ++index) {
            const auto& note = log.notes[static_cast<std::size_t>(index)];
            const bool chosen = index == log.shown;
            ImGui::PushID(index);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            if (ImGui::InvisibleButton("##release", ImVec2(width, row), ImGuiButtonFlags_EnableNav)) log.shown = index;
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const ImVec2 end(start.x + width, start.y + row);
            if (chosen) draw->AddRectFilled(start, end, rgba(28, 38, 52), S(4));
            else if (hovered) draw->AddRectFilled(start, end, rgba(255, 255, 255, 0.05f), S(4));
            if (chosen) draw->AddRectFilled(start, ImVec2(start.x + S(3), end.y), color::blue);
            const float x = start.x + S(14);
            const float y = start.y + (row - fonts.bold->FontSize - S(4) - fonts.caption->FontSize) * 0.5f;
            draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(x, y), chosen ? color::text : rgba(206, 211, 220), note.tag.c_str());
            const float tag_width = fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, note.tag.c_str()).x;
            // The release this is; else that it is the newest, or a pre-release.
            const char* mark = !installed.empty() && note.tag == installed ? "INSTALLED" : note.prerelease ? "PRE-RELEASE" : index == 0 ? "NEWEST" : nullptr;
            if (mark)
                badge(draw, fonts, ImVec2(x + tag_width + S(8), y + (fonts.bold->FontSize - fonts.caption->FontSize - S(8)) * 0.5f), mark,
                      mark[0] == 'I' ? color::good : mark[0] == 'P' ? color::warning : color::blue, color::ink);
            draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(x, y + fonts.bold->FontSize + S(4)), color::muted, note.date.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();

        // ------------------------------------------------ the chosen release's notes, on a card
        ImGui::SameLine(0, S(16));
        const auto& note = log.notes[static_cast<std::size_t>(log.shown)];
        const auto& text = log.texts[static_cast<std::size_t>(log.shown)];
        {
            const ImVec2 box = ImGui::GetCursorScreenPos();
            auto* behind = ImGui::GetWindowDrawList();
            behind->AddRectFilled(box, ImVec2(box.x + ImGui::GetContentRegionAvail().x, box.y + tall), rgba(31, 31, 34), S(6));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(20)));
        ImGui::BeginChild("##changelog_body", ImVec2(0, tall), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        // The notes are read, not scanned across: a column of a readable width, however wide the panel is.
        ImGui::PushTextWrapPos(std::min(ImGui::GetContentRegionMax().x, ImGui::GetCursorPosX() + S(760)));
        const auto label = note.title.empty() || note.title == note.tag ? "Build " + note.tag : note.title;
        ImGui::PushFont(fonts.tile);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopFont();
        if (!note.date.empty()) ImGui::TextDisabled("%s %s", note.prerelease ? "Pre-release," : "Released", note.date.c_str());
        ImGui::Dummy(ImVec2(0, S(6)));
        if (text.lines.empty()) ImGui::TextDisabled("This release has no patch notes.");
        else {
            ImGui::BeginChild("##changelog_notes", ImVec2(std::min(ImGui::GetContentRegionAvail().x, S(760)), 0),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
            draw_notes(fonts, text, note.tag);
            ImGui::EndChild();
        }
        if (text.cut) ImGui::TextDisabled("The rest is on the release's GitHub page.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    if (ImGui::Button("Open on GitHub", ImVec2(S(160), 0))) open_url(update::releases_page());
    ImGui::SameLine();
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    const bool close = ImGui::Button("CLOSE", ImVec2(S(110), 0));
    default_focus();
    if (close || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ui.changelog = false;
    ImGui::End();
}

} // namespace dingosdk::launcher_gui::detail
