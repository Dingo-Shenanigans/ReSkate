#include "gui_internal.h"

#include "Engine/Core/Log/logging.h"

// The CHANGELOGS panel: what the newest ReSkate release changed, as its GitHub release says.
namespace dingosdk::launcher_gui::detail {
namespace {

void start_fetch(Changelog& log) {
    if (log.loading.exchange(true)) return;
    if (log.worker.joinable()) log.worker.join();
    log.error.clear();
    log.fetched_prereleases = log.prereleases;
    log.worker = std::thread([&log, prereleases = log.prereleases] {
        update::ReleaseNote notes;
        std::string error;
        try {
            notes = update::fetch_release_note(prereleases);
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

void changelog_window(const Fonts& fonts, ImVec2 size, Ui& ui) {
    auto& log = ui.notes;
    {
        std::lock_guard lock(log.mutex);
        if (log.arrived) {
            log.arrived = false;
            log.error = std::move(log.incoming_error);
            if (log.error.empty()) {
                log.note = std::move(log.incoming);
                log.text = thunderstore::readme_lines(log.note.notes);
                log.loaded = true;
            }
            log.incoming = {};
        }
    }
    // (The pre-release setting changed since: the notes are fetched again for it.)
    if (log.loaded && !log.loading && log.fetched_prereleases != log.prereleases) log.loaded = false;
    if (!log.loaded && !log.loading && log.error.empty()) start_fetch(log);

    const auto panel = begin_panel("##changelog_panel", size, ImVec2(S(880), S(680)));
    panel_title(fonts, tr("CHANGELOGS"));
    const float footer = ImGui::GetFrameHeight() + S(24) + S(12);
    ImGui::BeginChild("##changelog_body", ImVec2(0, panel.y - ImGui::GetCursorPosY() - footer), ImGuiChildFlags_NavFlattened);
    if (log.loading && !log.loaded) {
        ImGui::TextDisabled("%s", tr("Loading the changelogs..."));
    } else if (!log.loaded) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", trf("Could not load the changelogs: {}", log.error).c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Try again"), ImVec2(S(120), 0))) start_fetch(log);
    } else {
        const auto& note = log.note;
        const auto label = note.title.empty() || note.title == note.tag ? note.tag : note.tag + "  -  " + note.title;
        ImGui::PushFont(fonts.heading);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopFont();
        if (!note.date.empty())
            ImGui::TextDisabled("%s", (note.prerelease ? trf("Pre-release, {}", note.date) : trf("Released {}", note.date)).c_str());
        ImGui::Dummy(ImVec2(0, S(6)));
        if (log.text.lines.empty()) ImGui::TextDisabled("%s", tr("This release has no patch notes."));
        else draw_readme(fonts, log.text);
        if (log.text.cut) ImGui::TextDisabled("%s", tr("The rest is on the release's GitHub page."));
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    if (ImGui::Button(tr("Open on GitHub"), ImVec2(S(160), 0))) open_url(update::releases_page());
    ImGui::SameLine();
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    const bool close = ImGui::Button(tr("CLOSE"), ImVec2(S(110), 0));
    default_focus();
    if (close || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ui.changelog = false;
    ImGui::End();
}

} // namespace dingosdk::launcher_gui::detail
