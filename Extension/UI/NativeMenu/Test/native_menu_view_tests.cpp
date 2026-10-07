#include "Extension/UI/NativeMenu/native_menu_view.h"
#include <cstdio>
#include <stdexcept>

using namespace dingosdk;
using namespace dingosdk::multiplayer::menu_view;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void map_names() {
    const std::string root = "Levels/Game/DingoLevel_Root/DingoLevel_Root";
    const std::string city = "levels/game/BAM_LevelRoot/BAM_LevelRoot";
    const std::string grom = "Levels/Game/DingoLevel_Isle_of_Grom/DingoLevel_Isle_of_Grom";
    check(map_name(root + "|" + city) == "San Vansterdam", "Composite destinations show the actual city name");
    check(map_name(city + "|") == "San Vansterdam", "Root-only destinations omit the separator");
    check(map_name(grom) == "Isle of Grom", "Native level prefixes are hidden");
    check(map_name("Levels/Game/DingoLevel_MPR/DingoLevel_MPR") == "Super Ultra Mega Resort", "Resort uses its full name");
    check(map_name("Levels/Game/DingoLevel_FTUE_Island/DingoLevel_FTUE_Island") == "Tutorial Island", "Tutorial uses the level picker name");
    check(map_name("Levels/Game/DingoLevel_SDM/DingoLevel_SDM_Int_001/DingoLevel_SDM_Int_001") == "Stadium 1", "Stadium uses the level picker name");
    check(map_name("Levels/Unknown_Park") == "Unknown Park", "Unknown maps retain a readable fallback");
    check(map_name("") == "Load a map", "Missing destination has a useful label");

    std::vector<overlay::Level> levels(2);
    levels[0].asset = "Levels/Custom/Warehouse"; levels[0].display_name = "Zee's Warehouse";
    levels[1].asset = city; // Native catalog entries can omit display names.
    const auto custom = root + "|LEVELS\\CUSTOM\\WAREHOUSE";
    check(map_name(custom, levels) == "Zee's Warehouse", "Native browser honors custom names across case and slash differences");
    check(world_destination_name(custom, levels) == "Zee's Warehouse", "Overlay resolver uses the same custom map name");
    check(world_destination_name(root + "|" + city, levels) == "San Vansterdam", "Overlay resolver uses built-in names when catalog labels are empty");

    MultiplayerModel model;
    model.lobbies.resize(3);
    model.lobbies[0].id = 1; model.lobbies[0].map = root + "|" + city;
    model.lobbies[1].id = 2; model.lobbies[1].map = root + "|" + grom;
    model.lobbies[2].id = 3; model.lobbies[2].map = custom;
    BrowserOptions options;
    options.query = "san vansterdam";
    auto result = browse(model, options, levels);
    check(result.total == 1 && result.lobbies.front()->id == 1, "Browser search finds a city by its display name");
    options.query = "Zee's Warehouse";
    result = browse(model, options, levels);
    check(result.total == 1 && result.lobbies.front()->id == 3, "Browser search finds custom display names");
    options.query.clear(); options.sort = Sort::map;
    result = browse(model, options, levels);
    check(result.lobbies[0]->id == 2 && result.lobbies[1]->id == 1 && result.lobbies[2]->id == 3,
          "Map sorting follows visible names instead of asset paths");
}
int main() {
    try {
        map_names();
        check(missing_tabs({1, 2, 3, 4}, 8, true) == std::vector<unsigned>{0, 1},
            "All pages insert in canonical order together");
        check(missing_tabs({1, 2, 3, 4, 8, 264}, 8, true).empty(),
            "Existing tabs are never reordered or reinserted");
        check(missing_tabs({1, 2, 3, 4, 8}, 8, true) == std::vector<unsigned>{1},
            "A missing tools page appends without moving existing indices");
        check(missing_tabs({1, 2, 3, 4}, 8, true, false) == std::vector<unsigned>{1},
              "offline mode leaves the Multiplayer tab out");
        check(missing_tabs({1, 2, 3, 4}, 8, false) == std::vector<unsigned>{0},
            "The Multiplayer page does not wait for the overlay's tool callbacks");
        MultiplayerModel model;
        const BrowserOptions by_name{{}, false, Sort::name};
        model.local_id = 77; model.map = "Levels/San_Vansterdam";
        for (unsigned i = 0; i < 14; ++i) {
            MultiplayerLobby lobby;
            lobby.id = 100 + i; lobby.owner = 200 + i;
            lobby.name = i % 2 ? "Downtown" : "Community";
            lobby.map = i % 3 ? model.map : "Levels/Isle_of_Grom";
            lobby.players = static_cast<int>(i + 1); lobby.capacity = 16;
            model.lobbies.push_back(lobby);
        }
        auto result = browse(model, by_name);
        check(result.total == 14 && result.lobbies.size() == 14, "All matching lobbies remain scrollable");
        check(result.lobbies.front()->id == 100 && result.lobbies[6]->id == 112 && result.lobbies.back()->id == 113,
              "Stable ordering of duplicate names across the full list");
        BrowserOptions options;
        options.query = "SAN VAN";
        result = browse(model, options);
        check(result.total == 9, "Case-insensitive friendly map search");
        options.query = "dOwNtOwN"; options.same_map = true;
        check(browse(model, options).total == 5, "Combine name and current-map filters");
        options = {}; options.sort = Sort::players;
        check(browse(model, options).lobbies.front()->players == 14, "Most populated first");
        // The team's own servers lead the list under every sort, in that sort's order among themselves.
        model.lobbies[2].dedicated = model.lobbies[2].official = true;
        model.lobbies[5].dedicated = model.lobbies[5].official = true;
        result = browse(model, options);
        check(result.lobbies[0]->id == 105 && result.lobbies[1]->id == 102 && result.lobbies[2]->players == 14,
              "Official servers are not first when sorting by players");
        result = browse(model, by_name);
        check(result.lobbies[0]->id == 102 && result.lobbies[1]->id == 105 && result.lobbies[2]->id == 100,
              "Official servers are not first when sorting by name");
        // Then the servers friends are in, which a search for a friend's name finds too.
        model.lobbies[9].friends = {"Ana"};
        result = browse(model, by_name);
        check(result.lobbies[0]->id == 102 && result.lobbies[1]->id == 105 && result.lobbies[2]->id == 109 && result.lobbies[3]->id == 100,
              "A friend's server is not next after the official ones");
        BrowserOptions by_friend;
        by_friend.query = "ANA";
        result = browse(model, by_friend);
        check(result.total == 1 && result.lobbies.front()->id == 109, "A friend's name did not find their server");
        model.lobbies[9].friends.clear();
        model.lobbies[2].dedicated = model.lobbies[2].official = false;
        model.lobbies[5].dedicated = model.lobbies[5].official = false;
        options.query = "missing";
        result = browse(model, options);
        check(result.total == 0 && result.lobbies.empty(), "Empty search");
        auto lobby = model.lobbies.front();
        check(can_join(model, lobby), "Available lobby");
        lobby.players = lobby.capacity;
        check(!can_join(model, lobby), "Full lobby must not join");
        lobby.players = 1; lobby.owner = model.local_id;
        check(!can_join(model, lobby), "Own lobby must not join");
        // A guest hops straight to another server; a host ends their session first, and nobody
        // joins the one they are in.
        lobby.owner = 123; model.active = true; model.hosting = true;
        check(!can_join(model, lobby), "A host must end their session before joining");
        model.hosting = false; model.public_lobby = 999;
        check(can_join(model, lobby), "A guest could not hop to another server");
        model.public_lobby = lobby.id;
        check(!can_join(model, lobby), "The server a player is in must not join");
        lobby.friends = {"Ana", "Ben", "Cy", "Di"};
        check(lobby_friends_text(lobby) == "Ana, Ben and 2 more" && lobby_friends_text(lobby, 4) == "Ana, Ben, Cy, Di",
              "Friends in a server were not named");
        lobby.friends.clear();
        model.active = false; model.public_lobby = 0; model.lobby_joining = true;
        check(!can_join(model, lobby), "Pending connection must not join");
        MultiplayerPlayer player;
        player.id = 88; player.epoch = 7; player.connected = true; player.name = "Skater";
        model.roster.push_back(player);
        model.active = true; model.lobby_joining = false;
        const auto identity = player_identity(player);
        check(selected_player(model, identity) == &model.roster.front(), "Player selection resolves the current connection");
        model.roster.front().name = "Renamed skater";
        check(selected_player(model, identity) == &model.roster.front(), "Renaming a player preserves selection");
        model.roster.front().epoch++;
        check(!selected_player(model, identity), "A reconnected player cannot inherit stale player options");
        const auto reconnected = player_identity(model.roster.front());
        check(selected_player(model, reconnected), "The new connection can be selected");
        model.roster.front().connected = false;
        check(!selected_player(model, reconnected), "Disconnected players have no player options");
        model.roster.front().connected = true;
        model.local_id = player.id;
        check(!selected_player(model, reconnected), "The local player has no removal options");
        model.local_id = 77; model.active = false;
        check(!selected_player(model, reconnected), "Ending the session clears player options");
        model.active = true; model.roster.clear();
        check(!selected_player(model, reconnected), "A removed player clears selection");
        check(caption("A\nB\tC") == "A B C", "Names cannot insert extra rows");
        check(caption("ab\xc3\xa9xyz", 3) == "ab...", "Truncate at UTF-8 boundary");
        check(map_name("Levels\\Isle_of_Grom") == "Isle of Grom", "Friendly map label");
        std::puts("Native menu browser checks passed.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
