#include "gui_internal.h"

#include "text_encoding.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"

#include <algorithm>
#include <format>

// The LEGAL panel: the privacy policy and terms of service as the ReSkate backend has them now
// (GET https://api.reskate.dev/api/v1/legal/<kind>), so a change reaches players without a release.
namespace dingosdk::launcher_gui::detail {
namespace {
constexpr const char* kinds[Legal::count]{"privacy", "terms"};
constexpr const char* names[Legal::count]{"PRIVACY POLICY", "TERMS OF SERVICE"};
constexpr std::uint64_t answer_limit = 256 * 1024;
constexpr int timeout_ms = 15000;

void start_fetch(Legal& legal, int kind) {
    auto& slot = legal.slots[static_cast<std::size_t>(kind)];
    if (slot.loading.exchange(true)) return;
    if (slot.worker.joinable()) slot.worker.join();
    slot.error.clear();
    slot.worker = std::thread([&legal, &slot, kind] {
        Legal::Document document;
        std::string error;
        try {
            std::string body;
            const auto url = std::string("https://api.reskate.dev/api/v1/legal/") + kinds[kind];
            launcher_update::http_stream(launcher_text::wide(url), answer_limit, timeout_ms,
                                         [&](const char* data, DWORD size) { body.append(data, size); });
            const auto root = Json::parse(body);
            document.title = root.at("title").string();
            document.markdown = root.at("markdown").string();
            document.version = root.at("version").get<std::uint64_t>();
            document.updated = root.at("updated_at").string().substr(0, 10);
            if (document.markdown.empty()) throw std::runtime_error("the document came back empty");
        } catch (const std::exception& failure) {
            error = failure.what();
            logging::write(logging::Level::warning, logging::Channel::launcher,
                           std::format("Could not get the {}: {}", kinds[kind], error));
        }
        std::lock_guard lock(legal.mutex);
        slot.incoming = std::move(document);
        slot.incoming_error = std::move(error);
        slot.arrived = true;
        slot.loading = false;
    });
}
} // namespace

void legal_window(const Fonts& fonts, ImVec2 size, Ui& ui) {
    auto& legal = ui.legal;
    const int kind = legal.open;
    auto& slot = legal.slots[static_cast<std::size_t>(kind)];
    {
        std::lock_guard lock(legal.mutex);
        for (auto& each : legal.slots)
            if (each.arrived) {
                each.arrived = false;
                each.error = std::move(each.incoming_error);
                if (each.error.empty()) {
                    each.document = std::move(each.incoming);
                    each.text = thunderstore::readme_lines(each.document.markdown, 2000);
                    each.loaded = true;
                }
                each.incoming = {};
            }
    }
    if (!slot.loaded && !slot.loading && slot.error.empty()) start_fetch(legal, kind);

    const auto panel = begin_panel("##legal_panel", size, ImVec2(S(880), S(680)));
    panel_title(fonts, names[kind]);
    const float footer = ImGui::GetFrameHeight() + S(24) + S(12);
    ImGui::BeginChild("##legal_body", ImVec2(0, panel.y - ImGui::GetCursorPosY() - footer), ImGuiChildFlags_NavFlattened);
    if (slot.loading && !slot.loaded) {
        ImGui::TextDisabled("Loading...");
    } else if (!slot.loaded) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Could not load it: %s", slot.error.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Try again", ImVec2(S(120), 0))) start_fetch(legal, kind);
    } else {
        const auto& document = slot.document;
        ImGui::PushFont(fonts.heading);
        ImGui::TextUnformatted(document.title.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("Version %llu, updated %s", static_cast<unsigned long long>(document.version), document.updated.c_str());
        ImGui::Dummy(ImVec2(0, S(6)));
        draw_readme(fonts, slot.text);
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    const int other = kind == Legal::privacy ? Legal::terms : Legal::privacy;
    if (ImGui::Button(kind == Legal::privacy ? "Terms of Service" : "Privacy Policy", ImVec2(S(180), 0))) legal.open = other;
    ImGui::SameLine();
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    const bool close = ImGui::Button("CLOSE", ImVec2(S(110), 0));
    default_focus();
    if (close || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) legal.open = -1;
    ImGui::End();
}

void check_legal_versions(Ui& ui, const Settings& settings) {
    auto& legal = ui.legal;
    if (!legal.versions_started) {
        legal.versions_started = true;
        legal.versions_worker = std::thread([&legal] {
            std::array<std::uint64_t, Legal::count> found{};
            try {
                std::string body;
                launcher_update::http_stream(L"https://api.reskate.dev/api/v1/legal", answer_limit, timeout_ms,
                                             [&](const char* data, DWORD size) { body.append(data, size); });
                const auto root = Json::parse(body);
                const auto& documents = root.at("documents");
                for (std::size_t index = 0; index < documents.size(); ++index) {
                    const auto& document = documents.at(index);
                    const auto kind = document.at("kind").string();
                    for (int at = 0; at < Legal::count; ++at)
                        if (kind == kinds[at]) found[static_cast<std::size_t>(at)] = document.at("version").get<std::uint64_t>();
                }
            } catch (const std::exception& failure) {
                // Nothing to show: the launcher does not hold anyone up over it.
                logging::write(logging::Level::warning, logging::Channel::launcher,
                               std::format("Could not check the privacy policy and terms: {}", failure.what()));
            }
            std::lock_guard lock(legal.mutex);
            legal.current = found;
            legal.versions_arrived = true;
        });
    }
    std::lock_guard lock(legal.mutex);
    if (!legal.versions_arrived) return;
    legal.versions_arrived = false;
    const auto& current = legal.current;
    legal.agreement = current[Legal::privacy] > settings.accepted_privacy || current[Legal::terms] > settings.accepted_terms;
    if (legal.agreement)
        logging::write(logging::Level::info, logging::Channel::launcher,
                       std::format("Privacy policy {} and terms {} to agree to (agreed: {} and {}).", current[Legal::privacy],
                                   current[Legal::terms], settings.accepted_privacy, settings.accepted_terms));
}

void legal_agreement_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui, HWND window) {
    auto& legal = ui.legal;
    auto& settings = launcher.settings();
    const bool first = !settings.accepted_privacy && !settings.accepted_terms;
    const auto panel = begin_panel("##legal_agreement", size, ImVec2(S(640), S(470)));
    panel_title(fonts, first ? "BEFORE YOU PLAY" : "UPDATED TERMS");
    ImGui::PushTextWrapPos(0);
    if (first) {
        ImGui::TextUnformatted("ReSkate has terms of service and a privacy policy. Read them, and agree to keep using ReSkate.");
    } else {
        std::string changed;
        if (legal.current[Legal::terms] > settings.accepted_terms) changed = "terms of service";
        if (legal.current[Legal::privacy] > settings.accepted_privacy)
            changed += changed.empty() ? "privacy policy" : " and the privacy policy";
        ImGui::TextUnformatted(std::format("The {} changed since you last agreed. Read what changed, and agree to keep using "
                                           "ReSkate.", changed).c_str());
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::TextDisabled("In short:");
    // (BulletText does not wrap: a bullet, then wrapped text beside it.)
    for (const char* line : {"Play fair online and respect other players.",
                             "Don't unlock or copy cosmetics that skate sells.",
                             "Crash reports go to the ReSkate team unless you turn them off in Settings. Nothing in them is "
                             "meant to identify you.",
                             "ReSkate is a free fan project, not made by EA, and comes with no warranty."}) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", line);
    }
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(10)));
    if (ImGui::Button("Read the Terms of Service", ImVec2(S(260), 0))) legal.open = Legal::terms;
    ImGui::SameLine();
    if (ImGui::Button("Read the Privacy Policy", ImVec2(S(260), 0))) legal.open = Legal::privacy;

    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    if (ImGui::Button("QUIT", ImVec2(S(110), 0))) PostMessageW(window, WM_CLOSE, 0, 0);
    ImGui::SameLine();
    ImGui::SetCursorPosX(panel.x - S(28) - S(160));
    push_primary_button();
    const bool agree = ImGui::Button("I AGREE", ImVec2(S(160), 0));
    pop_primary_button();
    default_focus();
    if (agree) {
        settings.accepted_privacy = (std::max)(settings.accepted_privacy, legal.current[Legal::privacy]);
        settings.accepted_terms = (std::max)(settings.accepted_terms, legal.current[Legal::terms]);
        launcher.save();
        legal.agreement = false;
        logging::write(logging::Level::info, logging::Channel::launcher,
                       std::format("Agreed to privacy policy {} and terms {}.", settings.accepted_privacy, settings.accepted_terms));
    }
    ImGui::End();
}

} // namespace dingosdk::launcher_gui::detail
