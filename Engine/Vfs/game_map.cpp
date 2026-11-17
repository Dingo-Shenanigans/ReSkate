#include "game_map.h"
#include "game_bundles.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/texture.h"
#include <algorithm>
#include <charconv>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dingosdk::vfs {
namespace fb = frostbite;
namespace ebx = frostbite::ebx;
namespace {
constexpr std::size_t most_tiles = 16; // held at once: the largest are 512 x 540, a megabyte each

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
const ebx::Object* object_of(const ebx::Value& value) {
    const auto* p = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
    return p && *p ? p->get() : nullptr;
}
std::optional<double> number(const ebx::Object& object, std::string_view name) {
    const auto* found = object.find(name);
    if (!found) return std::nullopt;
    if (const auto* v = std::get_if<std::int64_t>(&found->value.data)) return static_cast<double>(*v);
    if (const auto* v = std::get_if<std::uint64_t>(&found->value.data)) return static_cast<double>(*v);
    if (const auto* v = std::get_if<double>(&found->value.data)) return *v;
    return std::nullopt;
}
const ebx::Object* child(const ebx::Object& object, std::string_view name) {
    const auto* found = object.find(name);
    return found ? object_of(found->value) : nullptr;
}
std::string type_of(const ebx::Document& doc, const ebx::InstanceRecord& instance) {
    return instance.descriptor >= 0 && static_cast<std::size_t>(instance.descriptor) < doc.types.size()
               ? doc.types[static_cast<std::size_t>(instance.descriptor)].name
               : std::string{};
}
std::optional<ebx::Document> document(const GameData& data, const GameBundle& bundle, std::string_view name) {
    std::size_t index{};
    if (!bundle.find(fb::AssetKind::ebx, name, &index)) return std::nullopt;
    const auto* payload = bundle.payload(fb::AssetKind::ebx, index);
    if (!payload) return std::nullopt;
    return ebx::read_document(data.read(*payload));
}

struct Camera {
    float x{}, y{}, z{}, fov{};
};
// The one camera of the level's "dev_mapcameras": where it is, and its angle.
std::optional<Camera> read_camera(const GameData& data, const fb::TocDocument& toc, const std::string& folder) {
    const auto bundle = data.read_bundle(toc, "win32/" + folder + "/dev_mapscreenshots");
    if (!bundle) return std::nullopt;
    const auto doc = document(data, *bundle, folder + "/dev_mapcameras");
    if (!doc) return std::nullopt;
    for (const auto& instance : doc->instances) {
        if (!instance.object || type_of(*doc, instance) != "CameraEntityData") continue;
        const auto* transform = child(*instance.object, "Transform");
        const auto* trans = transform ? child(*transform, "trans") : nullptr;
        const auto fov = number(*instance.object, "Fov");
        if (!trans || !fov) continue;
        const auto x = number(*trans, "x"), y = number(*trans, "y"), z = number(*trans, "z");
        if (!x || !y || !z) continue;
        return Camera{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z), static_cast<float>(*fov)};
    }
    return std::nullopt;
}

constexpr std::string_view record_prefix = "ui/features/map/data/coremap_", record_suffix = "_resource";
constexpr std::string_view everyones_record = "ui/features/map/data/coremap_bam_resource"; // San Vansterdam's, in every level's bundle
// A map made for a level a mod adds (ReSkate Studio's pause map, modding/maps/ui/pause_map.cpp): its
// record is a copy of one of the game's under this name, in San Vansterdam's "coregameassets" bundle,
// beside San Vansterdam's own.
constexpr std::string_view mods_record_prefix = "ui/reskate/coremap/";
constexpr std::string_view map_suffix = "_map";

// The level's own map record in this bundle, or nothing: a mod's where there is one, else the game's
// for this level (not San Vansterdam's copy, where there is another).
const fb::BundleAsset* map_record(const GameBundle& bundle) {
    const fb::BundleAsset* found = nullptr;
    for (const auto& asset : bundle.manifest.ebx) {
        if (asset.name.starts_with(mods_record_prefix)) return &asset;
        if (!asset.name.starts_with(record_prefix) || !asset.name.ends_with(record_suffix)) continue;
        if (!found || found->name == everyones_record) found = &asset;
    }
    return found;
}

// Where the map's camera is, from the record itself: its view model's CameraPos, the record's only
// boxed value that is a place high over the level (x, height, z, and nothing).
std::optional<Camera> record_camera(const ebx::Document& record) {
    for (const auto& boxed : record.boxedValues) {
        if (boxed.rawBytes.size() != 16) continue;
        std::array<float, 4> value{};
        std::memcpy(value.data(), boxed.rawBytes.data(), sizeof(value));
        if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2]) || value[1] < 100 || value[1] > 1.0e6f || value[3] != 0) continue;
        return Camera{value[0], value[1], value[2], 5}; // (the map's camera always has 5 degrees)
    }
    return std::nullopt;
}

// A number at the start of `text`, taken off it.
std::optional<std::uint32_t> take_number(std::string_view& text) {
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end == text.data()) return std::nullopt;
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    return value;
}
}

std::optional<GameMap> read_game_map(const std::filesystem::path& gameRoot, std::string_view level, const std::filesystem::path& patchRoot) {
    const auto asset = lower(level);
    const auto slash = asset.rfind('/');
    if (asset.empty() || slash == std::string::npos) return std::nullopt;
    const auto folder = asset.substr(0, slash);
    const GameData data(gameRoot, patchRoot);
    GameMap map;
    map.toc = "Win32/" + asset + ".toc";
    fb::TocDocument toc;
    try {
        toc = data.read_toc(map.toc);
    } catch (const std::exception&) {
        return std::nullopt; // not one of the game's levels
    }
    // The level's map record: in its own bundle, or its "coregameassets" one.
    std::optional<GameBundle> holder;
    const fb::BundleAsset* record = nullptr;
    std::vector<std::string> wanted{"win32/" + asset};
    for (const auto& bundle : toc.bundles)
        if (bundle.name.ends_with("coregameassets")) wanted.push_back(bundle.name);
    for (const auto& name : wanted) {
        auto bundle = data.read_bundle(toc, name);
        if (!bundle) continue;
        if (const auto* found = map_record(*bundle)) {
            map.bundle = name;
            holder = std::move(bundle);
            record = map_record(*holder);
            break;
        }
    }
    if (!holder || !record) return std::nullopt;
    const auto record_doc = document(data, *holder, record->name);
    if (!record_doc) return std::nullopt;

    // The camera the map was made from: where the record says it is (what the game's own map screen
    // goes by, and all a map made for a mod's level has). Should a record ever have none, the camera
    // entity the game's levels keep: in the level's own folder, or, for a level a mod built on one of
    // the game's, in any "dev_mapscreenshots" bundle of this superbundle, and then in the game's own
    // superbundle for the folder the map's record is in.
    constexpr std::string_view shots = "/dev_mapscreenshots", platform = "win32/";
    auto camera = record_camera(*record_doc);
    if (!camera) camera = read_camera(data, toc, folder);
    for (std::size_t i = 0; i < toc.bundles.size() && !camera; ++i) {
        const auto& name = toc.bundles[i].name;
        if (!name.ends_with(shots) || !name.starts_with(platform)) continue;
        camera = read_camera(data, toc, name.substr(platform.size(), name.size() - platform.size() - shots.size()));
    }
    if (!camera && map.bundle.starts_with(platform)) {
        const auto cut = map.bundle.rfind('/');
        const auto from = map.bundle.substr(platform.size(), cut - platform.size());
        const auto leaf = from.rfind('/');
        if (cut != std::string::npos && cut > platform.size() && leaf != std::string::npos) {
            try {
                camera = read_camera(data, data.read_toc("Win32/" + from + from.substr(leaf) + ".toc"), from);
            } catch (const std::exception&) {
            }
        }
    }
    if (!camera || !(camera->y > 1) || !(camera->fov > 0.1f && camera->fov < 120)) return std::nullopt;

    // The map asset it imports: the one of the bundle's maps whose own guid is among the imports.
    std::optional<ebx::Document> asset_doc;
    for (std::size_t i = 0; i < holder->manifest.ebx.size() && !asset_doc; ++i) {
        const auto& candidate = holder->manifest.ebx[i];
        if (!candidate.name.ends_with(map_suffix)) continue;
        const auto* payload = holder->payload(fb::AssetKind::ebx, i);
        if (!payload) continue;
        const auto bytes = data.read(*payload);
        const auto guid = ebx::read_file_guid(bytes);
        if (std::none_of(record_doc->imports.begin(), record_doc->imports.end(), [&](const auto& import) { return import.fileGuid == guid; }))
            continue;
        asset_doc = ebx::read_document(bytes);
        map.name = candidate.name;
    }
    const auto* root = asset_doc ? asset_doc->root() : nullptr;
    if (!root || !root->object || asset_doc->rootType != "DingoMapAsset") return std::nullopt;
    const auto* size = child(*root->object, "SourceTextureSize");
    const auto source_x = size ? number(*size, "x") : std::nullopt, source_y = size ? number(*size, "y") : std::nullopt;
    const auto* lods_field = root->object->find("Lods");
    const auto* lods = lods_field ? std::get_if<ebx::Value::Array>(&lods_field->value.data) : nullptr;
    if (!source_x || !source_y || !(*source_x >= 1) || !(*source_y >= 1) || !lods || lods->empty()) return std::nullopt;
    map.source_width = static_cast<float>(*source_x);
    map.source_height = static_cast<float>(*source_y);
    for (const auto& value : *lods) {
        const auto* lod = object_of(value);
        const auto scale = lod ? number(*lod, "ScaleFactor") : std::nullopt;
        const auto columns = lod ? number(*lod, "TileCountX") : std::nullopt, rows = lod ? number(*lod, "TileCountY") : std::nullopt;
        if (!scale || !columns || !rows || !(*scale > 0) || *columns < 1 || *rows < 1 || *columns > 64 || *rows > 64) return std::nullopt;
        GameMapLod one;
        one.scale = static_cast<float>(*scale);
        one.columns = static_cast<std::uint32_t>(*columns);
        one.rows = static_cast<std::uint32_t>(*rows);
        one.tiles.resize(std::size_t{one.columns} * one.rows);
        map.lods.push_back(std::move(one));
    }
    // The tiles' textures, by their names: "<leaf>_<lod>_<index>_<w>,<h>_<w>_<h>".
    // (The game's are named by the asset's leaf; a map made for a mod's level by its whole name.)
    const auto whole = map.name + "_", stem = map.name.substr(map.name.rfind('/') + 1) + "_";
    for (const auto& resource : holder->manifest.resources) {
        if (resource.resourceType != fb::texture_resource_type) continue;
        std::string_view rest = resource.name;
        if (rest.starts_with(whole)) rest.remove_prefix(whole.size());
        else if (rest.starts_with(stem)) rest.remove_prefix(stem.size());
        else continue;
        const auto lod = take_number(rest);
        if (!lod || rest.empty() || rest.front() != '_') continue;
        rest.remove_prefix(1);
        const auto index = take_number(rest);
        if (!index || *lod >= map.lods.size() || *index >= map.lods[*lod].tiles.size()) continue;
        if (!rest.empty() && rest.front() != '_') continue; // (a longer number: another tile's)
        map.lods[*lod].tiles[*index] = resource.name;
    }
    // What the picture covers: the camera looks straight down from its height through its angle.
    map.centre_x = camera->x;
    map.centre_z = camera->z;
    map.height = 2 * camera->y * std::tan(camera->fov * 0.5f * 3.14159265f / 180);
    map.width = map.height * 16 / 9; // (the camera's view is 16:9, whatever the picture's own shape)
    return map;
}

GameMapReader::GameMapReader(std::filesystem::path gameRoot, GameMap map, const std::filesystem::path& patchRoot)
    : textures_(std::move(gameRoot), patchRoot), map_(std::move(map)) {}

const fb::Image& GameMapReader::tile(std::size_t lod, std::uint32_t index) {
    const auto key = std::make_pair(lod, index);
    if (const auto found = tiles_.find(key); found != tiles_.end()) return found->second;
    fb::Image image; // (left empty when it cannot be read: that part of the map is clear)
    const auto& name = map_.lods[lod].tiles[index];
    if (!name.empty()) {
        try {
            image = textures_.read(map_.toc, map_.bundle, name, 4096);
        } catch (const std::exception&) {
        }
    }
    if (order_.size() >= most_tiles) {
        tiles_.erase(order_.front());
        order_.pop_front();
    }
    order_.push_back(key);
    return tiles_.emplace(key, std::move(image)).first->second;
}

fb::Image GameMapReader::window(float x, float z, float metres, std::uint32_t side) {
    fb::Image out;
    out.width = out.height = side;
    out.rgba.assign(std::size_t{side} * side * 4, 0);
    if (!side || !(metres > 0) || map_.lods.empty() || !(map_.width > 0) || !(map_.height > 0)) return out;
    // The smallest tiles that still have a pixel for each of the window's.
    const float wanted = static_cast<float>(side) / metres * 0.9f;
    std::size_t lod = 0;
    for (std::size_t i = map_.lods.size(); i-- > 0;)
        if (map_.source_width * map_.lods[i].scale / map_.width >= wanted) {
            lod = i;
            break;
        }
    const auto& level = map_.lods[lod];
    const float left = map_.centre_x - map_.width / 2, top = map_.centre_z - map_.height / 2;
    // Each column's and row's tile and place in it are the same down a column and along a row.
    struct Place {
        std::uint32_t tile{};
        float part{};
        bool on{};
    };
    std::vector<Place> across(side), down(side);
    for (std::uint32_t i = 0; i < side; ++i) {
        const float step = (static_cast<float>(i) + 0.5f) / static_cast<float>(side) * metres - metres / 2;
        const float u = (x + step - left) / map_.width * static_cast<float>(level.columns);
        const float v = (z + step - top) / map_.height * static_cast<float>(level.rows);
        if (u >= 0 && u < static_cast<float>(level.columns)) across[i] = {static_cast<std::uint32_t>(u), u - std::floor(u), true};
        if (v >= 0 && v < static_cast<float>(level.rows)) down[i] = {static_cast<std::uint32_t>(v), v - std::floor(v), true};
    }
    for (std::uint32_t row = 0; row < side; ++row) {
        if (!down[row].on) continue;
        const fb::Image* image = nullptr;
        std::uint32_t held = ~0u;
        for (std::uint32_t column = 0; column < side; ++column) {
            if (!across[column].on) continue;
            const auto index = down[row].tile * level.columns + across[column].tile;
            if (index != held) {
                held = index;
                image = &tile(lod, index);
            }
            if (!image->width || !image->height) continue;
            const auto px = std::min(image->width - 1, static_cast<std::uint32_t>(across[column].part * static_cast<float>(image->width)));
            const auto py = std::min(image->height - 1, static_cast<std::uint32_t>(down[row].part * static_cast<float>(image->height)));
            const auto* from = image->rgba.data() + (std::size_t{py} * image->width + px) * 4;
            auto* to = out.rgba.data() + (std::size_t{row} * side + column) * 4;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = 255;
        }
    }
    return out;
}
}
