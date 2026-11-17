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
            std::array<std::string, Legal::count> dated;
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
                        if (kind == kinds[at]) {
                            found[static_cast<std::size_t>(at)] = document.at("version").get<std::uint64_t>();
                            dated[static_cast<std::size_t>(at)] = document.value("updated_at", std::string()).substr(0, 10);
                        }
                }
            } catch (const std::exception& failure) {
                // Nothing to show: the launcher does not hold anyone up over it.
                logging::write(logging::Level::warning, logging::Channel::launcher,
                               std::format("Could not check the privacy policy and terms: {}", failure.what()));
            }
            std::lock_guard lock(legal.mutex);
            legal.current = found;
            legal.updated = dated;
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
    const float pad = S(28);
    const auto panel = begin_panel("##legal_agreement", size, ImVec2(S(640), std::min(S(540), size.y - S(40))));
    auto* draw = ImGui::GetWindowDrawList();
    panel_title(fonts, first ? "BEFORE YOU PLAY" : "UPDATED TERMS");
    const float inner = panel.x - pad * 2;
    ImGui::PushTextWrapPos(0);
    if (first) {
        ImGui::TextDisabled("ReSkate has terms of service and a privacy policy. Here is the short of them.");
    } else {
        std::string changed;
        if (legal.current[Legal::terms] > settings.accepted_terms) changed = "terms of service";
        if (legal.current[Legal::privacy] > settings.accepted_privacy)
            changed += changed.empty() ? "privacy policy" : " and the privacy policy";
        ImGui::TextDisabled("%s", std::format("The {} changed since you last agreed. The short of them is still this.", changed).c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(8)));

    // ------------------------------------------------ the short of them: a row each in one box, as a setting is
    {
        struct Line { const char* title; const char* text; };
        const std::array lines{
            Line{"Play fair", "Respect the other players online."},
            Line{"No paid cosmetics", "Don't unlock or copy what skate. sells."},
            Line{"Crash reports", "They help the ReSkate team fix crashes and aren't meant to identify you. Optional."},
            Line{"A free fan project", "Not made by EA, and it comes with no warranty."}};
        const float row = S(52), x_pad = S(18);
        const ImVec2 box = ImGui::GetCursorScreenPos(), box_end(box.x + inner, box.y + row * static_cast<float>(lines.size()));
        draw->AddRectFilled(box, box_end, rgba(31, 31, 34), S(6));
        draw->AddRect(box, box_end, color::outline, S(6));
        // The launcher's mark for what matters on a tile: its edge in blue.
        draw->AddRectFilled(box, ImVec2(box.x + S(4), box_end.y), color::blue, S(6), ImDrawFlags_RoundCornersLeft);
        for (std::size_t at = 0; at < lines.size(); ++at) {
            const float top = box.y + row * static_cast<float>(at);
            if (at) draw->AddLine(ImVec2(box.x + S(5), top), ImVec2(box_end.x - S(1), top), color::outline);
            const float block = fonts.bold->FontSize + S(4) + fonts.body->FontSize;
            const float y = top + (row - block) * 0.5f;
            const ImVec4 clip(box.x + x_pad, top, box_end.x - x_pad, top + row);
            draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(box.x + x_pad, y), color::text, lines[at].title);
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(box.x + x_pad, y + fonts.bold->FontSize + S(4)), color::muted, lines[at].text,
                          nullptr, 0, &clip);
        }
        ImGui::Dummy(ImVec2(inner, box_end.y - box.y));
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    // ------------------------------------------------ the documents themselves: a row each, which opens it
    {
        const float row = S(54);
        const ImVec2 box = ImGui::GetCursorScreenPos(), box_end(box.x + inner, box.y + row * 2);
        draw->AddRectFilled(box, box_end, rgba(31, 31, 34), S(6));
        draw->AddRect(box, box_end, color::outline, S(6));
        for (const int kind : {Legal::terms, Legal::privacy}) {
            const ImVec2 start = ImGui::GetCursorScreenPos(), end(start.x + inner, start.y + row);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            if (ImGui::InvisibleButton(kind == Legal::terms ? "##read_terms" : "##read_privacy", ImVec2(inner, row), ImGuiButtonFlags_EnableNav))
                legal.open = kind;
            ImGui::PopStyleVar();
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                draw->AddRectFilled(ImVec2(start.x + S(1), start.y + S(1)), ImVec2(end.x - S(1), end.y - S(1)), rgba(255, 255, 255, 0.045f), S(5));
            }
            if (kind == Legal::privacy) draw->AddLine(ImVec2(start.x + S(1), start.y), ImVec2(end.x - S(1), start.y), color::outline);
            const float block = fonts.bold->FontSize + S(4) + fonts.caption->FontSize;
            const float y = start.y + (row - block) * 0.5f, x = start.x + S(16);
            draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(x, y), color::text, kind == Legal::terms ? "Terms of Service" : "Privacy Policy");
            // When it was last changed, and that it has since this player agreed (why they are asked again).
            const auto& dated = legal.updated[static_cast<std::size_t>(kind)];
            const auto agreed = kind == Legal::terms ? settings.accepted_terms : settings.accepted_privacy;
            const bool changed = !first && legal.current[static_cast<std::size_t>(kind)] > agreed;
            const auto under = dated.empty() ? std::string("Read it in full") : "Updated " + dated;
            draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(x, y + fonts.bold->FontSize + S(4)), color::muted, under.c_str());
            if (changed) {
                const float after = x + fonts.caption->CalcTextSizeA(fonts.caption->FontSize, FLT_MAX, 0, under.c_str()).x;
                draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(after, y + fonts.bold->FontSize + S(4)), color::warning,
                              "   /   changed since you last agreed");
            }
            // A chevron at the far end: it opens.
            const ImVec2 tip(end.x - S(20), start.y + row * 0.5f);
            const ImU32 ink = hovered ? color::text : color::muted;
            const ImVec2 chevron[]{ImVec2(tip.x - S(5), tip.y - S(6)), ImVec2(tip.x + S(1), tip.y), ImVec2(tip.x - S(5), tip.y + S(6))};
            draw->AddPolyline(chevron, 3, ink, 0, S(2));
        }
    }

    // ------------------------------------------------ the answer
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    if (ImGui::Button("QUIT", ImVec2(S(110), 0))) PostMessageW(window, WM_CLOSE, 0, 0);
    const float agree_width = S(160);
    {
        // What pressing it means, said beside it.
        const char* means = "By pressing I AGREE you accept both.";
        const float across = fonts.caption->CalcTextSizeA(fonts.caption->FontSize, FLT_MAX, 0, means).x;
        const ImVec2 at(ImGui::GetWindowPos().x + panel.x - pad - agree_width - S(14) - across,
                        ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - fonts.caption->FontSize) * 0.5f);
        draw->AddText(fonts.caption, fonts.caption->FontSize, at, color::muted, means);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(panel.x - pad - agree_width);
    push_primary_button();
    const bool agree = ImGui::Button("I AGREE", ImVec2(agree_width, 0));
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
