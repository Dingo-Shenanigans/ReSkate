#include "world_layer_scan.h"
#include "content_cache.h"
#include "game_bundles.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Core/Text/ascii.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace dingosdk::world_layer_scan {
namespace fs = std::filesystem;
namespace fb = frostbite;
namespace {
struct MapSource { WorldMap map; const char* toc; };
// The playable maps and the superbundle each one's level data lives in.
constexpr std::array<MapSource, 6> maps{{
    {WorldMap::bam, "Win32/levels/game/bam_levelroot/bam_levelroot.toc"},
    {WorldMap::grom, "Win32/levels/game/dingolevel_isle_of_grom/dingolevel_isle_of_grom.toc"},
    {WorldMap::ftue, "Win32/levels/game/dingolevel_ftue_island/dingolevel_ftue_island.toc"},
    {WorldMap::stadium_1, "Win32/levels/game/dingolevel_sdm/dingolevel_sdm_int_001/dingolevel_sdm_int_001.toc"},
    {WorldMap::stadium_2, "Win32/levels/game/dingolevel_sdm/dingolevel_sdm_int_002/dingolevel_sdm_int_002.toc"},
    {WorldMap::mpr, "Win32/levels/game/dingolevel_mpr/dingolevel_mpr.toc"},
}};

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }
std::string leaf_of(std::string_view bundle) {
    const auto slash = bundle.find_last_of('/');
    return std::string(slash == std::string_view::npos ? bundle : bundle.substr(slash + 1));
}
std::string slug(std::string_view text) {
    std::string result;
    for (const char c : ascii_lower(text)) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (word) result += c;
        else if (!result.empty() && result.back() != '_') result += '_';
    }
    while (!result.empty() && result.back() == '_') result.pop_back();
    return result;
}

struct Reference { std::string bundle, carrier; bool autoload{}; };

// The references each bundle's own (primary) EBX partition carries.
std::vector<Reference> read_references(const fs::path& gameRoot, const char* tocPath, std::set<std::string>& shipped) {
    vfs::GameData data(gameRoot);
    const auto toc = data.read_toc(tocPath);
    std::vector<Reference> result;
    for (const auto& entry : toc.bundles) {
        const auto name = ascii_lower(entry.name.starts_with("win32/") ? std::string_view(entry.name).substr(6) : entry.name);
        shipped.insert(name);
        const auto bundle = data.read_bundle(toc, entry.name);
        if (!bundle) continue;
        std::size_t index{};
        if (!bundle->find(fb::AssetKind::ebx, name, &index)) continue;
        const auto* payload = bundle->payload(fb::AssetKind::ebx, index);
        if (!payload) continue;
        const auto document = fb::ebx::read_document(data.read(*payload));
        for (const auto& instance : document.instances) {
            if (!instance.object) continue;
            const auto* target = instance.object->find("BundleName");
            const auto* autoload = instance.object->find("AutoLoad");
            const auto* text = target ? std::get_if<std::string>(&target->value.data) : nullptr;
            const auto* flag = autoload ? std::get_if<bool>(&autoload->value.data) : nullptr;
            if (text && flag && !text->empty()) result.push_back({*text, name, *flag});
        }
    }
    return result;
}

// Layers there is no sense in switching off. They stay in the node tree, so
// their children are still found and still switchable, but they get no row of
// their own: CoreGameAssets holds what every map is built out of, and a player
// who turned it off lost the world with no clue which row had done it. A
// choice already stored for one is keyed by row, so it simply stops applying.
bool essential_layer(std::string_view bundle) {
    return contains(leaf_of(ascii_lower(bundle)), "coregameassets");
}

std::string category(std::string_view bundle) {
    const auto path = ascii_lower(bundle);
    const auto leaf = leaf_of(path);
    if (contains(leaf, "activit")) return "Activities";
    if (contains(leaf, "season")) return "Seasonal";
    if (contains(path, "/_debug_/") || leaf.starts_with("dev_") || leaf.starts_with("debug") || leaf.starts_with("qv_") ||
        leaf.starts_with("mktg") || leaf.starts_with("techart") || contains(leaf, "test") || contains(leaf, "marketing"))
        return "Debug";
    if (contains(leaf, "lighting") || leaf.starts_with("tod_") || contains(leaf, "_tod_") || leaf.ends_with("_lrv"))
        return "Lighting";
    if (contains(leaf, "communitypark")) return "Parks";
    if (contains(leaf, "event")) return "Seasonal";
    if (leaf.starts_with("wp") || leaf.starts_with("worldroot") || contains(leaf, "terrain") || contains(leaf, "vista") ||
        contains(leaf, "coregameassets"))
        return "Structure";
    return "World";
}

void add_map(WorldLayerCatalog& catalog, WorldMap map, const std::vector<Reference>& references,
    const std::set<std::string>& shipped) {
    // Keep the references the player can switch: shipped bundles the game does
    // not drive itself. The first reference to a bundle wins.
    std::map<std::string, Reference> kept;
    std::vector<std::string> order;
    for (const auto& reference : references) {
        const auto bundle = ascii_lower(reference.bundle);
        if (!shipped.contains(bundle) || contains(bundle, "activitypresentation") ||
            contains(reference.carrier, "activitypresentation") || contains(bundle, "commlot") ||
            contains(reference.carrier, "commlot"))
            continue;
        if (kept.emplace(bundle, reference).second) order.push_back(bundle);
    }
    // A reference's parent is the layer whose bundle carries it; anything
    // carried by the level root (or by a bundle that is not a layer) is a root.
    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots;
    for (const auto& bundle : order) {
        const auto& carrier = kept.at(bundle).carrier;
        if (carrier != bundle && kept.contains(carrier)) children[carrier].push_back(bundle);
        else roots.push_back(bundle);
    }
    const auto by_name = [&](const std::string& a, const std::string& b) { return a < b; };
    std::sort(roots.begin(), roots.end(), by_name);
    std::set<std::string> placed;
    std::function<void(const std::string&, int)> emit = [&](const std::string& bundle, int parent) {
        if (!placed.insert(bundle).second) return;
        const auto slot = static_cast<int>(catalog.nodes.size());
        const auto& reference = kept.at(bundle);
        catalog.nodes.push_back({reference.bundle, map, parent, reference.autoload});
        auto list = children[bundle];
        std::sort(list.begin(), list.end(), by_name);
        for (const auto& child : list) emit(child, slot);
    };
    const auto first = catalog.nodes.size();
    for (const auto& root : roots) emit(root, -1);
    if (catalog.nodes.size() == first) return;
    catalog.anchors.push_back(static_cast<unsigned>(first));

    // One row per layer. Keys are "<map>_<bundle leaf>", the whole bundle path
    // when two leaves collide.
    std::map<std::string, unsigned> leaves;
    for (auto slot = first; slot < catalog.nodes.size(); ++slot) ++leaves[slug(leaf_of(catalog.nodes[slot].bundle))];
    const std::string prefix(world_map_key(map));
    for (auto slot = first; slot < catalog.nodes.size(); ++slot) {
        const auto& node = catalog.nodes[slot];
        if (essential_layer(node.bundle)) continue;
        const auto leaf = leaf_of(node.bundle);
        const auto key = prefix + "_" + (leaves[slug(leaf)] > 1 ? slug(node.bundle) : slug(leaf));
        auto label = leaf;
        std::replace(label.begin(), label.end(), '_', ' ');
        catalog.layers.push_back({key, label, node.bundle, map, static_cast<unsigned>(slot),
            static_cast<unsigned>(slot), category(node.bundle)});
    }
}

std::string stamp(const fs::path& gameRoot) {
    std::ostringstream out;
    out << "2";   // 2: no row for the layers essential_layer keeps out
    for (const auto& source : maps) {
        const auto path = gameRoot / "Data" / fs::path(source.toc);
        std::error_code error;
        const auto size = fs::file_size(path, error);
        const auto time = fs::last_write_time(path, error).time_since_epoch().count();
        out << ';' << source.toc << ':' << (error ? 0 : size) << ':' << (error ? 0 : time);
    }
    return out.str();
}

WorldMap map_of(std::string_view key) {
    for (const auto map : {WorldMap::bam, WorldMap::grom, WorldMap::ftue, WorldMap::stadium_1, WorldMap::stadium_2, WorldMap::mpr})
        if (world_map_key(map) == key) return map;
    throw std::runtime_error("Unknown world map in world-layer cache");
}

WorldLayerCatalog from_json(const Json& document) {
    WorldLayerCatalog catalog;
    for (const auto& node : document.at("nodes"))
        catalog.nodes.push_back({node.at("bundle").string(), map_of(node.at("map").string()),
            node.at("parent").get<int>(), node.at("autoload").get<bool>()});
    for (const auto& anchor : document.at("anchors")) catalog.anchors.push_back(anchor.get<unsigned>());
    for (const auto& row : document.at("rows"))
        catalog.layers.push_back({row.at("key").string(), row.at("label").string(), row.at("detail").string(),
            map_of(row.at("map").string()), row.at("leaf").get<unsigned>(), row.at("switch").get<unsigned>(),
            row.at("category").string()});
    // The same invariants the runtime relies on: parents first, one map per path.
    for (std::size_t i = 0; i < catalog.nodes.size(); ++i) {
        const auto parent = catalog.nodes[i].parent;
        if (parent < -1 || parent >= static_cast<int>(i) ||
            (parent >= 0 && catalog.nodes[static_cast<std::size_t>(parent)].map != catalog.nodes[i].map))
            throw std::runtime_error("Invalid world-layer cache");
    }
    for (const auto anchor : catalog.anchors)
        if (anchor >= catalog.nodes.size() || catalog.nodes[anchor].parent != -1) throw std::runtime_error("Invalid world-layer cache");
    for (const auto& layer : catalog.layers)
        if (layer.leaf >= catalog.nodes.size() || layer.switch_slot >= catalog.nodes.size() ||
            catalog.nodes[layer.leaf].map != layer.map || catalog.nodes[layer.switch_slot].map != layer.map)
            throw std::runtime_error("Invalid world-layer cache");
    return catalog;
}
}

WorldLayerCatalog scan(const fs::path& gameRoot) {
    struct Result { std::vector<Reference> references; std::set<std::string> shipped; };
    std::vector<std::future<Result>> pending;
    std::vector<std::jthread> workers;
    pending.reserve(maps.size());
    workers.reserve(maps.size());
    // MSVC std::async schedules onto Windows' existing thread pool. During
    // pre-entrypoint initialization the launcher gates those existing threads,
    // so waiting for a pool job deadlocks on a cold/stale layer cache. Dedicated
    // threads are created after the gate and can finish the scan independently.
    // jthread also joins all started jobs if creation or a scan throws.
    for (const auto& source : maps) {
        std::packaged_task<Result()> task([&gameRoot, toc = source.toc] {
            Result result;
            result.references = read_references(gameRoot, toc, result.shipped);
            return result;
        });
        pending.push_back(task.get_future());
        workers.emplace_back(std::move(task));
    }
    WorldLayerCatalog catalog;
    for (std::size_t i = 0; i < maps.size(); ++i) {
        const auto result = pending[i].get();
        add_map(catalog, maps[i].map, result.references, result.shipped);
    }
    return catalog;
}

std::string to_json(const WorldLayerCatalog& catalog, const std::string& stamp) {
    Json document{{"stamp", stamp}, {"nodes", Json::array()}, {"anchors", Json::array()}, {"rows", Json::array()}};
    for (const auto& node : catalog.nodes)
        document["nodes"].push_back(Json{{"bundle", node.bundle}, {"map", std::string(world_map_key(node.map))},
            {"parent", node.parent}, {"autoload", node.autoload}});
    for (const auto anchor : catalog.anchors) document["anchors"].push_back(anchor);
    for (const auto& layer : catalog.layers)
        document["rows"].push_back(Json{{"key", layer.key}, {"label", layer.label}, {"detail", layer.detail},
            {"map", std::string(world_map_key(layer.map))}, {"leaf", layer.leaf}, {"switch", layer.switch_slot},
            {"category", layer.category}});
    return document.dump(1);
}

WorldLayerCatalog load_or_scan(const fs::path& gameRoot, const fs::path& file) {
    const auto current = stamp(gameRoot);
    try {
        std::ifstream input(file, std::ios::binary);
        if (input) {
            const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            const auto document = Json::parse(text.begin(), text.end());
            if (document.value("stamp", "") == current) return from_json(document);
        }
    } catch (...) {}
    auto catalog = scan(gameRoot);
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    const auto temporary = fs::path(file).concat(".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << to_json(catalog, current);
        if (!output) return catalog;
    }
    fs::rename(temporary, file, error);
    return catalog;
}

WorldLayerCatalog read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path_utf8(file));
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return from_json(Json::parse(text.begin(), text.end()));
}

fs::path cache_file() { return content_cache::directory() / "world-layers.json"; }
}
