#include "profile_update.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::profile {
void Store::Update::encode(Json& patch) {
    const auto& value = store_.value_;
    const auto& previous = store_.database_->document();
    const auto add = [&](std::string_view table, Json row, unsigned keys = 1) {
        const auto key = storage::row_key(row, keys);
        patch[table][key] = std::move(row);
    };
    const auto remove = [&](std::string_view table, const auto& predicate) {
        for (const auto& [key, row] : previous.at(table).items())
            if (predicate(row)) patch[table][key] = nullptr;
    };
    for (const auto& [table, keys] : records_) for (const auto& key : keys) {
        if (table == "player_settings") add(table, Json::array({"options", key, Json(value.bool_options.at(key)).dump()}), 2);
        else if (table == "entitlements") add(table, Json::array({key, value.entitlements.at(key) ? 1 : 0}));
        else if (table == "quests") add(table, Json::array({key, value.quests.at(key)}));
        else if (table == "neighborhood_ranks") add(table, Json::array({key, value.neighborhood_ranks.at(key)}));
        else if (table == "play_events") {
            const auto& event = value.play_events.at(key);
            detail::require(event.id == key && detail::valid_text(key) &&
                detail::valid_text(event.context, true) && event.count >= 0, "Invalid play event");
            add(table, Json::array({key, event.context, std::to_string(event.timestamp), event.count}));
        } else throw std::runtime_error("Unknown profile record update");
    }
    std::set<std::string> sections, loadouts, challenge_rows, challenge_catalog;
    std::map<std::string, std::set<std::string>> inventory;
    bool settings{}, card{}, score{};
    for (const auto& change : json_edits_) {
        auto path = change.path;
        if (change.root == &value.settings) { settings = true; continue; }
        std::string section;
        if (change.root == &value.customization) section = "customization";
        else { section = path.front(); path.erase(path.begin()); }
        sections.insert(section);
        if (path.empty()) continue;
        if (section == "customization" && path[0] == "loadouts" && path.size() > 1) loadouts.insert(path[1]);
        if (section == "customization" && path[0] == "player_card") card = true;
        if ((section == "customization" || section == "object_dropper") && path[0] == "inventory" && path.size() > 1)
            inventory[section == "customization" ? "cosmetic" : "object"].insert(path[1]);
        if (section == "progress" && path[0] == "rip_score") score = true;
        if (section == "challenges" && path.size() > 1) {
            if (path[0] == "progress") challenge_rows.insert(path[1]);
            if (path[0] == "catalog") challenge_catalog.insert(path[1]);
        }
    }
    // Preserve the structural/unknown fields without copying any item catalog,
    // inventory, loadout collection, or unrelated progression records.
    for (const auto& section : sections) {
        const auto& source = section == "customization" ? value.customization : value.extensions.at(section);
        Json residual = Json::object();
        for (const auto& [key, field] : source.items()) {
            const bool empty = ((section == "customization" || section == "object_dropper") &&
                    (key == "inventory" || key == "catalog")) ||
                (section == "customization" && key == "loadouts") ||
                (section == "challenges" && (key == "catalog" || key == "progress"));
            if (empty) residual[key] = Json::object();
            else if (!((section == "customization" && key == "player_card") ||
                       (section == "progress" && key == "rip_score"))) residual[key] = field;
        }
        if (section == "progress") for (auto key : {"quests", "play_events", "neighborhood_ranks"}) residual[key] = Json::object();
        add("profile_extensions", Json::array({section, residual.dump()}));
    }
    if (settings) {
        detail::validate_settings(value);
        (void)park_choices(value); (void)world_layer_choices(value);
        (void)world_controls(value); (void)graphics_controls(value); (void)profile::freecam_controller(value); (void)profile::freecam_controller_binding(value); (void)profile::freecam_binding(value); (void)profile::tp_to_freecam_binding(value); (void)profile::noclip_binding(value);
        (void)profile::forward_velocity_binding(value);
        (void)profile::vote_yes_binding(value); (void)profile::vote_no_binding(value);
        for (const auto& slot : action_binds) (void)profile::action_binding(value, slot.key);
        (void)profile::up_velocity_binding(value);
        (void)profile::offboard_up_velocity_binding(value);
        remove("player_settings", [](const Json& row) { return row[0] != "options"; });
        auto residual = value.settings;
        const auto flatten = [&](auto&& self, Json& node, const std::string& scope) -> void {
            for (auto it = node.items().begin(); it != node.items().end();) {
                if (it->second.is_object()) {
                    self(self, it->second, scope + "/" + database::escape(it->first)); ++it;
                } else {
                    add("player_settings", Json::array({scope, it->first, it->second.dump()}), 2);
                    it = node.items().erase(it);
                }
            }
        };
        flatten(flatten, residual, "/settings");
        add("profile_extensions", Json::array({"settings", residual.dump()}));
    }
    for (const auto& [kind, keys] : inventory) {
        const auto& rows = kind == "cosmetic" ? value.customization.at("inventory") :
            value.extensions.at("object_dropper").at("inventory");
        detail::require(rows.is_object() && rows.size() <= 8192, "Inventory exceeds limit");
        for (const auto& key : keys) {
            detail::require(detail::valid_text(key) && rows.at(key).is_boolean(), "Invalid inventory item");
            add("inventory", Json::array({kind, key, rows.at(key).get<bool>() ? 1 : 0}), 2);
        }
    }
    const auto outfit = [&](std::string_view table, const Json& key, Json row) {
        (void)decode_loadout(row);
        const auto format = row.at("recipe_format"), recipes = row.at("recipes");
        row.erase("recipe_format"); row.erase("recipes");
        add(table, Json::array({key, format, recipes.dump(), row.dump()}));
    };
    if (!loadouts.empty()) {
        const auto& rows = value.customization.at("loadouts");
        detail::require(rows.size() <= 10, "Invalid local preset collection");
        for (const auto& id : loadouts) outfit("cosmetic_loadouts", id, rows.at(id));
    }
    if (card) {
        validate_player_card(decode_loadout(value.customization.at("player_card")));
        outfit("player_card", 1, value.customization.at("player_card"));
    }
    if (score) {
        (void)rip_score(value);
        auto row = value.extensions.at("progress").at("rip_score");
        const auto total = row.at("value"), cap = row.at("cap"), level = row.at("level");
        row.erase("value"); row.erase("cap"); row.erase("level");
        add("rip_score", Json::array({1, total, cap, level, row.dump()}));
    }
    if (!challenge_rows.empty() || !challenge_catalog.empty()) detail::validate_challenges(value);
    for (const auto& id : challenge_catalog)
        add("catalog_entries", Json::array({"challenge", id, value.extensions.at("challenges").at("catalog").at(id).dump()}), 2);
    for (const auto& id : challenge_rows) {
        const auto& row = value.extensions.at("challenges").at("progress").at(id);
        auto extra = row; for (auto key : {"attempt", "completed_criteria", "receipt"}) extra.erase(key);
        add("challenge_progress", Json::array({id, row.at("attempt"), extra.dump()}));
        remove("challenge_goals", [&](const Json& old) { return old[0] == id; });
        const auto& goals = row.at("completed_criteria");
        for (std::size_t i = 0; i < goals.size(); ++i) add("challenge_goals", Json::array({id, goals[i], i}), 2);
        if (row.contains("receipt")) {
            const auto& receipt = row.at("receipt"); auto extras = receipt;
            for (auto key : {"attempt", "status", "completed_criteria", "grants"}) extras.erase(key);
            add("challenge_receipts", Json::array({id, receipt.at("attempt"), receipt.at("status"),
                receipt.at("completed_criteria").dump(), receipt.at("grants").dump(), extras.dump()}));
        }
    }
}
}
