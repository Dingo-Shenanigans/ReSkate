#include "Extension/Customization/developer_board_material.h"
#include "Extension/Customization/developer_hoodie_material.h"
#include "Extension/Multiplayer/developer_identity.h"
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

using namespace dingosdk::multiplayer;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
bool rejected(std::string_view json) {
    try {
        parse_identity_lists(json);
    } catch (const std::exception &) {
        return true;
    }
    return false;
}
constexpr std::uint64_t dev = 76561198000000001ULL, other_dev = 76561198000000002ULL, homie = 76561198000000003ULL,
                        creator = 76561198000000004ULL, stranger = 76561198000000005ULL;
using L = IdentityList;

void parsing() {
    // The answer as the backend sends it; a list comes back sorted, each player once.
    const auto lists = parse_identity_lists(
        R"({"categories":{"dev":["76561198000000002","76561198000000001","76561198000000002"],)"
        R"("homie":["76561198000000003"],"content_creator":["76561198000000004"]}})");
    check(lists[0] == std::vector{dev, other_dev} && lists[1] == std::vector{homie} && lists[2] == std::vector{creator} &&
              lists[3].empty() && lists[4].empty(),
          "The backend's answer was not read as its three lists, with nobody banned");
    // The ban list comes beside the categories; a player can be in both.
    const auto bans = parse_identity_lists(
        R"({"categories":{"dev":["76561198000000001"]},"banned":["76561198000000005","76561198000000001","76561198000000005"]})");
    check(bans[0] == std::vector{dev} && bans[static_cast<std::size_t>(L::banned)] == std::vector{dev, stranger}, "The ban list was not read");
    // A newer backend may list more categories and fields, and an older one fewer.
    const auto partial = parse_identity_lists(
        R"({"updated":"2026-10-04","categories":{"moderator":["76561198000000005"],"dev":["76561198000000001"]}})");
    check(partial[0] == std::vector{dev} && partial[1].empty() && partial[2].empty(),
          "An unknown category was read, or a missing one was not empty");
    // The team's own servers come beside them too: servers signed in with a login token, whose
    // Steam ID stays. Nobody is on it when the answer leaves it out.
    const auto servers = parse_identity_lists(
        R"({"categories":{},"official_servers":["85568392924040002","85568392924040001","85568392924040002"]})");
    check(servers[static_cast<std::size_t>(L::official_server)] == std::vector<std::uint64_t>{85568392924040001ULL, 85568392924040002ULL} &&
              bans[static_cast<std::size_t>(L::official_server)].empty(),
          "The official server list was not read");
    for (const char *wrong : {R"({"categories":{},"official_servers":["76561198000000001"]})",  // a player
                              R"({"categories":{},"official_servers":["90071992551410001"]})",  // an anonymous server
                              R"({"categories":{},"official_servers":["85568392920039424"]})",  // account 0
                              R"({"categories":{"dev":["85568392924040001"]}})"})                 // a server as a player
        check(rejected(wrong), "A list with the wrong kind of Steam ID was accepted: " + std::string(wrong));
    // Blocked servers: any server's ID, an anonymous one too; and the rule about login tokens.
    const auto blocks = parse_identity_lists(R"({"categories":{},"blocked_servers":["90071992551410001","85568392924040001"]})");
    check(blocks[static_cast<std::size_t>(L::blocked_server)] == std::vector<std::uint64_t>{85568392924040001ULL, 90071992551410001ULL} &&
              blocks[static_cast<std::size_t>(L::official_server)].empty() && servers[static_cast<std::size_t>(L::blocked_server)].empty(),
          "The blocked server list was not read");
    check(rejected(R"({"categories":{},"blocked_servers":["76561198000000001"]})"), "A player was accepted as a blocked server");
    check(parse_server_tokens_required(R"({"categories":{},"server_tokens_required":true})") &&
              !parse_server_tokens_required(R"({"categories":{},"server_tokens_required":false})") &&
              !parse_server_tokens_required(R"({"categories":{}})") && !parse_server_tokens_required(R"({"server_tokens_required":"yes"})"),
          "The login token rule was not read");
    // The ends of the range players' SteamID64s come from.
    check(!rejected(R"({"categories":{"dev":["76561197960265729","76561202255233023"]}})"), "A valid SteamID64 was refused");

    for (const std::string_view wrong : {
             "", "go away\n", "<html>Just a moment...</html>", "[]", "{}", R"({"categories":[]})",
             R"({"categories":{"dev":"76561198000000001"}})",   // not a list
             R"({"categories":{"dev":[76561198000000001]}})",   // a number, which JSON readers round
             R"({"categories":{"dev":["76561198000000001 "]}})", R"({"categories":{"dev":["+76561198000000001"]}})",
             R"({"categories":{"dev":["zee_x64"]}})", R"({"categories":{"dev":[""]}})",
             R"({"categories":{"dev":["76561197960265728"]}})", // account 0
             R"({"categories":{"dev":["76561202255233024"]}})", // past the last account
             R"({"categories":{"dev":["103582791429521408"]}})", // a Steam group
             R"({"categories":{"homie":["1"]}})", R"({"categories":{},"banned":"76561198000000005"})",
             R"({"categories":{},"banned":[76561198000000005]})", R"({"categories":{},"banned":["everyone"]})"})
        check(rejected(wrong), "An answer that is not the lists was accepted: " + std::string(wrong));
}

void lookup() {
    check(!reskate_developer(dev) && !identity_listed(homie, L::homie), "Someone was listed before any lists arrived");
    check(publish_identity_lists({{{dev, other_dev}, {homie}, {creator}}}), "The first lists did not count as a change");
    check(reskate_developer(dev) && reskate_developer(other_dev) && identity_listed(homie, L::homie) &&
              identity_listed(creator, L::content_creator),
          "A listed player was not found");
    check(!reskate_developer(homie) && !reskate_developer(creator) && !reskate_developer(stranger) &&
              !identity_listed(dev, L::homie) && !identity_listed(homie, L::content_creator),
          "A player was found in a list they are not in");
    check(!reskate_developer(0) && !identity_listed(dev, L::count), "No player, or no list, matched");
    check(!reskate_banned(dev) && !reskate_banned(stranger), "Someone is banned though the lists ban nobody");
    check(publish_identity_lists({{{dev, other_dev}, {homie}, {creator}, {}, {dev, stranger}}}) && reskate_banned(stranger) &&
              reskate_banned(dev) && reskate_developer(dev) && !reskate_banned(other_dev) && !reskate_banned(0),
          "The ban list did not ban exactly the players on it");
    check(publish_identity_lists({{{dev, other_dev}, {homie}, {creator}}}) && !reskate_banned(stranger), "A lifted ban stayed");
    check(!publish_identity_lists({{{dev, other_dev}, {homie}, {creator}}}), "The same lists counted as a change");
    // The one mark a player carries: a developer's before a content creator's before a homie's.
    check(publish_identity_lists({{{dev}, {dev, homie, creator}, {dev, creator}}}) && identity_mark(dev) == L::developer &&
              identity_mark(creator) == L::content_creator && identity_mark(homie) == L::homie && !identity_mark(stranger) &&
              !identity_mark(0),
          "A player's mark is not the first list they are on");
    // Centrix: read from the answer as its own list, and its mark comes after a developer's and
    // before a content creator's.
    const auto centrix = parse_identity_lists(R"({"categories":{"centrix":["76561198000000005"],"dev":["76561198000000001"]}})");
    check(centrix[static_cast<std::size_t>(L::centrix)] == std::vector{stranger} && centrix[static_cast<std::size_t>(L::banned)].empty(),
          "The Centrix list was not read as its own");
    check(publish_identity_lists({{{dev}, {homie, stranger}, {creator, stranger}, {dev, creator, stranger}}}) &&
              identity_mark(dev) == L::developer && identity_mark(stranger) == L::centrix && identity_mark(creator) == L::centrix &&
              identity_mark(homie) == L::homie && !reskate_banned(stranger),
          "Centrix is not marked after a developer and before a content creator");
    // Staff: its own list too, marked after a developer and before everyone else.
    const auto staff = parse_identity_lists(R"({"categories":{"staff":["76561198000000005"],"centrix":["76561198000000005"]}})");
    check(staff[static_cast<std::size_t>(L::staff)] == std::vector{stranger} && staff[static_cast<std::size_t>(L::centrix)] == std::vector{stranger} &&
              staff[static_cast<std::size_t>(L::developer)].empty(),
          "The staff list was not read as its own");
    check(publish_identity_lists({{{dev}, {homie}, {creator}, {stranger}, {}, {}, {dev, homie, stranger}}}) &&
              identity_mark(dev) == L::developer && identity_mark(stranger) == L::staff && identity_mark(homie) == L::staff &&
              identity_mark(creator) == L::content_creator,
          "Staff is not marked after a developer and before the other lists");
    {
        // Their items go green, and the rainbow is a style they (and a developer) can pick: it
        // travels as a solid colour older builds show, and nobody else's item takes it.
        const MarkStyle usual{}, rainbow{MarkMode::solid, {0x10, 0x80, 0x38}, rainbow_marker, 0};
        const auto green = dingosdk::developer_hoodie_detail::item_animation(L::staff, usual);
        check(green.on && !green.rainbow && green.stops[1][1] > green.stops[1][0] && green.stops[1][1] > green.stops[1][2],
              "The staff's items are not green");
        check(valid_mark_style(rainbow) && rainbow_style(rainbow) && !rainbow_style(usual) &&
                  !rainbow_style({MarkMode::gradient, {}, rainbow_marker, 0}),
              "The rainbow style is not a solid colour with its marker");
        check(dingosdk::developer_hoodie_detail::item_animation(L::staff, rainbow).rainbow && dingosdk::developer_hoodie_detail::item_animation(L::developer, rainbow).rainbow,
              "A member of staff or a developer could not pick the rainbow");
        const auto homies = dingosdk::developer_hoodie_detail::item_animation(L::homie, rainbow);
        check(homies.on && !homies.rainbow && homies.stops[0] == homies.stops[2], "Someone else's item took the rainbow");
    }
    check(publish_identity_lists({{{dev}, {dev, homie, creator}, {dev, creator}}}), "The lists did not go back");
    // Their tag and their items are each their own to hide.
    check(own_tag_shown() && own_items_shown(), "A player's tag or items start hidden");
    show_own_tag(false);
    check(!own_tag_shown() && own_items_shown() && identity_mark(dev) == L::developer,
          "Hiding their own tag did not take, hid their items, or changed the lists");
    show_own_tag(true);
    show_own_items(false);
    check(own_tag_shown() && !own_items_shown(), "Hiding their own items did not take, or hid their tag");
    show_own_items(true);
    check(publish_identity_lists({{{dev, other_dev}, {homie}, {creator}}}), "The lists did not go back");
    // Removing someone in the panel takes their mark away at the next refresh.
    check(publish_identity_lists({{{dev}, {}, {}}}) && reskate_developer(dev) && !reskate_developer(other_dev) &&
              !identity_listed(homie, L::homie),
          "Replaced lists kept a player who was removed");
}
// The hoodie and board: what a list gives a cosmetic, and what a player makes of it on the Special page.
void items() {
    using namespace dingosdk::developer_hoodie_detail;
    using dingosdk::multiplayer::MarkMode;
    using dingosdk::multiplayer::MarkStyle;
    using dingosdk::multiplayer::MarkStyles;
    const MarkStyle standard{};
    const auto spectrum = item_animation(L::developer, standard), red = item_animation(L::content_creator, standard),
               gold = item_animation(L::homie, standard);
    check(spectrum.on && spectrum.rainbow && red.on && !red.rainbow && gold.on && !gold.rainbow &&
              !item_animation(std::nullopt, standard).on,
          "A list did not give its animation, or a player on none got one");
    check(item_color(spectrum, 0) == rainbow(0) && item_color(spectrum, 3000) == rainbow(3000), "The developer's rainbow changed");
    // Red and gold go from a deep shade to a bright one, on into the colour next to it, and back
    // every three seconds.
    for (const auto *animation : {&red, &gold}) {
        const auto deep = item_color(*animation, 0), bright = item_color(*animation, 750), far = item_color(*animation, 1500);
        check(item_color(*animation, 3000) == deep && item_color(*animation, 4500) == far, "An animation does not repeat every three seconds");
        check(bright[0] > deep[0] * 2 && far[0] >= bright[0], "An animation does not go from deep to bright");
        if (animation == &red) {
            // A content creator's: red, then a reddish pink.
            check(bright[1] < bright[0] * .1f && bright[2] < bright[0] * .1f, "Red is not red");
            check(far[2] > bright[2] * 3 && far[2] < far[0] * .4f && far[1] < far[0] * .15f, "Red does not go on into a reddish pink");
        } else {
            // A homie's: gold, then yellow.
            check(bright[1] > bright[0] * .5f && bright[1] < bright[0] * .75f && bright[2] < bright[0] * .15f, "Gold is not gold");
            check(far[1] > bright[1] * 1.2f && far[1] < far[0] && far[2] < far[1] * .4f, "Gold does not go on into yellow");
        }
        for (std::uint64_t at = 0; at < 6000; at += 125) {
            // The material's gamut, and red always the strongest part of the colour.
            const auto color = item_color(*animation, at);
            check(color[0] <= .801f && color[1] > 0 && color[2] > 0 && color[0] > color[1] && color[0] > color[2],
                  "An animation leaves its colours");
        }
    }

    // A cosmetic turned off, and one given the player's own two colours.
    const MarkStyle off{MarkMode::off, {}, {}, 0}, own{MarkMode::gradient, {255, 0, 0}, {0, 0, 255}, 0};
    check(!item_animation(L::developer, off).on && !item_animation(L::homie, off).on, "A cosmetic turned off still animates");
    const auto custom = item_animation(L::homie, own);
    const auto first = item_color(custom, 0), last = item_color(custom, 1500), half = item_color(custom, 750);
    check(custom.on && !custom.rainbow && first[0] > .7f && first[2] < .01f && last[2] > .7f && last[0] < .01f && half[0] > .3f &&
              half[2] > .3f && item_color(custom, 3000) == first,
          "A player's own colours are not what their cosmetic moves between");
    // A developer can leave the rainbow for colours of their own; no style gives anyone else the rainbow.
    check(!item_animation(L::developer, own).rainbow && item_animation(L::developer, own).on, "A developer cannot have their own colours");
    // One colour of their own stands still, at any speed.
    const auto still = item_animation(L::homie, {MarkMode::solid, {0, 255, 0}, {9, 9, 9}, 2});
    check(still.on && !still.rainbow && item_color(still, 0) == material_color({0, 255, 0}) && item_color(still, 0)[1] > .7f &&
              item_color(still, 777) == item_color(still, 0) && item_color(still, 1500) == item_color(still, 0),
          "A solid colour is not the colour picked, or does not stand still");
    // A player on no list gets nothing, whatever style their packets carry: the styles only
    // shape what a list already gives.
    for (const auto mode : {MarkMode::standard, MarkMode::gradient, MarkMode::solid})
        for (const std::uint8_t speed : {std::uint8_t{0}, std::uint8_t{2}})
            check(!item_animation(std::nullopt, {mode, {255, 0, 0}, {0, 0, 255}, speed}).on,
                  "A player on no list got colours by asking for them");
    for (const auto mode : {MarkMode::standard, MarkMode::gradient, MarkMode::solid})
        check(!item_animation(L::content_creator, {mode, {}, {}, 0}).rainbow && !item_animation(L::homie, {mode, {}, {}, 0}).rainbow,
              "A style gave the rainbow to someone who is not a developer");
    // Slow takes twice as long and fast half, for the rainbow too.
    check(item_animation(L::homie, {MarkMode::gradient, {}, {}, 1}).period == 6000 &&
              item_animation(L::homie, {MarkMode::standard, {}, {}, 2}).period == 1500 &&
              item_animation(L::developer, {MarkMode::standard, {}, {}, 1}).period == 16000 &&
              item_animation(L::developer, {MarkMode::standard, {}, {}, 2}).period == 4000 &&
              item_color(item_animation(L::developer, {MarkMode::standard, {}, {}, 2}), 1000) == rainbow(2000),
          "The speed is not half or twice the usual");
    // The pickers start from colours close to the list's own.
    const auto picks = standard_picks(L::content_creator);
    const auto start = material_color(picks.first), finish = material_color(picks.second);
    check(std::abs(start[0] - .2f) < .02f && start[1] < .02f && std::abs(finish[0] - .8f) < .01f && std::abs(finish[2] - .13f) < .02f,
          "A content creator's pickers do not start from their red");

    // The styles are kept in the profile as text.
    MarkStyles styles{};
    styles[0] = own, styles[2] = off, styles[4] = {MarkMode::gradient, {1, 2, 3}, {254, 253, 252}, 2};
    styles.back() = {MarkMode::solid, {7, 8, 9}, {}, 0};
    const auto text = mark_styles_text(styles);
    check(text.size() == styles.size() * 16 && parse_mark_styles(text) == styles && parse_mark_styles(mark_styles_text({})) == MarkStyles{},
          "The styles did not come back from their text");
    check(!parse_mark_styles("") && !parse_mark_styles("00") && !parse_mark_styles(std::string(text.size(), 'f')) &&
              !parse_mark_styles(std::string(text.size(), 'g')) && !parse_mark_styles(text + "00") &&
              !parse_mark_styles(std::string_view(text).substr(16)),
          "Text that is not styles was read as some");
    // Every marked cosmetic has a name, the skater's before the board's.
    check(dingosdk::multiplayer::mark_item_names.size() == slots.size() + dingosdk::developer_board_detail::part_count &&
              dingosdk::multiplayer::mark_item_names[slots.size()] == "Deck" && slots[2] == "CAS_Footwear_Slot",
          "The marked cosmetics and their names do not line up");
}

// Whatever a player wears is coloured where the item colours itself, each cosmetic on its own;
// turning one off (the Special page, or a list that no longer has the player) puts its own
// colours back, and publishes them the extra times the renderer needs to show them.
void settling() {
    using namespace dingosdk::developer_hoodie_detail;
    using dingosdk::multiplayer::MarkMode;
    const auto red = item_animation(L::content_creator, {}), gold = item_animation(L::homie, {});
    const ItemAnimation none;
    using Skater = std::array<ItemAnimation, slots.size()>;
    const auto wearing = [&](const ItemAnimation &top, const ItemAnimation &shoes) {
        Skater out{};
        out[0] = top, out[2] = shoes;
        return out;
    };
    const Skater bare{};
    const Color black{.02f, .02f, .02f}, navy{.02f, .02f, .2f}, teal{.02f, .3f, .3f};
    const auto bound = [](std::uintptr_t item, std::uintptr_t node, std::uint64_t key, const Color &color, std::uint8_t part) {
        return Binding{item, item + 40, node, {0, sizeof(Color), 1, 1, key}, color, part};
    };
    // What the game holds for each colour, read afresh each tick as in game: a top with two
    // regions it colours and one it leaves to its texture, and shoes that colour none of theirs.
    std::array<Color, 4> held{black, navy, neutral, neutral};
    dingosdk::DeveloperHoodieState state;
    Materials live;
    live.component = 1, live.appearance = 2, live.regions_only = true;
    live.bindings = {bound(10, 100, color_keys[0], black, 0), bound(10, 101, color_keys[3], navy, 0),
                     bound(10, 102, color_keys[7], neutral, 0), bound(11, 103, color_keys[0], neutral, 2)};
    std::vector<std::uintptr_t> published;
    int writes{};
    auto write = [&](const Binding &binding, const Color &color) {
        held[binding.node - 100] = color;
        ++writes;
        return true;
    };
    auto publish = [&](std::uintptr_t item) { published.push_back(item); };
    const auto tick = [&](const Skater &animations, std::uint64_t at) {
        for (std::size_t i = 0; i < held.size(); ++i) live.bindings[i].color = held[i];
        animate(state, live, 5, 6, animations, at, write, publish);
    };
    using Items = std::vector<std::uintptr_t>;
    tick(wearing(red, none), 0);
    tick(wearing(red, none), 700);
    check(state.colors.size() == 2 && held[0] == item_color(red, 700) && held[1] == held[0] && held[2] == neutral &&
              held[3] == neutral && published == Items{10, 10},
          "The top's own regions were not coloured, or more than them was");
    // Shoes that colour none of their regions take the colour on all of them.
    tick(wearing(red, gold), 800);
    check(held[3] == item_color(gold, 800) && held[2] == neutral && held[0] == item_color(red, 800) && state.colors.size() == 3,
          "An item with no colours of its own was not coloured, or cosmetics did not each take their own");
    published.clear();
    tick(bare, 900);
    check(held[0] == black && held[1] == navy && held[3] == neutral && published == Items{10, 11},
          "Turning it off did not bring the items' own colours back");
    for (std::uint64_t at = 901; at < 911; ++at) tick(bare, at);
    check(published.size() == 2 * (1 + settle_publishes) && state.colors.empty() && held[0] == black && held[1] == navy,
          "The own colours were not published the extra times, or the items were not left alone after");
    // A colour the game itself sets meanwhile is the game's to keep.
    tick(wearing(gold, none), 1000);
    held[0] = teal;
    tick(bare, 1100);
    check(held[0] == teal && held[1] == navy, "Turning it off overwrote a colour the game had set");
    // And it comes back on with the own colours still known.
    for (std::uint64_t at = 1200; at < 1210; ++at) tick(bare, at);
    tick(wearing(red, none), 2000);
    tick(bare, 2100);
    check(held[0] == teal && held[1] == navy, "A second round lost the top's own colours");
    for (std::uint64_t at = 2200; at < 2210; ++at) tick(bare, at);

    // A colour that stands still is written once, and published until the renderer shows it.
    const auto still = item_animation(L::homie, {MarkMode::solid, {0, 255, 0}, {}, 0});
    published.clear(), writes = 0;
    for (std::uint64_t at = 3000; at < 3010; ++at) tick(wearing(still, none), at);
    check(held[0] == item_color(still, 0) && held[1] == held[0] && writes == 2 && published == Items(1 + settle_publishes, 10),
          "A solid colour was written again and again, or not published until it showed");
    // While a recipe is on its way nothing is touched, and what was saved is kept.
    live.pending = true;
    tick(bare, 4000);
    check(held[0] == item_color(still, 0) && state.colors.size() == 2 && writes == 2, "A pending recipe was written to");
    live.pending = false;
    tick(bare, 4100);
    check(held[0] == teal && held[1] == navy, "The own colours were lost while a recipe was pending");

    // The board does the same, each of its parts on its own: here the deck and the wheels, whose
    // material has no colour of its own until one is added.
    namespace board = dingosdk::developer_board_detail;
    std::array<Color, 2> painted{black, board::shader_default};
    dingosdk::DeveloperBoardState deck;
    Materials parts;
    parts.component = 1, parts.appearance = 2;
    parts.bindings = {Binding{30, 10, 100, board::parameter_key(board::base_color, 1), black, 0},
                      Binding{31, 11, 0, board::parameter_key(board::base_color, 1), board::shader_default, 3}};
    int shown{};
    bool added{};
    auto paint = [&](const Binding &binding, const Color &color) {
        if (!binding.node) added = true; // the engine adds the missing parameter
        painted[binding.part ? 1 : 0] = color;
        return true;
    };
    auto show = [&](std::uintptr_t) { ++shown; };
    using Board = std::array<ItemAnimation, board::part_count>;
    const auto roll = [&](const ItemAnimation &on_deck, const ItemAnimation &on_wheels, std::uint64_t at) {
        parts.bindings[0].color = painted[0], parts.bindings[1].color = painted[1];
        parts.bindings[1].node = added ? 101 : 0;
        animate(deck, parts, 5, 6, Board{on_deck, none, none, on_wheels}, at, paint, show);
    };
    roll(gold, red, 0);
    roll(gold, red, 700);
    check(deck.colors.size() == 2 && added && painted[0] == item_color(gold, 700) && painted[1] == item_color(red, 700),
          "The board's parts did not each take their own animation, or a missing colour was not added");
    // The wheels alone turned off: back to the shaders' default, while the deck goes on.
    for (std::uint64_t at = 800; at < 810; ++at) roll(gold, none, at);
    check(painted[0] == item_color(gold, 809) && painted[1] == board::shader_default && deck.colors.size() == 1,
          "Turning one part off stopped another, or did not stop it");
    shown = 0;
    for (std::uint64_t at = 900; at < 910; ++at) roll(none, none, at);
    check(painted[0] == black && painted[1] == board::shader_default && shown == 1 + settle_publishes && deck.colors.empty(),
          "Turning it off did not bring the board's own colours back and publish them the extra times");
}

// --live <SteamID64>: the deployed backend, read the way the game reads it. Not
// part of the test run, which stays off the network.
int live(const char *player) {
    const auto id = std::strtoull(player, nullptr, 10);
    for (int waited = 0; waited < 150 && !reskate_developer(id); ++waited) {
        refresh_identity_lists();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << player << (reskate_developer(id) ? " is" : " is not") << " a developer in the lists the backend sent\n";
    return reskate_developer(id) ? 0 : 1;
}
} // namespace

int main(int count, char **arguments) {
    if (count == 3 && std::string_view(arguments[1]) == "--live") return live(arguments[2]);
    parsing();
    lookup();
    items();
    settling();
    std::cout << "developer identity tests passed\n";
}
