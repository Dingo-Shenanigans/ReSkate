#include "multiplayer_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Core/Text/ascii.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <Windows.h>

namespace dingosdk::overlay::menu::multiplayer_detail {
namespace {
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
// A row of the list: its lobby (an index into the model's list) and what it is sorted and
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
    const auto filter = ascii_lower(menu.multiplayer_lobby_search.data());
    for (std::size_t index = 0; index < mp.lobbies.size(); ++index) {
        const auto &lobby = mp.lobbies[index];
        Row row{index, ascii_lower(lobby.name), map_label(model, lobby.map), {}, same_map(lobby.map, mp.map),
                lobby.owner == mp.local_id, lobby.players >= lobby.capacity};
        // Servers advertise their map's name rather than its destination.
        if (lobby.dedicated && !mp.map.empty()) row.same_map = row.map == map_label(model, mp.map);
        row.map_key = ascii_lower(row.map);
        if (menu.multiplayer_same_map_only && !row.same_map) continue;
        if (menu.multiplayer_dedicated_only && !lobby.dedicated) continue;
        if (!filter.empty()) {
            auto text = row.host_key + " " + row.map_key + " " + ascii_lower(lobby.map);
            for (const auto &name : lobby.friends) text += " " + ascii_lower(name);
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
// Height the join-by-code card takes below the list, before it has been drawn once.
float direct_join_height() {
    return ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 2 + px(40);
}
void direct_join(SkateMenu &menu, const MultiplayerModel &mp) {
    begin_card(menu, "join-code", "JOIN WITH A CODE");
    const bool available = !mp.active && !mp.lobby_joining;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputTextWithHint(
        "##join-code", "Paste a friend's join code", menu.multiplayer_join_code.data(),
        menu.multiplayer_join_code.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    const int pressed = button_row(menu, "join-code-actions", {{"JOIN", available, true}, {"WITH PASSWORD", available}});
    note("Joining loads the host's map for you.");
    end_card();
    const bool password_pressed = pressed == 1;
    if (available && (pressed == 0 || enter || password_pressed)) {
        std::string code(menu.multiplayer_join_code.data());
        const auto first = code.find_first_not_of(" \t\r\n"), last = code.find_last_not_of(" \t\r\n");
        code = first == std::string::npos ? std::string{} : code.substr(first, last - first + 1);
        if (code.empty() || !std::all_of(code.begin(), code.end(),
                                         [](unsigned char c) { return std::isxdigit(c) || c == '-'; }))
            feedback(menu, "Paste the complete join code from the host.");
        else {
            const auto known = std::find_if(mp.lobbies.begin(), mp.lobbies.end(), [&](const auto &row) {
                return ascii_lower(row.code) == ascii_lower(code) && row.password_required;
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
    ImGui::SetNextWindowSize(ImVec2(std::min(px(460), viewport->WorkSize.x - 24.f), 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
    bool open = true;
    // B on the controller cancels, but not the press that only ended typing in the password box.
    static bool typing_before{};
    if (ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        if (ImGui::IsWindowAppearing()) typing_before = false;
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
            note(map_label(model, target.map).c_str());
            const auto current = std::find_if(mp.lobbies.begin(), mp.lobbies.end(), [&](const auto &row) {
                return row.id == target.id && row.owner == target.owner && row.code == target.code &&
                       same_map(row.map, target.map);
            });
            if (current == mp.lobbies.end())
                unavailable = "This lobby is no longer listed. Refresh and try again.";
            else if (current->players >= current->capacity)
                unavailable = "This lobby is now full.";
            if (!same_map(target.map, mp.map))
                note("Joining will load this host's map automatically.");
        } else
            note("Join the session using your code and password.");
        ImGui::Spacing();
        field(menu, "Password");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText(
            "##lobby-password", menu.multiplayer_join_password.data(), menu.multiplayer_join_password.size(),
            ImGuiInputTextFlags_EnterReturnsTrue |
                (menu.multiplayer_password_show ? 0 : ImGuiInputTextFlags_Password));
        toggle_row(menu, "Show password", nullptr, menu.multiplayer_password_show);
        if (!unavailable.empty())
            warn(unavailable.c_str());
        else if (!menu.multiplayer_password_error.empty())
            warn(menu.multiplayer_password_error.c_str());
        ImGui::Spacing();
        const int pressed = button_row(menu, "password-actions",
                                       {{"JOIN LOBBY", unavailable.empty() && menu.multiplayer_join_password[0] != 0, true}, {"CANCEL"}});
        const bool join = pressed == 0;
        const bool cancel = pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !typing_before);
        typing_before = ImGui::IsAnyItemActive();
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
    // Refreshing waits out the game-wide limit (multiplayer_browse_interval_us); the button counts it down.
    const int wait = browse_wait(mp);
    if (menu.multiplayer_lobbies_frame != frame - 1 && !wait) send_console(menu, callbacks, "mp browse");
    menu.multiplayer_lobbies_frame = frame;
    // Search, then the order, the filters and refresh as one row of tiles.
    constexpr std::array<const char *, 4> sorts{"BEST MATCH", "MOST PLAYERS", "BY NAME", "BY MAP"};
    menu.multiplayer_lobby_sort = std::clamp(menu.multiplayer_lobby_sort, 0, static_cast<int>(sorts.size()) - 1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##lobby-filter", "Search servers, maps or friends", menu.multiplayer_lobby_search.data(),
                             menu.multiplayer_lobby_search.size());
    const auto refresh_label = wait ? "REFRESH IN " + std::to_string(wait) : std::string(mp.lobby_searching ? "REFRESHING..." : "REFRESH");
    const int tool = button_row(menu, "lobby-tools",
        {{sorts[static_cast<std::size_t>(menu.multiplayer_lobby_sort)], true, false, "A changes the order: best match, most players, name or map."},
         {"SAME MAP", true, menu.multiplayer_same_map_only, "Only servers on the map you are on."},
         {"DEDICATED", true, menu.multiplayer_dedicated_only, "Only dedicated servers: always up, not someone's own game."},
         {refresh_label.c_str(), !wait && !mp.lobby_joining, false, "Look for servers again."}});
    if (tool == 0) menu.multiplayer_lobby_sort = (menu.multiplayer_lobby_sort + 1) % static_cast<int>(sorts.size());
    if (tool == 1) menu.multiplayer_same_map_only = !menu.multiplayer_same_map_only;
    if (tool == 2) menu.multiplayer_dedicated_only = !menu.multiplayer_dedicated_only;
    if (tool == 3) send_console(menu, callbacks, "mp browse");

    const auto &rows = lobby_list(menu, model);
    const auto shown = std::to_string(rows.size()) + " of " + std::to_string(mp.lobbies.size()) + " servers" +
        (mp.browser_status.empty() ? std::string{} : "  -  " + mp.browser_status);
    note(shown.c_str());

    // A row per server in a list that scrolls on its own, so the code card below always stays in
    // view. The card's real height from the last frame; the estimate only covers the first.
    const float code_space = menu.multiplayer_code_height > 0 ? menu.multiplayer_code_height : direct_join_height() + px(8);
    const float height = std::max(px(120), ImGui::GetContentRegionAvail().y - code_space);
    ImGui::BeginChild("lobby-list", ImVec2(0, height), ImGuiChildFlags_NavFlattened);
    for (const auto &row : rows) {
        const auto &lobby = mp.lobbies[row.lobby];
        const auto id = std::to_string(lobby.id);
        // From a session they joined a player can hop straight to another; a host ends theirs first.
        const bool here = mp.active && lobby.id == mp.public_lobby;
        const bool hop = mp.active && !mp.hosting && !mp.echo && !here;
        const bool joinable = (!mp.active || hop) && !mp.lobby_joining && !row.self && !row.full;
        // The team's own servers are ReSkate blue, and the ones friends are skating in green; a blue
        // edge marks the ones on your map.
        const bool with_friends = !lobby.friends.empty() && !lobby.official;
        OpenRow entry;
        entry.name = lobby.name;
        entry.detail = row.map + (row.same_map ? "  -  your map" : "") +
                       (lobby.ping >= 0 ? "  -  " + std::to_string(lobby.ping) + " ms" : std::string{}) +
                       (lobby.friends.empty() ? std::string{} : "  -  with " + lobby_friends_text(lobby));
        entry.side = std::to_string(lobby.players) + " / " + std::to_string(lobby.capacity);
        entry.side_colour = row.full ? skate_theme::danger : skate_theme::white;
        entry.name_colour = lobby.official ? skate_theme::official_text : with_friends ? skate_theme::friends_text : 0;
        entry.edge = row.same_map ? skate_theme::blue : 0;
        entry.tags.push_back({lobby.official ? "OFFICIAL" : lobby.dedicated ? "DEDICATED" : "PLAYER",
                              lobby.official ? skate_theme::official : lobby.dedicated ? skate_theme::blue : skate_theme::grey_text});
        if (lobby.password_required) entry.tags.push_back({"PASSWORD", skate_theme::warning});
        entry.hint = "A to join, or for more about this server.";
        if (!open_row(menu, id.c_str(), entry)) continue;
        // What joining does from here, or why it cannot (a controller has no tooltips).
        const auto about = row.self ? std::string("This is your lobby.") : here ? std::string("You are skating here.")
                         : row.full ? std::string("This server is full.")
                         : mp.lobby_joining ? std::string("A lobby join is in progress.")
                         : !joinable ? std::string("End your session before joining another.")
                         : hop && row.same_map ? std::string("Leaves your current session and joins this one.")
                         : hop ? "Leaves your current session, loads " + row.map + " and joins this one."
                         : !row.same_map ? "Joining loads " + row.map + " for you." : std::string{};
        if (!about.empty()) note(about.c_str());
        if (!lobby.friends.empty()) info(menu, "Friends here", lobby_friends_text(lobby, lobby.friends.size()));
        const int pressed = action_chips(menu, "join",
            {{row.self ? "YOURS" : here ? "HERE" : row.full ? "FULL" : "JOIN", joinable, true},
             {"WITH PASSWORD", joinable, false, "Type the server's password before joining."}});
        if (pressed == 0) {
            if (lobby.password_required) prompt_password(menu, &lobby);
            else send_private(menu, "join-lobby", id, menu.multiplayer_join_password, false);
        }
        if (pressed == 1) prompt_password(menu, &lobby);
        end_open_row();
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
