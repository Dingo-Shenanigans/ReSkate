#include "Engine/Vfs/content_catalogs.h"
#include "local_customization.h"
#include <limits>
#include <set>
#include <stdexcept>

namespace dingosdk::profile {
namespace {
using Json = dingosdk::Json;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool text(const std::string& value, bool empty = false) {
    if ((!empty && value.empty()) || value.size() > 255) return false;
    for (const unsigned char c : value) if (c < 32 || c == 127) return false;
    return true;
}
std::uint32_t word(const Json& value) {
    require(value.is_number_integer() && !value.is_boolean(), "Recipe word must be an integer");
    if (value.is_number_unsigned()) {
        const auto n = value.get<std::uint64_t>();
        require(n <= (std::numeric_limits<std::uint32_t>::max)(), "Recipe word exceeds uint32");
        return static_cast<std::uint32_t>(n);
    }
    const auto n = value.get<std::int64_t>();
    require(n >= 0 && n <= (std::numeric_limits<std::uint32_t>::max)(), "Recipe word exceeds uint32");
    return static_cast<std::uint32_t>(n);
}
std::vector<std::uint32_t> words(const Json& value, std::size_t limit) {
    require(value.is_array() && value.size() <= limit, "Invalid recipe parameter array");
    std::vector<std::uint32_t> result;
    for (const auto& item : value) result.push_back(word(item));
    return result;
}
}
CosmeticLoadout decode_loadout(const Json& value) {
    require(value.is_object() && word(value.at("recipe_format")) == 1, "Unsupported local recipe format");
    const auto& recipes = value.at("recipes");
    require(recipes.is_array() && !recipes.empty() && recipes.size() <= 16, "Invalid recipe count");
    CosmeticLoadout result;
    std::set<std::uint32_t> templates;
    for (const auto& entry : recipes) {
        require(entry.is_object(), "Recipe must be an object");
        CosmeticRecipe recipe;
        recipe.template_key = word(entry.at("template_key"));
        recipe.template_version = word(entry.at("template_version"));
        require(recipe.template_key != 0 && templates.insert(recipe.template_key).second,
            "Invalid or duplicate recipe template");
        recipe.scalar_bits = words(entry.at("scalar_bits"), 512);
        const auto& items = entry.at("items");
        require(items.is_array() && items.size() <= 256, "Invalid cosmetic slot count");
        std::set<std::uint32_t> slots;
        for (const auto& item : items) {
            require(item.is_object() && item.at("asset").is_string(), "Invalid cosmetic slot");
            CosmeticSlot slot;
            slot.slot = word(item.at("slot"));
            slot.asset = item.at("asset").get<std::string>();
            require(text(slot.asset, true) && slots.insert(slot.slot).second, "Invalid or duplicate cosmetic slot");
            slot.parameter_bits = words(item.at("parameter_bits"), 128);
            recipe.items.push_back(std::move(slot));
        }
        result.recipes.push_back(std::move(recipe));
    }
    return result;
}
Json encode_loadout(const CosmeticLoadout& value) {
    Json result{{"recipe_format", 1}, {"recipes", Json::array()}};
    for (const auto& recipe : value.recipes) {
        Json items = Json::array();
        for (const auto& slot : recipe.items)
            items.push_back({{"slot", slot.slot}, {"asset", slot.asset}, {"parameter_bits", slot.parameter_bits}});
        result["recipes"].push_back({{"template_key", recipe.template_key},
            {"template_version", recipe.template_version}, {"scalar_bits", recipe.scalar_bits}, {"items", items}});
    }
    (void)decode_loadout(result);
    return result;
}
void validate_customization(const Json& value) {
    require(value.is_object(), "Customization must be an object");
    if (value.contains("catalog")) {
        const auto& catalog = value.at("catalog");
        require(catalog.is_object() && catalog.size() <= saved_item_limit, "Invalid cosmetic metadata catalog");
        for (const auto& [key, metadata] : catalog.items()) {
            require(text(key) && metadata.is_object(), "Invalid cosmetic metadata record");
            for (const auto* field : {"title", "description", "rarity_id"})
                if (metadata.contains(field))
                    require(metadata.at(field).is_string() && text(metadata.at(field).get<std::string>(), true),
                        "Cosmetic metadata must be text of at most 255 bytes without control characters");
        }
    }
    if (value.contains("player_card")) validate_player_card(decode_loadout(value.at("player_card")));
    if (value.contains("selected_preset_index"))
        require(word(value.at("selected_preset_index")) < 10, "Invalid selected cosmetic preset");
    if (value.contains("loadouts")) {
        const auto& loadouts = value.at("loadouts");
        require(loadouts.is_object() && loadouts.size() <= 10, "Invalid local preset collection");
        for (const auto& [id, recipe] : loadouts.items()) {
            require(text(id), "Invalid local preset identifier");
            (void)decode_loadout(recipe);
        }
    }
    if (value.contains("inventory")) {
        const auto& inventory = value.at("inventory");
        require(inventory.is_object() && inventory.size() <= saved_item_limit, "Invalid cosmetic inventory");
        for (const auto& [key, owned] : inventory.items())
            require(text(key) && owned.is_boolean(), "Invalid cosmetic ownership record");
    }
}
void validate_player_card(const CosmeticLoadout& value) {
    require(value.recipes.size() == 1, "RIP Card requires one recipe");
    const auto& recipe = value.recipes.front();
    require(recipe.template_key == 2169419386U && recipe.template_version == 1 &&
        recipe.scalar_bits.empty() && recipe.items.size() == 3, "Unsupported RIP Card recipe");
    std::set<std::uint32_t> slots{782673846U, 42677725U, 2466297683U};
    for (const auto& item : recipe.items)
        require(slots.erase(item.slot) == 1 && text(item.asset) && item.parameter_bits.empty(),
            "Invalid RIP Card slot");
}
dingosdk::Json item_display_metadata(const std::string& asset, const dingosdk::Json& overrides) {
    // Titles, descriptions and rarities come from the installed content cache.
    const auto& items = content_cache::catalogs().items;
    std::string key = asset;
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    Json result = items.value(key, Json::object());
    // User metadata takes precedence field by field, including explicit empty
    // descriptions. Do not copy unrelated saved inventory data into the catalog.
    for (const auto* field : {"title", "description", "rarity_id"})
        if (overrides.contains(field)) result[field] = overrides.at(field);
    return result;
}
}
