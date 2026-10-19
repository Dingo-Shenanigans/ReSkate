#include "park_object_sets.h"
#include "park_document.h"
#include <algorithm>
#include <stdexcept>

namespace dingosdk::editor {
profile::ObjectLayout anchored_set(profile::ObjectLayout objects) {
    if (objects.empty())
        return objects;
    std::array<float, 3> low = objects.front().position, high = low;
    for (const auto &object : objects)
        for (unsigned i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], object.position[i]);
            high[i] = std::max(high[i], object.position[i]);
        }
    const std::array<float, 3> anchor{(low[0] + high[0]) * .5f, low[1], (low[2] + high[2]) * .5f};
    std::uint64_t id = 1;
    for (auto &object : objects) {
        object.id = id++; // Ids are the set's own; placing gives every object a new one.
        for (unsigned i = 0; i < 3; ++i)
            object.position[i] -= anchor[i];
    }
    return objects;
}
void save_object_set(const std::filesystem::path &directory, std::string_view name, std::string_view map,
                     const profile::ObjectLayout &objects) {
    if (!valid_park_name(name))
        throw std::runtime_error("Use 1-64 letters, numbers, spaces, underscores or hyphens for the set name.");
    if (objects.empty())
        throw std::runtime_error("Select objects to save as a set.");
    if (objects.size() > 1024)
        throw std::runtime_error("A set holds at most 1024 objects.");
    const auto existing = list_parks(directory);
    if (existing.size() >= max_object_sets && std::find(existing.begin(), existing.end(), name) == existing.end())
        throw std::runtime_error("You have " + std::to_string(max_object_sets) + " sets. Delete one first.");
    const auto anchored = anchored_set(objects);
    for (const auto &object : anchored)
        if (!profile::valid_placed_object(object))
            throw std::runtime_error("The selection has an object that cannot be saved.");
    save_park(directory, name, {map.empty() ? std::string("unknown") : std::string(map), anchored});
}
void remove_object_set(const std::filesystem::path &directory, std::string_view name) {
    if (!valid_park_name(name))
        throw std::runtime_error("That set does not exist.");
    std::error_code error;
    if (!std::filesystem::remove(directory / (std::string(name) + ".park.json"), error) || error)
        throw std::runtime_error("Could not delete the set " + std::string(name) + ".");
}
std::vector<EditorObjectSet> load_object_sets(const std::filesystem::path &directory) {
    std::vector<EditorObjectSet> sets;
    if (directory.empty())
        return sets;
    for (const auto &name : list_parks(directory)) {
        try {
            const auto park = load_park(directory, name);
            EditorObjectSet set{name, park.map, {}};
            for (const auto &object : park.objects)
                set.objects.push_back({object.id, object.item, object.position, object.rotation, true, object.scale});
            if (!set.objects.empty())
                sets.push_back(std::move(set));
        } catch (...) { /* A damaged or foreign file is not listed. */ }
    }
    return sets;
}
} // namespace dingosdk::editor
