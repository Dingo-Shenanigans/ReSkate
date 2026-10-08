#include "multiplayer_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <Windows.h>

namespace dingosdk::overlay::menu::multiplayer_detail {
namespace {
std::string lowercase(std::string value) {
    for (auto &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
bool same_map(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               if (x == '\\')
                   x = '/';
               if (y == '\\')
                   y = '/';
               return std::tolower(x) == std::tolower(y);
           });
}
// A tile of the list: its lobby (an index into the model's list) and what it is sorted and
// filtered by.
struct Row {
    std::size_t lobby{};
    std::string host_key, map, map_key;
    bool same_map{}, self{}, full{};
};
// The list's rows, built again only when what they come from changes: the lobbies, the
// levels map_label names them with, the search, the sort and the filters. The model is a
// fresh copy every poll, so it is compared by value rather than by address.
struct LobbyRows {
    bool valid{};
    std::vector<MultiplayerLobby> lobbies;
    std::vector<std::pair<std::string, std::string>> levels; // asset, display name
    std::string search, map;
    std::uint64_t local_id{};
    int sort{};
    bool same_map_only{}, dedicated_only{};
    std::vector<Row> rows;
};
LobbyRows &lobby_rows() {
    static LobbyRows value;
    return value;
}
bool same_lobby(const MultiplayerLobby &a, const MultiplayerLobby &b) {
    return a.id == b.id && a.owner == b.owner && a.name == b.name && a.map == b.map && a.code == b.code &&
           a.password_required == b.password_required && a.players == b.players && a.capacity == b.capacity &&
           a.dedicated == b.dedicated && a.official == b.official && a.ping == b.ping && a.friends == b.friends;
}
bool rows_current(const LobbyRows &cache, const SkateMenu &menu, const Model &model) {
    const auto &mp = model.multiplayer;
    if (!cache.valid || cache.sort != menu.multiplayer_lobby_sort || cache.same_map_only != menu.multiplayer_same_map_only ||
        cache.dedicated_only != menu.multiplayer_dedicated_only || cache.local_id != mp.local_id || cache.map != mp.map ||
        cache.search != menu.multiplayer_lobby_search.data() || cache.lobbies.size() != mp.lobbies.size() ||
        cache.levels.size() != model.levels.size())
        return false;
    for (std::size_t i = 0; i < mp.lobbies.size(); ++i)
        if (!same_lobby(cache.lobbies[i], mp.lobbies[i])) return false;
    for (std::size_t i = 0; i < model.levels.size(); ++i)
        if (cache.levels[i].first != model.levels[i].asset || cache.levels[i].second != model.levels[i].display_name)
            return false;
    return true;
}
const std::vector<Row> &lobby_list(const SkateMenu &menu, const Model &model) {
    auto &cache = lobby_rows();
    if (rows_current(cache, menu, model)) return cache.rows;
    const auto &mp = model.multiplayer;
    cache.valid = true;
    cache.lobbies = mp.lobbies;
    cache.levels.clear();
    for (const auto &level : model.levels) cache.levels.emplace_back(level.asset, level.display_name);
    cache.search = menu.multiplayer_lobby_search.data();
    cache.map = mp.map;
    cache.local_id = mp.local_id;
    cache.sort = menu.multiplayer_lobby_sort;
    cache.same_map_only = menu.multiplayer_same_map_only;
    cache.dedicated_only = menu.multiplayer_dedicated_only;
    auto &rows = cache.rows;
    rows.clear();
    const auto filter = lowercase(menu.multiplayer_lobby_search.data());
    for (std::size_t index = 0; index < mp.lobbies.size(); ++index) {
        const auto &lobby = mp.lobbies[index];
        Row row{index, lowercase(lobby.name), map_label(model, lobby.map), {}, same_map(lobby.map, mp.map),
                lobby.owner == mp.local_id, lobby.players >= lobby.capacity};
        // Servers advertise their map's name rather than its destination.
        if (lobby.dedicated && !mp.map.empty()) row.same_map = row.map == map_label(model, mp.map);
        row.map_key = lowercase(row.map);
        if (menu.multiplayer_same_map_only && !row.same_map) continue;
        if (menu.multiplayer_dedicated_only && !lobby.dedicated) continue;
        if (!filter.empty()) {
            auto text = row.host_key + " " + row.map_key + " " + lowercase(lobby.map);
            for (const auto &name : lobby.friends) text += " " + lowercase(name);
            if (text.find(filter) == std::string::npos) continue;
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [&](const Row &a, const Row &b) {
        const auto &first = mp.lobbies[a.lobby], &second = mp.lobbies[b.lobby];
        // The ReSkate team's own servers lead the list, however the rest is sorted.
        if (first.official != second.official) return first.official;
        // Then where friends are.
        if (first.friends.empty() != second.friends.empty()) return !first.friends.empty();
        switch (menu.multiplayer_lobby_sort) {
        case 1:
            if (first.players != second.players) return first.players > second.players;
            break;
        case 2:
            if (a.host_key != b.host_key) return a.host_key < b.host_key;
            break;
        case 3:
            if (a.map_key != b.map_key) return a.map_key < b.map_key;
            break;
        default: {
            // Joinable lobbies on your map first, then the busiest.
            const bool available_a = a.same_map && !a.self && !a.full, available_b = b.same_map && !b.self && !b.full;
            if (available_a != available_b) return available_a;
            if (first.players != second.players) return first.players > second.players;
            break;
        }
        }
        if (a.host_key != b.host_key) return a.host_key < b.host_key;
        return first.id < second.id;
    });
    return rows;
}
void clear_password_prompt(SkateMenu &menu) {
    SecureZeroMemory(menu.multiplayer_join_password.data(), menu.multiplayer_join_password.size());
    menu.multiplayer_password_lobby.reset();
    menu.multiplayer_password_code.clear();
    menu.multiplayer_password_error.clear();
    menu.multiplayer_password_popup_requested = menu.multiplayer_password_show = false;
}
void prompt_password(SkateMenu &menu, const MultiplayerLobby *lobby, std::string code = {}) {
    clear_password_prompt(menu);
    if (lobby)
        menu.multiplayer_password_lobby = *lobby;
    menu.multiplayer_password_code = std::move(code);
    menu.multiplayer_password_popup_requested = true;
}
// Height the join-by-code card takes below the lobby table.
float direct_join_height() {
    return ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2 + px(40);
}
void direct_join(SkateMenu &menu, const MultiplayerModel &mp) {
    begin_card(menu, "join-code", "JOIN WITH A CODE");
    const bool available = !mp.active && !mp.lobby_joining;
    const auto &style = ImGui::GetStyle();
    const float join = ImGui::CalcTextSize("Join").x + style.FramePadding.x * 2 + px(16);
    const float password = ImGui::CalcTextSize("With password...").x + style.FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - join - password - style.ItemSpacing.x * 2);
    const bool enter = ImGui::InputTextWithHint(
        "##join-code", "Paste a friend's join code", menu.multiplayer_join_code.data(),
        menu.multiplayer_join_code.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!available);
    skate_theme::push_primary_button();
    const bool join_pressed = ImGui::Button("Join", ImVec2(join, 0));
    skate_theme::pop_primary_button();
    ImGui::SameLine();
    const bool password_pressed = ImGui::Button("With password...", ImVec2(password, 0));
    ImGui::EndDisabled();
    note("Joining loads the host's map for you.");
    end_card();
    if (available && (join_pressed || enter || password_pressed)) {
        std::string code(menu.multiplayer_join_code.data());
        const auto first = code.find_first_not_of(" \t\r\n"), last = code.find_last_not_of(" \t\r\n");
        code = first == std::string::npos ? std::string{} : code.substr(first, last - first + 1);
        if (code.empty() || !std::all_of(code.begin(), code.end(),
                                         [](unsigned char c) { return std::isxdigit(c) || c == '-'; }))
            feedback(menu, "Paste the complete join code from the host.");
        else {
            const auto known = std::find_if(mp.lobbies.begin(), mp.lobbies.end(), [&](const auto &row) {
                return lowercase(row.code) == lowercase(code) && row.password_required;
            });
            if (password_pressed || known != mp.lobbies.end())
                prompt_password(menu, nullptr, std::move(code));
            else
                send_private(menu, "join", code, menu.multiplayer_join_password, false);
        }
    }
}
} // namespace
void password_popup(SkateMenu &menu, const Model &model) {
    constexpr auto title = "Lobby password###multiplayer-password";
    if (menu.multiplayer_password_popup_requested) {
        // Open and begin outside the table/row/tab ID stacks, including empty lists.
        ImGui::OpenPopup(title);
        menu.multiplayer_password_popup_requested = false;
    }
    if (!ImGui::IsPopupOpen(title)) {
        if (menu.multiplayer_password_lobby || !menu.multiplayer_password_code.empty())
            clear_password_prompt(menu);
        return;
    }
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(420.f, viewport->WorkSize.x - 24.f), 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
    bool open = true;
    if (ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        const auto &mp = model.multiplayer;
        const bool by_code = !menu.multiplayer_password_code.empty();
        std::string unavailable;
        if (!by_code && !menu.multiplayer_password_lobby)
            unavailable = "Select a lobby again.";
        else if (mp.active || mp.lobby_joining)
            unavailable = "Another session is already active or connecting.";
        if (menu.multiplayer_password_lobby) {
            const auto &target = *menu.multiplayer_password_lobby;
            ImGui::PushFont(menu.bold);
            ImGui::TextWrapped("%s", target.name.c_str());
            ImGui::PopFont();
            ImGui::TextWrapped("%s", map_label(model, target.map).c_str());
            const auto current = std::find_if(mp.lobbies.begin(), mp.lobbies.end(), [&](const auto &row) {
                return row.id == target.id && row.owner == target.owner && row.code == target.code &&
                       same_map(row.map, target.map);
            });
            if (current == mp.lobbies.end())
                unavailable = "This lobby is no longer listed. Refresh and try again.";
            else if (current->players >= current->capacity)
                unavailable = "This lobby is now full.";
            if (!same_map(target.map, mp.map))
                ImGui::TextWrapped("Joining will load this host's map automatically.");
        } else
            ImGui::TextWrapped("Join the session using your code and password.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Password");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputText(
            "##lobby-password", menu.multiplayer_join_password.data(), menu.multiplayer_join_password.size(),
            ImGuiInputTextFlags_EnterReturnsTrue |
                (menu.multiplayer_password_show ? 0 : ImGuiInputTextFlags_Password));
        ImGui::Checkbox("Show password", &menu.multiplayer_password_show);
        if (!unavailable.empty())
            ImGui::TextWrapped("%s", unavailable.c_str());
        else if (!menu.multiplayer_password_error.empty())
            ImGui::TextWrapped("%s", menu.multiplayer_password_error.c_str());
        ImGui::Spacing();
        const float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
        ImGui::BeginDisabled(!unavailable.empty() || !menu.multiplayer_join_password[0]);
        const bool join = ImGui::Button("Join lobby", ImVec2(button_width, 0));
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel =
            ImGui::Button("Cancel", ImVec2(button_width, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (cancel) {
            clear_password_prompt(menu);
            ImGui::CloseCurrentPopup();
        } else if ((join || enter) && unavailable.empty()) {
            if (!menu.multiplayer_join_password[0])
                menu.multiplayer_password_error = "Enter the lobby password.";
            else {
                const auto argument = by_code ? menu.multiplayer_password_code
                                              : std::to_string(menu.multiplayer_password_lobby->id);
                if (send_private(menu, by_code ? "join" : "join-lobby", argument,
                                 menu.multiplayer_join_password)) {
                    clear_password_prompt(menu);
                    ImGui::CloseCurrentPopup();
                } else
                    menu.multiplayer_password_error = menu.feedback;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (!open)
        clear_password_prompt(menu);
}
void join_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    const auto &mp = model.multiplayer;
    // Coming into view (the menu opening, or switching to this tab) fetches the
    // latest lobbies, so the list is never one left over from earlier.
    const int frame = ImGui::GetFrameCount();
    if (menu.multiplayer_lobbies_frame != frame - 1 && !mp.lobby_searching) send_console(menu, callbacks, "mp browse");
    menu.multiplayer_lobbies_frame = frame;
    // Toolbar: search, sort, same-map filter and refresh on one row.
    const auto &style = ImGui::GetStyle();
    constexpr std::array<const char *, 4> sorts{"Best match", "Most players", "Name", "Map"};
    menu.multiplayer_lobby_sort = std::clamp(menu.multiplayer_lobby_sort, 0, static_cast<int>(sorts.size()) - 1);
    const char *refresh_label = mp.lobby_searching ? "Refreshing..." : "Refresh";
    const float refresh_width = ImGui::CalcTextSize("Refreshing...").x + style.FramePadding.x * 2;
    const float sort_width = ImGui::CalcTextSize("Most players").x + style.FramePadding.x * 2 + ImGui::GetFrameHeight();
    const float same_map_width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Same map only").x;
    const float dedicated_width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Dedicated only").x;
    ImGui::SetNextItemWidth(std::max(px(120), ImGui::GetContentRegionAvail().x - refresh_width - sort_width -
                                                  same_map_width - dedicated_width - style.ItemSpacing.x * 4 - px(8)));
    ImGui::InputTextWithHint("##lobby-filter", "Search servers or maps", menu.multiplayer_lobby_search.data(),
                             menu.multiplayer_lobby_search.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sort_width);
    ImGui::Combo("##lobby-sort", &menu.multiplayer_lobby_sort, sorts.data(), static_cast<int>(sorts.size()));
    ImGui::SameLine(0, style.ItemSpacing.x + px(8));
    ImGui::Checkbox("Same map only", &menu.multiplayer_same_map_only);
    ImGui::SameLine();
    ImGui::Checkbox("Dedicated only", &menu.multiplayer_dedicated_only);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Dedicated servers are always up and are not someone's own game.");
    ImGui::SameLine();
    ImGui::BeginDisabled(mp.lobby_searching || mp.lobby_joining);
    if (ImGui::Button(refresh_label, ImVec2(refresh_width, 0)))
        send_console(menu, callbacks, "mp browse");
    ImGui::EndDisabled();

    const auto &rows = lobby_list(menu, model);
    const auto shown = std::to_string(rows.size()) + " of " + std::to_string(mp.lobbies.size()) + " servers" +
        (mp.browser_status.empty() ? std::string{} : "  -  " + mp.browser_status);
    note(shown.c_str());

    // One tile per lobby in a list that scrolls on its own, so the code card
    // below always stays in view.
    // The card's real height from the last frame; the estimate only covers the first.
    const float code_space = menu.multiplayer_code_height > 0 ? menu.multiplayer_code_height : direct_join_height() + px(8);
    const float height = std::max(px(120), ImGui::GetContentRegionAvail().y - code_space);
    ImGui::BeginChild("lobby-list", ImVec2(0, height));
    auto *draw = ImGui::GetWindowDrawList();
    const float tile_height = px(66), pad = px(14), gap = px(6);
    const float join_width = px(110);
    // Only the tiles in view are drawn; every tile is the same height.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (auto i = static_cast<std::size_t>(clipper.DisplayStart); i < static_cast<std::size_t>(clipper.DisplayEnd); ++i) {
            const auto &row = rows[i];
            const auto &lobby = mp.lobbies[row.lobby];
            const auto id = std::to_string(lobby.id);
            ImGui::PushID(id.c_str());
            const auto top = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            // From a session they joined a player can hop straight to another; a host ends theirs first.
            const bool here = mp.active && lobby.id == mp.public_lobby;
            const bool hop = mp.active && !mp.hosting && !mp.echo && !here;
            const bool joinable = (!mp.active || hop) && !mp.lobby_joining && !row.self && !row.full;
            // The team's own servers are ReSkate blue, and the ones friends are skating in green.
            const bool with_friends = !lobby.friends.empty() && !lobby.official;
            skate_theme::rough_rect(draw, top, ImVec2(top.x + width, top.y + tile_height),
                                    lobby.official ? skate_theme::official_tile : with_friends ? skate_theme::friends_tile : skate_theme::tile,
                                    static_cast<unsigned>(i + 41), px(1));
            if (lobby.official || with_friends)
                draw->AddRect(top, ImVec2(top.x + width, top.y + tile_height), lobby.official ? skate_theme::official : skate_theme::good, 0, 0,
                              px(1.5f));
            if (row.same_map) draw->AddRectFilled(top, ImVec2(top.x + px(4), top.y + tile_height), skate_theme::blue);

            // Right: Join, then the player count and tags leading into it.
            const float join_x = top.x + width - pad - join_width;
            ImGui::SetCursorScreenPos(ImVec2(join_x, top.y + (tile_height - ImGui::GetFrameHeight()) * .5f));
            ImGui::BeginDisabled(!joinable);
            skate_theme::push_primary_button();
            ImGui::PushFont(menu.bold);
            if (ImGui::Button(row.self ? "YOURS" : here ? "HERE" : row.full ? "FULL" : "JOIN", ImVec2(join_width, 0))) {
                if (lobby.password_required) prompt_password(menu, &lobby);
                else send_private(menu, "join-lobby", id, menu.multiplayer_join_password, false);
            }
            ImGui::PopFont();
            skate_theme::pop_primary_button();
            ImGui::EndDisabled();
            if (!joinable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", row.self ? "This is your lobby." : here ? "You are skating here."
                                        : row.full ? "This server is full."
                                        : mp.lobby_joining ? "A lobby join is in progress."
                                                           : "End your session before joining another.");
            } else if (joinable && ImGui::IsItemHovered()) {
                if (hop && row.same_map) ImGui::SetTooltip("Leaves your current session and joins this one.");
                else if (hop) ImGui::SetTooltip("Leaves your current session, loads %s and joins this one.", row.map.c_str());
                else if (!row.same_map) ImGui::SetTooltip("Joining loads %s for you.", row.map.c_str());
            }
            const auto players = std::to_string(lobby.players) + " / " + std::to_string(lobby.capacity);
            const auto players_size = menu.bold->CalcTextSizeA(px(18), FLT_MAX, 0, players.c_str());
            float right = join_x - px(18) - players_size.x;
            draw->AddText(menu.bold, px(18), ImVec2(right, top.y + (tile_height - players_size.y) * .5f),
                          row.full ? skate_theme::danger : skate_theme::white, players.c_str());
            if (lobby.password_required) {
                const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, "PASSWORD");
                right -= size.x + px(14) + px(12);
                ImGui::SetCursorScreenPos(ImVec2(right, top.y + (tile_height - ImGui::GetFrameHeight()) * .5f));
                tag(menu, "PASSWORD", skate_theme::warning);
            }
            {
                const char *kind = lobby.official ? "OFFICIAL" : lobby.dedicated ? "DEDICATED" : "PLAYER";
                const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, kind);
                right -= size.x + px(14) + px(12);
                ImGui::SetCursorScreenPos(ImVec2(right, top.y + (tile_height - ImGui::GetFrameHeight()) * .5f));
                tag(menu, kind, lobby.official ? skate_theme::official : lobby.dedicated ? skate_theme::blue : skate_theme::tile_light);
            }

            // Left: lobby name over its map.
            const float text_width = std::max(px(60), right - top.x - pad * 2);
            ImGui::SetCursorScreenPos(ImVec2(top.x + pad + px(4), top.y + px(12)));
            ImGui::BeginGroup();
            ImGui::PushFont(menu.bold);
            ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                                ImVec2(ImGui::GetCursorScreenPos().x + text_width, top.y + tile_height), true);
            if (lobby.official || with_friends)
                ImGui::PushStyleColor(ImGuiCol_Text, lobby.official ? skate_theme::official_text : skate_theme::friends_text);
            ImGui::TextUnformatted(lobby.name.c_str());
            if (lobby.official || with_friends) ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
            ImGui::TextUnformatted((row.map + (row.same_map ? "  -  your map" : "") +
                                    (lobby.ping >= 0 ? "  -  " + std::to_string(lobby.ping) + " ms" : std::string{})).c_str());
            ImGui::PopStyleColor();
            if (!lobby.friends.empty()) {
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::good);
                ImGui::TextUnformatted(("  -  with " + lobby_friends_text(lobby)).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopClipRect();
            ImGui::EndGroup();

            ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + tile_height + gap));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
    }
    if (rows.empty()) {
        ImGui::Dummy(ImVec2(0, px(18)));
        note(mp.lobby_searching ? "Searching for servers..."
             : mp.lobbies.empty() ? "No servers or open lobbies right now. Host one from the Host tab, or join a friend with a code below."
                                  : "Nothing matches your search and filters.");
    }
    ImGui::EndChild();
    const float below_list = ImGui::GetCursorPosY();
    ImGui::Dummy(ImVec2(0, px(4)));
    direct_join(menu, mp);
    // Everything under the list, so it fills the page exactly and never scrolls it.
    menu.multiplayer_code_height = ImGui::GetCursorPosY() - below_list;
}
} // namespace dingosdk::overlay::menu::multiplayer_detail
