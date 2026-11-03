#include "multiplayer_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/Assets/peer_cosmetics_fetch.h"
#include "Extension/Multiplayer/Remote/outfit_filter.h"
#include <algorithm>

// MULTIPLAYER > OUTFITS: what of other players' outfits shows on this screen (outfit_filter.h),
// and the cosmetics fetched because somebody wore them (peer_cosmetics_fetch.h). Nothing here
// changes what anybody else sees.
namespace dingosdk::overlay::menu::multiplayer_detail {
namespace {
namespace filter = multiplayer::outfit_filter;
namespace fetched = peer_cosmetics_fetch;
namespace store = mods::peer_cosmetics;

std::string lower(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string megabytes(std::uint64_t bytes) {
    const auto mb = (bytes + (1 << 19)) >> 20;
    return mb ? std::to_string(mb) + " MB" : "under 1 MB";
}
std::string ago(std::int64_t then) {
    const auto days = std::max<std::int64_t>(0, store::now_seconds() - then) / (24 * 60 * 60);
    return days == 0 ? "today" : days == 1 ? "yesterday" : std::to_string(days) + " days ago";
}
// The rules with one change, saved; the outfits on screen follow within a few frames.
template <class Change> void change(SkateMenu &menu, Change &&edit) {
    auto rules = *filter::rules();
    edit(rules);
    if (!filter::set_rules(std::move(rules))) feedback(menu, "The outfit filter could not be saved; it applies until the game closes.");
}

void filter_card(SkateMenu &menu) {
    const auto rules = filter::rules();
    begin_card(menu, "outfits-filter", "OTHER PLAYERS' OUTFITS", "Only changes what you see");
    bool mod_items = rules->mod_items;
    if (toggle_row(menu, "Mod items", "Show items from mods on other players. Off: everyone wears the game's own items here, "
                                      "and an item from a mod shows as the default for its slot, or nothing.", mod_items))
        change(menu, [&](filter::Rules &r) { r.mod_items = mod_items; });
    note("Items you hide show as the default for their slot. An item you do not have shows that way already.");
    end_card();
}

// A player's items from mods, a row each: whether it shows, its name and mod, and its buttons on
// the same row (the name is cut short before them).
void item_rows(SkateMenu &menu, const filter::Rules &rules, const std::vector<filter::WornItem> &items) {
    if (items.empty()) {
        note("Nothing from a mod, or their outfit has not been put on here yet.");
        return;
    }
    if (!ImGui::BeginTable("items", 3, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, px(118));
    ImGui::TableSetupColumn("##item");
    ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed, px(196));
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (item.hidden) tag(menu, "HIDDEN", skate_theme::danger);
        else if (!item.installed) tag(menu, "NOT INSTALLED", skate_theme::warning);
        else tag(menu, "SHOWN", skate_theme::good);
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        cell_text(item.title + (item.mod_title.empty() ? "" : "  -  " + item.mod_title));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.key.c_str());
        ImGui::TableNextColumn();
        const bool item_hidden = rules.items.contains(lower(item.key));
        if (ImGui::Button(item_hidden ? "Show item" : "Hide item", ImVec2(px(92), 0)))
            change(menu, [&](filter::Rules &r) {
                if (item_hidden) r.items.erase(lower(item.key));
                else r.items.insert(lower(item.key));
            });
        if (!item.mod.empty()) {
            ImGui::SameLine();
            const bool mod_hidden = rules.mods.contains(item.mod);
            if (ImGui::Button(mod_hidden ? "Show mod" : "Hide mod", ImVec2(px(92), 0)))
                change(menu, [&](filter::Rules &r) {
                    if (mod_hidden) r.mods.erase(item.mod);
                    else r.mods.insert(item.mod);
                });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every item from %s, on everyone.", item.mod_title.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void players_card(SkateMenu &menu, const MultiplayerModel &mp) {
    const auto rules = filter::rules();
    // Everyone but this player, by name whatever its case (then by ID, for players named alike).
    struct Row {
        const MultiplayerPlayer *peer;
        std::string name;
        std::vector<filter::WornItem> items;
    };
    std::vector<Row> rows;
    for (const auto &peer : mp.roster)
        if (peer.id != mp.local_id) rows.push_back({&peer, lower(peer.name), filter::worn(peer.id)});
    std::ranges::sort(rows, [](const Row &a, const Row &b) { return a.name != b.name ? a.name < b.name : a.peer->id < b.peer->id; });

    const auto count = std::to_string(rows.size()) + (rows.size() == 1 ? " player" : " players");
    begin_card(menu, "outfits-players", "PLAYERS", count.c_str());
    if (rows.empty()) {
        note("Players appear here once you are in a session with them.");
        end_card();
        return;
    }
    // A search finds players by name, or by what they wear: an item's or its mod's name. A player
    // found by what they wear opens, showing only the items that matched.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##outfits-search", "Search players, items or mods", menu.outfits_search.data(), menu.outfits_search.size());
    const auto query = lower(menu.outfits_search.data());
    const auto has = [&](const std::string &text) { return lower(text).find(query) != std::string::npos; };
    std::size_t shown{};
    for (const auto &row : rows) {
        const auto &peer = *row.peer;
        std::vector<filter::WornItem> items = row.items;
        bool by_items{};
        if (!query.empty() && !has(peer.name)) {
            std::erase_if(items, [&](const filter::WornItem &item) { return !has(item.title) && !has(item.mod_title) && !has(item.key); });
            if (items.empty()) continue;
            by_items = true;
        }
        ++shown;
        ImGui::PushID(std::to_string(peer.id).c_str());
        const bool plain = rules->players.contains(peer.id);
        const auto title = peer.name + "  (" + std::to_string(row.items.size()) + (row.items.size() == 1 ? " mod item)" : " mod items)");
        // The row folds open on its arrow and name only, as tall as the button beside it, so the
        // button is never under the fold's (or the next row's) click area.
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        if (by_items) ImGui::SetNextItemOpen(true);
        const bool open = ImGui::TreeNodeEx("##player", ImGuiTreeNodeFlags_AllowOverlap, "%s", title.c_str());
        ImGui::SameLine(right - px(170));
        if (ImGui::Button(plain ? "Show mod items" : "Game items only", ImVec2(px(170), 0)))
            change(menu, [&](filter::Rules &r) {
                if (plain) r.players.erase(peer.id);
                else r.players[peer.id] = peer.name;
            });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", plain ? "Show this player's mod items again." : "Show this player in the game's own items only.");
        if (open) {
            item_rows(menu, *rules, items);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!shown) note("No player's name, items or mods match the search.");
    end_card();
}

void hidden_card(SkateMenu &menu) {
    const auto rules = filter::rules();
    if (rules->items.empty() && rules->mods.empty() && rules->players.empty()) return;
    begin_card(menu, "outfits-hidden", "HIDDEN", "Kept between sessions");
    const auto row = [&](const std::string &label, const char *kind, auto &&undo) {
        ImGui::PushID((std::string(kind) + label).c_str());
        tag(menu, kind, skate_theme::white);
        ImGui::SameLine(0, px(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - px(80));
        if (ImGui::Button("Show", ImVec2(px(80), 0))) change(menu, undo);
        ImGui::PopID();
    };
    for (const auto &key : rules->items) row(filter::readable_key(key), "ITEM", [&](filter::Rules &r) { r.items.erase(key); });
    for (const auto &mod : rules->mods) row(mod, "MOD", [&](filter::Rules &r) { r.mods.erase(mod); });
    for (const auto &[id, name] : rules->players)
        row(name.empty() ? std::to_string(id) : name, "PLAYER", [&, id = id](filter::Rules &r) { r.players.erase(id); });
    end_card();
}

void fetched_card(SkateMenu &menu) {
    const auto view = fetched::view();
    const auto state = fetched::status();
    begin_card(menu, "outfits-fetched", "FETCHED COSMETICS",
               (std::to_string(view.mods.size()) + (view.mods.size() == 1 ? " mod, " : " mods, ") + megabytes(view.bytes)).c_str());
    note("Mods fetched because another player wore them. They are deleted when the game starts, as chosen here. "
         "Keep one to make it one of your own mods.");
    auto policy = view.policy;
    int how = static_cast<int>(policy.cleanup);
    bool changed{};
    field(menu, "Delete them", "When the game deletes fetched cosmetics. Using one, or seeing anyone wear one, counts as using it.");
    changed |= choice(menu, "outfits-cleanup", how, {"Unused", "Over a size", "Every launch", "Never"}, true, 0,
                      {"Delete each one nobody wore for a number of days.", "Keep them under a size, deleting the least recently seen.",
                       "Delete all of them every time the game starts.", "Keep them all."});
    policy.cleanup = static_cast<store::Cleanup>(how);
    if (policy.cleanup == store::Cleanup::unused) {
        field(menu, "Unused for", "Days without anyone wearing its items.");
        int days = static_cast<int>(policy.days);
        slider_int("##outfits-days", &days, 1, 90, "%d days", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        policy.days = static_cast<unsigned>(days);
    } else if (policy.cleanup == store::Cleanup::size) {
        field(menu, "At most", "How much fetched cosmetics may take on disk.");
        int mb = static_cast<int>(policy.max_mb);
        slider_int("##outfits-size", &mb, 100, 16384, "%d MB", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        policy.max_mb = static_cast<std::uint64_t>(mb);
    }
    if (changed && !(policy == view.policy) && !fetched::set_policy(policy)) feedback(menu, "The setting could not be saved.");

    if (!view.mods.empty() && ImGui::BeginTable("outfits-fetched", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthFixed, px(110));
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, px(80));
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, px(170));
        ImGui::TableHeadersRow();
        for (const auto &mod : view.mods) {
            ImGui::PushID(mod.folder.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            cell_text(mod.title + (mod.from.empty() ? "" : "  (" + mod.from + ")"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s", mod.folder.c_str(), mod.version.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (mod.remove) tag(menu, "NEXT LAUNCH", skate_theme::danger);
            else ImGui::TextUnformatted(ago(mod.seen).c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(megabytes(mod.bytes).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Button("Keep", ImVec2(px(80), 0))) {
                if (fetched::keep(mod.folder)) feedback(menu, (mod.title + " is one of your own mods now.").c_str());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make it one of your own mods: it is never deleted for being unused.");
            ImGui::SameLine();
            if (ImGui::Button(mod.remove ? "Undo" : "Delete", ImVec2(px(80), 0))) fetched::remove_at_next_launch(mod.folder, !mod.remove);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", mod.remove ? "Keep it after all." : "Delete it when the game next starts (its files are in use now).");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!state.line.empty()) {
        std::string line = "Last fetch: " + state.line;
        if (state.busy && state.total) line += " (" + megabytes(state.received) + " of " + megabytes(state.total) + ")";
        note(line.c_str());
    }
    end_card();
}
} // namespace

void outfits_page(SkateMenu &menu, const Model &model) {
    filter_card(menu);
    players_card(menu, model.multiplayer);
    hidden_card(menu);
    fetched_card(menu);
}
} // namespace dingosdk::overlay::menu::multiplayer_detail
