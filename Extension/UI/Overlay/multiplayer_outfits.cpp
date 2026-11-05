// The Multiplayer page's OUTFITS tab: Skate Synchros' settings (contrib/outfit-sharing.md).
#include "multiplayer_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/Multiplayer/Synchros/synchros_runtime.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace dingosdk::overlay::menu::multiplayer_detail {
namespace {
namespace synchros = dingosdk::multiplayer::synchros;

// What the player is typing, kept between frames.
struct Drafts {
    std::array<char, 40> code{}, group_code{}, group_name{}, new_group{};
};
Drafts& drafts() {
    static Drafts d;
    return d;
}

std::string name_of(const MultiplayerModel& mp, std::uint64_t id) {
    for (const auto& p : mp.roster)
        if (p.id == id && !p.name.empty()) return p.name;
    return std::to_string(id);
}

std::string megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

// A read-only code with a Copy button beside it.
void code_row(SkateMenu& menu, const char* id, const std::string& shown) {
    std::array<char, 40> text{};
    std::copy_n(shown.begin(), std::min(shown.size(), text.size() - 1), text.begin());
    const float copy = ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - copy - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText(id, text.data(), text.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    ImGui::PushID(id);
    if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(shown.c_str());
        feedback(menu, "Code copied.");
    }
    ImGui::PopID();
}

// A text box with a button beside it; true when the button is pressed (or Enter in the box).
bool entry_row(const char* id, std::array<char, 40>& text, const char* hint, const char* button) {
    const float width = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - width - ImGui::GetStyle().ItemSpacing.x);
    const bool entered = ImGui::InputTextWithHint(id, hint, text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::PushID(id);
    const bool pressed = ImGui::Button(button, ImVec2(width, 0));
    ImGui::PopID();
    return entered || pressed;
}

// A row with a label at the left and a small button at the right; true when it is pressed.
bool list_row(const std::string& label, const char* button, ImU32 colour = 0) {
    ImGui::AlignTextToFramePadding();
    if (colour) ImGui::PushStyleColor(ImGuiCol_Text, colour);
    cell_text(label);
    if (colour) ImGui::PopStyleColor();
    const float width = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2 + px(16);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - width);
    return ImGui::Button(button, ImVec2(width, 0));
}

// Two buttons sharing a row equally.
bool half_button_row(const char* label, bool first) {
    const float width = (ImGui::GetContentRegionAvail().x - (first ? ImGui::GetStyle().ItemSpacing.x : 0.0f)) *
                        (first ? .5f : 1.0f);
    return ImGui::Button(label, ImVec2(width, 0));
}

} // namespace

void outfits_page(SkateMenu& menu, const Model& model) {
    const auto& mp = model.multiplayer;
    auto s = synchros::settings();
    auto& d = drafts();
    const auto said = [&](const std::string& text) { if (!text.empty()) feedback(menu, text.c_str()); };

    begin_card(menu, "synchros", "SKATE SYNCHROS", "See each other's modded outfits");
    note("Players see each other's modded outfits without installing each other's mods, and only the items being worn "
         "are sent. Nobody sees yours without a code from you, and you see nobody's without one from them. Paid store items "
         "are never shared, and what you are sent is never yours to wear.");
    bool share = s.share, show = s.show;
    if (toggle_row(menu, "Share my outfit", "Players who have your code, or are in a group with you, see your modded items.", share))
        said(synchros::change([&](synchros::Settings& x) { x.share = share; return std::string(share ? "Sharing your outfit." : "Not sharing your outfit."); }));
    if (toggle_row(menu, "Show shared outfits", "See the modded items of players whose code you have, or who are in a group with you.", show))
        said(synchros::change([&](synchros::Settings& x) { x.show = show; return std::string(show ? "Showing shared outfits." : "Not showing shared outfits."); }));
    bool straight_away = s.reload_on_arrival;
    ImGui::BeginDisabled(!s.show);
    if (toggle_row(menu, "Show arrivals straight away",
                   "Loads the map again for you alone, for a few seconds, as soon as a player's outfit has arrived "
                   "(at most once a minute). Off: it shows from the next map.", straight_away))
        said(synchros::change([&](synchros::Settings& x) { x.reload_on_arrival = straight_away; return std::string(); }));
    ImGui::EndDisabled();
    end_card();

    const auto paired_count = std::to_string(s.paired.size()) + " paired";
    begin_card(menu, "synchros-code", "YOUR CODE", s.paired.empty() ? nullptr : paired_count.c_str());
    if (s.own_code.empty()) {
        note("Turn on Share my outfit to get your code.");
    } else {
        field(menu, "Code", "Give it to whoever may see your outfit. It says nothing about who you are.");
        code_row(menu, "##own-code", synchros::show_code(s.own_code, synchros::CodeKind::personal));
        if (ImGui::Button("New code", ImVec2(-FLT_MIN, 0)))
            said(synchros::change([](synchros::Settings& x) {
                return "Your new code: " + synchros::renew_code(x) + ". Players already paired stay until you remove them.";
            }));
    }
    if (!s.paired.empty()) {
        field(menu, "Paired", "Players who entered your code. They keep seeing your outfit until you remove them.");
        for (const auto id : s.paired) {
            ImGui::PushID(std::to_string(id).c_str());
            if (list_row(name_of(mp, id), "Remove"))
                said(synchros::change([id](synchros::Settings& x) { x.paired.erase(id); return std::string("Removed."); }));
            ImGui::PopID();
        }
    }
    end_card();

    const auto code_count = std::to_string(s.entered.size());
    begin_card(menu, "synchros-codes", "CODES YOU HAVE", s.entered.empty() ? nullptr : code_count.c_str());
    note("Enter a code a player gave you to see their outfit when you are in a session together.");
    if (entry_row("##enter-code", d.code, "RSK-XXXX-XXXX-XXXX", "Add")) {
        const std::string typed = d.code.data();
        said(synchros::change([&](synchros::Settings& x) {
            const auto why = synchros::enter_code(x, typed);
            return why.empty() ? std::string("Code added.") : why;
        }));
        d.code.fill(0);
    }
    for (const auto& code : s.entered) {
        ImGui::PushID(code.c_str());
        if (list_row(synchros::show_code(code, synchros::CodeKind::personal), "Remove"))
            said(synchros::change([code](synchros::Settings& x) { std::erase(x.entered, code); return std::string("Code removed."); }));
        ImGui::PopID();
    }
    end_card();

    const auto group_count = std::to_string(s.groups.size());
    begin_card(menu, "synchros-groups", "GROUPS", s.groups.empty() ? nullptr : group_count.c_str());
    note("Everyone who enters a group's code sees everyone else's outfit in it. Make one and give its code to your crew.");
    field(menu, "Make a group");
    if (entry_row("##new-group", d.new_group, "Group name", "Create")) {
        const std::string name = d.new_group.data();
        said(synchros::change([&](synchros::Settings& x) {
            const auto shown = synchros::create_group(x, name);
            return shown.empty() ? std::string("You are in as many groups as there can be.") : "Group made. Its code: " + shown;
        }));
        d.new_group.fill(0);
    }
    field(menu, "Join a group");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##group-name", "Your name for it (optional)", d.group_name.data(), d.group_name.size());
    if (entry_row("##join-group", d.group_code, "RSG-XXXX-XXXX-XXXX", "Join")) {
        const std::string typed = d.group_code.data(), name = d.group_name.data();
        said(synchros::change([&](synchros::Settings& x) {
            const auto why = synchros::join_group(x, typed, name);
            return why.empty() ? std::string("Joined the group.") : why;
        }));
        d.group_code.fill(0);
        d.group_name.fill(0);
    }
    for (std::size_t i = 0; i < s.groups.size(); ++i) {
        const auto& g = s.groups[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::PushFont(menu.bold);
        cell_text(g.name.empty() ? std::string("Group") : g.name);
        ImGui::PopFont();
        if (g.created) {
            ImGui::SameLine(0, px(6));
            tag(menu, "MADE HERE", skate_theme::blue);
        }
        code_row(menu, "##group-code", synchros::show_code(g.code, synchros::CodeKind::group));
        if (g.created) {
            if (half_button_row("New group code", true))
                said(synchros::change([i](synchros::Settings& x) {
                    const auto shown = synchros::renew_group_code(x, i);
                    return shown.empty() ? std::string("That group could not be given a new code.")
                                         : "New code: " + shown + ". Give it to everyone still meant to be in the group.";
                }));
            ImGui::SameLine();
        }
        if (half_button_row("Leave", !g.created))
            said(synchros::change([i](synchros::Settings& x) {
                if (i < x.groups.size()) synchros::leave_group(x, i);
                return std::string("Left the group.");
            }));
        ImGui::PopID();
    }
    end_card();

    if (mp.active) {
        const auto here = synchros::players();
        begin_card(menu, "synchros-session", "IN THIS SESSION");
        if (!s.share && !s.show) note("Turn on sharing or showing to swap outfits with the players here.");
        else if (here.empty()) note("Nobody here is using Skate Synchros yet.");
        for (const auto& p : here) {
            ImGui::PushID(std::to_string(p.id).c_str());
            std::string state = !p.connected ? "not using Skate Synchros"
                              : !p.refused.empty() ? "stopped: " + p.refused
                              : p.welcomed ? (p.files_missing ? "receiving their outfit (" + std::to_string(p.files_missing) + " files to come)"
                                                              : std::to_string(p.items) + " of their items shown")
                              : "they do not share with you";
            if (p.welcomed && p.custom_shaders)
                state += "; " + std::to_string(p.custom_shaders) + " with custom shaders (not shared yet) show as the default";
            if (p.sharing) state += "; they see yours";
            if (list_row(name_of(mp, p.id) + "  -  " + state, "Block")) {
                synchros::block_player(p.id);
                feedback(menu, "Blocked: neither of you sees the other's outfit.");
            }
            ImGui::PopID();
        }
        end_card();
    }

    const auto blocked_count = std::to_string(s.blocked.size());
    begin_card(menu, "synchros-blocked", "BLOCKED", s.blocked.empty() ? nullptr : blocked_count.c_str());
    if (s.blocked.empty()) note("Nobody. Block a player from IN THIS SESSION: neither of you then sees the other's outfit, codes or not.");
    for (const auto id : s.blocked) {
        ImGui::PushID(std::to_string(id).c_str());
        if (list_row(name_of(mp, id), "Unblock"))
            said(synchros::change([id](synchros::Settings& x) { x.blocked.erase(id); return std::string("Unblocked."); }));
        ImGui::PopID();
    }
    end_card();

    begin_card(menu, "synchros-cache", "RECEIVED OUTFITS");
    info(menu, "On this PC", megabytes(synchros::cache_bytes()));
    note("Kept so an outfit is not sent again each session. The oldest go first past 2 GB.");
    if (ImGui::Button("Empty", ImVec2(-FLT_MIN, 0))) {
        synchros::empty_cache();
        feedback(menu, "Emptied. Shared outfits are sent again when you next meet.");
    }
    end_card();
}

} // namespace dingosdk::overlay::menu::multiplayer_detail
