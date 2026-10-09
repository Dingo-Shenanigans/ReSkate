#include "local_profile.h"
#include "profile_internal.h"
#include "profile_update.h"
#include "Extension/Progression/fast_travel_unlock.h"
#include <algorithm>

namespace dingosdk::profile {
using namespace detail;
std::optional<CosmeticLoadout> Store::cosmetic_loadout(std::string_view id) const {
    std::lock_guard lock(mutex_);
    if (!value_.customization.contains("loadouts")) return std::nullopt;
    const auto& rows = value_.customization.at("loadouts");
    const auto it = rows.find(std::string(id));
    return it == rows.end() ? std::nullopt : std::optional<CosmeticLoadout>(decode_loadout(*it));
}
void Store::save_cosmetic_loadout(std::string_view id, const CosmeticLoadout& loadout) {
    std::lock_guard lock(mutex_);
    require(valid_text(id), "Invalid local preset identifier");
    const auto encoded = encode_loadout(loadout);
    Update update(*this);
    auto& row = update.json(value_.customization, {"loadouts", id});
    if (!row.is_null() && decode_loadout(row) == loadout) return;
    if (row.is_null()) row = encoded;
    else for (const auto& [key, value] : encoded.items()) row[key] = value;
    update.commit();

}
std::optional<CosmeticLoadout> Store::player_card() const {
    std::lock_guard lock(mutex_);
    const auto it = value_.customization.find("player_card");
    return it == value_.customization.end() ? std::nullopt : std::optional<CosmeticLoadout>(decode_loadout(*it));
}
void Store::save_player_card(const CosmeticLoadout& value) {
    validate_player_card(value);
    const auto encoded = encode_loadout(value);
    std::lock_guard lock(mutex_);
    Update update(*this);
    auto& card = update.json(value_.customization, {"player_card"});
    if (!card.is_null() && decode_loadout(card) == value) return;
    if (card.is_null()) card = encoded;
    else for (const auto& [key, field] : encoded.items()) card[key] = field;
    update.commit();

}
void Store::seed_cosmetic_inventory(const std::vector<std::string>& keys) {
    std::lock_guard lock(mutex_);
    const auto enabled = value_.bool_options.find(unlock_cosmetics_option);
    if (enabled == value_.bool_options.end() || !enabled->second) return;
    Update update(*this);
    update.json(value_.customization, {"catalog"}, true);
    const auto& inventory = update.json(value_.customization, {"inventory"}, true);
    for (const auto& key : keys) {
        require(valid_text(key), "Invalid installed cosmetic key");
        const auto found = inventory.find(key);
        if (found == inventory.end() || !found->is_boolean() || !found->get<bool>())
            update.json(value_.customization, {"inventory", key}) = true;
    }
    update.commit();

}
void Store::seed_object_inventory(const std::vector<std::string>& keys) {
    std::lock_guard lock(mutex_);
    const auto enabled = value_.bool_options.find(unlock_objects_option);
    if (enabled == value_.bool_options.end() || !enabled->second) return;
    Update update(*this);
    update.json(value_.extensions, {"object_dropper", "catalog"}, true);
    const auto& inventory = update.json(value_.extensions, {"object_dropper", "inventory"}, true);
    for (const auto& key : keys) {
        require(valid_text(key), "Invalid installed object key");
        const auto found = inventory.find(key);
        if (found == inventory.end() || !found->is_boolean() || !found->get<bool>())
            update.json(value_.extensions, {"object_dropper", "inventory", key}) = true;
    }
    update.commit();

}
void Store::reconcile_inventory(const std::vector<std::string>& cosmetics, const std::vector<std::string>& objects) {
    std::lock_guard lock(mutex_);
    const auto cosmetic_inventory = value_.customization.value("inventory", dingosdk::Json::object());
    const auto object_inventory =
        value_.extensions.value("object_dropper", dingosdk::Json::object()).value("inventory", dingosdk::Json::object());
    const auto owned = [](const dingosdk::Json& inventory, const std::string& key) {
        const auto found = inventory.find(key);
        return found != inventory.end() && found->is_boolean() && found->get<bool>();
    };
    Update update(*this);
    bool changed{};
    for (const auto& key : cosmetics)
        if (owned(cosmetic_inventory, key)) {
            update.json(value_.customization, {"inventory", key}) = false;
            changed = true;
        }
    for (const auto& key : objects)
        if (owned(object_inventory, key)) {
            update.json(value_.extensions, {"object_dropper", "inventory", key}) = false;
            changed = true;
        }
    if (changed) update.commit();
}
std::uint32_t Store::selected_cosmetic_preset() const {
    std::lock_guard lock(mutex_);
    return value_.customization.value("selected_preset_index", std::uint32_t{0});
}
void Store::set_selected_cosmetic_preset(std::uint32_t index) {
    std::lock_guard lock(mutex_);
    require(index < 10, "Invalid cosmetic preset index");
    Update update(*this);
    update.json(value_.customization, {"selected_preset_index"}) = index;
    update.commit();

}
void Store::set_bus_stop_state(unsigned number, unsigned state) {
    std::lock_guard lock(mutex_);
    const auto* stop = number <= 255 ? fixed_bus_stop_catalog_entry(static_cast<std::uint8_t>(number)) : nullptr;
    require(stop && state <= 2, "Invalid bus stop or state");
    Update update(*this);
    update.record(value_.bool_options, unlock_bus_stops_option, "player_settings") = true;
    update.record(value_.entitlements, stop->visibility_entitlement, "entitlements") = state != 0;
    update.record(value_.entitlements, stop->collected_entitlement, "entitlements") = state == 2;
    const auto key = std::string(stop->collect_event);
    auto& event = update.record(value_.play_events, key, "play_events");
    if (event.id.empty()) event.id = key;
    event.count = state == 2 ? (std::max)(1, event.count) : 0;
    update.commit();

}
void Store::save_rip_score(const RipScore& score) {
    std::lock_guard lock(mutex_);
    require(score.value >= 0 && score.cap > 0 && score.level > 0, "Invalid RIP score, cap or level");
    Update update(*this);
    auto& row = update.json(value_.extensions, {"progress", "rip_score"});
    row["value"] = static_cast<std::uint64_t>(score.value);
    row["cap"] = static_cast<std::uint64_t>(score.cap);
    row["level"] = static_cast<std::uint32_t>(score.level);
    update.commit();

}
void Store::save_district_rank(std::string_view id, std::uint32_t rank) {
    std::lock_guard lock(mutex_);
    require(std::find(neighborhood_ids.begin(), neighborhood_ids.end(), id) != neighborhood_ids.end() && rank <= 10000, "Invalid district rank");
    Update update(*this);
    update.record(value_.bool_options, max_neighborhood_ranks_option, "player_settings") = false;
    update.record(value_.neighborhood_ranks, id, "neighborhood_ranks") = rank;
    update.commit();

}


bool Store::freecam_controller() const {
    std::lock_guard lock(mutex_);
    return profile::freecam_controller(value_);
}
void Store::save_freecam_controller(bool value) {
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"options", "freecam_controller"}) = value;
    update.commit();
}
std::uint32_t Store::freecam_controller_binding() const {
    std::lock_guard lock(mutex_);
    return profile::freecam_controller_binding(value_);
}
void Store::save_freecam_controller_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "freecam_controller"}) = combo;
    update.commit();
}

std::uint32_t Store::freecam_binding() const {
    std::lock_guard lock(mutex_);
    return profile::freecam_binding(value_);
}
void Store::save_freecam_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "freecam"}) = combo;
    update.commit();
}

std::uint32_t Store::tp_to_freecam_binding() const {
    std::lock_guard lock(mutex_);
    return profile::tp_to_freecam_binding(value_);
}
void Store::save_tp_to_freecam_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "tp_to_freecam"}) = combo;
    update.commit();
}

std::uint32_t Store::noclip_binding() const {
    std::lock_guard lock(mutex_);
    return profile::noclip_binding(value_);
}
void Store::save_noclip_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "noclip"}) = combo;
    update.commit();

}

std::uint32_t Store::forward_velocity_binding() const {
    std::lock_guard lock(mutex_);
    return profile::forward_velocity_binding(value_);
}
void Store::save_forward_velocity_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "forward_velocity"}) = combo;
    update.commit();
}

std::uint32_t Store::action_binding(std::string_view key) const {
    std::lock_guard lock(mutex_);
    return profile::action_binding(value_, key);
}
void Store::save_action_binding(std::string_view key, std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    const std::string name(key);
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", name.c_str()}) = combo;
    update.commit();
}
std::uint32_t Store::vote_binding(bool yes) const {
    std::lock_guard lock(mutex_);
    return yes ? profile::vote_yes_binding(value_) : profile::vote_no_binding(value_);
}
void Store::save_vote_binding(bool yes, std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", yes ? "vote_yes" : "vote_no"}) = combo;
    update.commit();
}
std::uint32_t Store::offboard_up_velocity_binding() const {
    std::lock_guard lock(mutex_);
    return profile::offboard_up_velocity_binding(value_);
}
void Store::save_offboard_up_velocity_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "offboard_up_velocity"}) = combo;
    update.commit();
}

std::uint32_t Store::up_velocity_binding() const {
    std::lock_guard lock(mutex_);
    return profile::up_velocity_binding(value_);
}
void Store::save_up_velocity_binding(std::uint32_t combo) {
    require(valid_action_binding(combo), "Unsupported controller combo or keyboard key");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"bindings", "up_velocity"}) = combo;
    update.commit();
}

void Store::save_graphics_controls(const GraphicsControls& c) {
    require(valid_graphics_controls(c), "Invalid graphics controls");
    std::lock_guard lock(mutex_);
    if (graphics_controls(value_) == c) return;
    Update update(*this);
    for (unsigned i = 0; i < graphics_keys.size(); ++i)
        update.json(value_.settings, {"graphics", graphics_keys[i]}) = c.effects[i] < 0 ? Json(nullptr) : Json(c.effects[i] != 0);
    update.commit();

}

void Store::save_world_controls(const WorldControls& c) {
    require(valid_world_controls(c), "Invalid world controls");
    std::lock_guard lock(mutex_);
    if (world_controls(value_) == c) return;
    Update update(*this);
    auto& out = update.json(value_.settings, {"world_controls"});
    const auto field = [&](const char* key, auto value) { out[key] = value == -1 ? Json(nullptr) : Json(value); };
    field("traffic", c.traffic); field("pedestrians", c.pedestrians); field("fog", c.fog);
    field("fog_distance", c.fog_distance); field("sky_brightness", c.sky_brightness);
    field("clouds", c.clouds); field("wind_strength", c.wind_strength); field("wind_direction", c.wind_direction);
    auto& detail=out["atmosphere"];
    if (!detail.is_object()) detail=Json::object();
    for (const auto& control : atmosphere_controls) {
        const auto choice=c.atmosphere.find(control.key);
        if (choice==c.atmosphere.end()) { detail.erase(control.key); continue; }
        const auto& v=choice->second;
        if (control.type==AtmosphereType::texture) detail[control.key]=v.texture;
        else if (control.type==AtmosphereType::toggle) detail[control.key]=v.number[0]!=0;
        else if (control.lanes==1) detail[control.key]=v.number[0];
        else {
            auto a=Json::array();
            for (unsigned i=0;i<control.lanes;++i) a.push_back(v.number[i]);
            detail[control.key]=std::move(a);
        }
    }
    update.commit();

}

void Store::save_world_layer_choice(unsigned layer, std::string_view mode) {
    require(layer < world_layers().size() && valid_world_layer_mode(mode), "Invalid world layer choice");
    std::lock_guard lock(mutex_);
    Update update(*this);
    update.json(value_.settings, {"world_layers", world_layers()[layer].key}) = mode;
    update.commit();

}

void Store::restore_world_layers(WorldMap map) {
    require(!world_map_key(map).empty(), "Invalid world map");
    std::lock_guard lock(mutex_);
    if (!value_.settings.contains("world_layers")) return;
    Update update(*this);
    auto& choices = update.json(value_.settings, {"world_layers"});
    for (const auto& layer : world_layers()) if (layer.map == map) choices.erase(std::string(layer.key));
    update.commit();

}

void Store::save_park_choice(unsigned lot, std::string_view id) {
    require(valid_park(lot, id), "Unknown park layout for this lot");
    std::lock_guard lock(mutex_);
    if (park_choices(value_)[lot] == id) return;
    Update update(*this);
    update.json(value_.settings, {"parks", park_lots[lot].key}) = id;
    update.commit();

}

void Store::save_park_choices(const ParkChoices& choices) {
    for (unsigned lot = 0; lot < choices.size(); ++lot)
        require(valid_park(lot, choices[lot]), "Unknown park layout for this lot");
    std::lock_guard lock(mutex_);
    if (park_choices(value_) == choices) return;
    Update update(*this);
    for (unsigned lot = 0; lot < choices.size(); ++lot)
        update.json(value_.settings, {"parks", park_lots[lot].key}) = choices[lot];
    update.commit();
}

}
