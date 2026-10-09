#pragma once
#include <array>
#include <string>
#include <string_view>
#include <stdexcept>

namespace dingosdk {
struct ParkLot {
    std::string_view key, label;
    // Flump, mega, skate, street: authored variants in this client.
    std::array<unsigned, 4> counts;
};
inline constexpr std::array<ParkLot, 3> park_lots{{
    {"construction", "Construction site / Hedgemont", {8, 4, 4, 9}},
    {"historic", "Piers 1 / Historic", {10, 5, 7, 6}},
    {"financial", "Piers 2 / Financial", {8, 4, 7, 6}},
}};
inline constexpr std::array<std::string_view, 4> park_families{"flumppark", "megapark", "skatepark", "streetpark"};
inline constexpr std::array<std::string_view, 4> park_family_labels{"Flump Park", "Mega Park", "Skate Park", "Street Park"};
using ParkChoices = std::array<std::string, park_lots.size()>;
inline std::string park_id(unsigned family, unsigned variant) {
    return std::string(park_families.at(family)) + (variant < 10 ? "_0" : "_") + std::to_string(variant);
}
inline bool valid_park(unsigned lot, std::string_view id) {
    if (lot >= park_lots.size()) return false;
    if (id.empty() || id == "empty") return true;
    for (unsigned family = 0; family < park_families.size(); ++family)
        for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant)
            if (id == park_id(family, variant)) return true;
    return false;
}
// The service event selects by family Contains + "parkN" EndsWith. The
// two-digit asset suffix belongs to the separate developer-console path.
inline std::string park_native_name(std::string_view id) {
    if (id.empty() || id == "empty") return {};
    for (unsigned family = 0; family < park_families.size(); ++family)
        for (unsigned variant = 1; variant <= 10; ++variant)
            if (id == park_id(family, variant))
                return std::string(park_families[family]) + std::to_string(variant);
    throw std::invalid_argument("Unknown park layout");
}
inline std::string park_label(std::string_view id) {
    if (id.empty()) return "Choose a layout";
    if (id == "empty") return "Empty lot";
    for (unsigned i = 0; i < park_families.size(); ++i)
        if (id.starts_with(std::string(park_families[i]) + "_"))
            return std::string(park_family_labels[i]) + " " + std::string(id.substr(park_families[i].size() + 1));
    return "Unknown layout";
}
struct ParksModel {
    bool available{}, ready{}, controlled_by_host{};
    bool randomize_on_launch{};
    ParkChoices choices;
    std::string feedback;
};
}
