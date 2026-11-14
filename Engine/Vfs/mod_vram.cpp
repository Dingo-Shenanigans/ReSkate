#include "mod_vram.h"
#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/mesh_set.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <tuple>

namespace dingosdk::mods {

namespace {
using namespace detail;

// A MeshSet resource: a mesh and its levels of detail.
constexpr std::uint32_t mesh_set_type = 0x49b156d4;
constexpr std::string_view line_version = "v1";

// Every level of detail of a mesh, vertices and indices.
std::uint64_t mesh_bytes(std::span<const std::byte> resource) {
    std::size_t lods{};
    const auto first = fb::any_mesh_lod_geometry(resource, 0, &lods);
    std::uint64_t bytes = std::uint64_t{first.vertex_bytes} + first.index_bytes;
    for (std::size_t lod = 1; lod < lods; ++lod) {
        const auto geometry = fb::any_mesh_lod_geometry(resource, lod);
        bytes += std::uint64_t{geometry.vertex_bytes} + geometry.index_bytes;
    }
    return bytes;
}

// What one bundle, or one superbundle of a map's, adds up to.
struct Sum {
    std::uint64_t whole{}, resident{};
};

// What has been counted already: a level's bundles list a texture or a mesh many times over, and
// the game loads it once.
struct Seen {
    std::set<fb::Guid> textures;                                                   // by their pixels' chunk
    std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint32_t>> meshes;      // by where they lie in the mod
};

} // namespace

TextureBytes texture_bytes(const frostbite::TextureHeader& header) noexcept {
    TextureBytes bytes;
    const std::size_t mips = std::min<std::size_t>(header.mip_count, header.mip_sizes.size());
    for (std::size_t mip = 0; mip < mips; ++mip) {
        bytes.whole += header.mip_sizes[mip];
        if (mip >= header.first_mip) bytes.resident += header.mip_sizes[mip];
    }
    return bytes;
}

bool on_demand_bundle(std::string_view name) noexcept {
    // An item's look (its preset, mesh and textures) is in a bundle of its own kind's, which the
    // game loads through a bundle reference table when a skater puts the item on; the items
    // themselves and their thumbnails are in cas_main_sharedbundle, loaded from the start (read
    // from 30 real packs, 2026-11-14).
    return name.find("bundlereftable") != std::string_view::npos;
}

ModVram read_mod_vram(const std::filesystem::path& data_root, const std::filesystem::path& directory,
                      const std::atomic<bool>* stop) noexcept {
    ModVram result;
    try {
        const auto baseRoot = data_root / L"Data";
        const auto& gameRoot = data_root;
        const auto layout = vfs::read_layout(baseRoot / L"layout.toc");
        // (Nothing is written: the store only reads here.)
        CasStore store(baseRoot, fs::temp_directory_path() / L"reskate-mod-vram-unused", layout.root);
        store.keep_open();   // thousands of small reads out of a few archives
        Seen data;           // across the game's own superbundles the mod changes
        for (const auto& relative : scan(directory).tocs) {
            std::error_code error;
            const auto baseToc = baseRoot / fs::path(relative);
            // A superbundle the game does not have is a map's: its levels.
            const bool level = !fs::is_regular_file(baseToc, error);
            const auto own = fb::read_toc(read_file(directory / fs::path(relative)));
            fb::TocDocument game;
            std::map<std::string, const fb::TocBundle*, std::less<>> shipped;
            if (!level) {
                game = fb::read_toc(read_file(baseToc));
                for (const auto& bundle : game.bundles) shipped.emplace(ascii_lower(bundle.name), &bundle);
            }
            Seen own_seen;
            auto& seen = level ? own_seen : data;
            Sum map;
            for (const auto& bundle : own.bundles) {
                if (stop && stop->load()) throw std::runtime_error("the read was stopped");
                try {
                    const auto region = fb::read_bundle_region(bundle.region);
                    if (std::none_of(region.files.begin(), region.files.end(), [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                        continue;
                    const auto listing = list_bundle(store, directory, baseRoot, bundle, gameRoot);
                    if (!listing) {
                        ++result.unread;
                        continue;
                    }
                    const auto name = ascii_lower(bundle.name);
                    const auto& manifest = listing->manifest;
                    // The game's copy of the bundle, read only once the mod turns out to replace
                    // something of it: what the game's own version of that already costs.
                    std::optional<Listing> original;
                    std::map<std::string, std::size_t, std::less<>> originals;
                    bool looked{};
                    const auto replaced = [&](const fb::BundleAsset& asset) -> std::optional<std::uint64_t> {
                        if (level) return std::nullopt;
                        if (!looked) {
                            looked = true;
                            if (const auto found = shipped.find(name); found != shipped.end())
                                original = list_bundle(store, baseRoot, baseRoot, *found->second, gameRoot);
                            if (original)
                                for (std::size_t index = 0; index < original->manifest.resources.size(); ++index)
                                    originals.emplace(ascii_lower(original->manifest.resources[index].name), index);
                        }
                        const auto found = originals.find(ascii_lower(asset.name));
                        if (found == originals.end()) return std::nullopt;
                        const auto& theirs = original->manifest.resources[found->second];
                        if (theirs.resourceType != asset.resourceType) return std::nullopt;
                        try {
                            const auto bytes = read_asset(store, baseRoot, baseRoot, *original, original->manifest.ebx.size() + found->second, gameRoot);
                            if (asset.resourceType == mesh_set_type) return mesh_bytes(bytes);
                            return texture_bytes(fb::read_texture_header(bytes, theirs.resourceMeta)).whole;
                        } catch (const std::exception&) {
                            return std::nullopt;   // counted as added: an estimate on the high side
                        }
                    };
                    Sum sum;
                    const auto resourcesAt = listing->first + manifest.ebx.size();
                    for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
                        const auto& asset = manifest.resources[index];
                        const bool texture = asset.resourceType == fb::texture_resource_type;
                        if (!texture && asset.resourceType != mesh_set_type) continue;
                        if (resourcesAt + index >= listing->files.size()) break;
                        const auto& file = listing->files[resourcesAt + index];
                        if (!file.location.patch) continue;   // the game's own
                        try {
                            Sum cost;
                            if (texture) {
                                const auto bytes = read_asset(store, directory, baseRoot, *listing, manifest.ebx.size() + index, gameRoot);
                                const auto header = fb::read_texture_header(bytes, asset.resourceMeta);
                                if (!seen.textures.insert(header.chunk).second) continue;
                                const auto size = texture_bytes(header);
                                cost = {size.whole, size.resident};
                                ++result.textures;
                            } else {
                                if (!seen.meshes.emplace(file.location.installChunk, file.location.archive, file.offset).second) continue;
                                // (A mesh streams its levels of detail too; none is counted as staying.)
                                cost.whole = mesh_bytes(read_asset(store, directory, baseRoot, *listing, manifest.ebx.size() + index, gameRoot));
                                ++result.meshes;
                            }
                            if (const auto before = replaced(asset)) {
                                const auto less = std::min(cost.whole, *before);
                                cost.whole -= less;
                                cost.resident -= std::min(cost.resident, less);
                            }
                            sum.whole += cost.whole;
                            sum.resident += cost.resident;
                        } catch (const std::exception&) {
                            ++result.unread;
                        }
                    }
                    if (level) {
                        map.whole += sum.whole;
                        map.resident += sum.resident;
                    } else if (on_demand_bundle(name)) {
                        if (!sum.whole) continue;
                        result.on_demand += sum.whole;
                        result.on_demand_largest = std::max(result.on_demand_largest, sum.whole);
                        ++result.on_demand_bundles;
                    } else {
                        result.always += sum.whole;
                    }
                } catch (const std::exception&) {
                    ++result.unread;   // a bundle that cannot be read counts for nothing; the merge says why
                }
            }
            if (map.whole > result.level) {
                result.level = map.whole;
                result.level_resident = map.resident;
            }
        }
    } catch (const std::exception& failure) {
        result.problem = failure.what();
    } catch (...) {
        result.problem = "the mod could not be read";
    }
    return result;
}

std::string vram_line(const ModVram& vram) {
    std::string line(line_version);
    for (const std::uint64_t value : {vram.level, vram.level_resident, vram.always, vram.on_demand, vram.on_demand_largest,
                                      std::uint64_t{vram.on_demand_bundles}, std::uint64_t{vram.textures}, std::uint64_t{vram.meshes},
                                      std::uint64_t{vram.unread}})
        line += ' ' + std::to_string(value);
    return line;
}

bool parse_vram_line(std::string_view line, ModVram& vram) noexcept {
    if (!line.starts_with(line_version)) return false;
    line.remove_prefix(line_version.size());
    std::array<std::uint64_t, 9> values{};
    for (auto& value : values) {
        if (line.empty() || line.front() != ' ') return false;
        line.remove_prefix(1);
        const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), value);
        if (error != std::errc{} || end == line.data()) return false;
        line.remove_prefix(static_cast<std::size_t>(end - line.data()));
    }
    if (!line.empty()) return false;
    const auto small = [](std::uint64_t value) { return value <= 0xffffffffu; };
    if (!small(values[5]) || !small(values[6]) || !small(values[7]) || !small(values[8])) return false;
    ModVram read;
    read.level = values[0];
    read.level_resident = values[1];
    read.always = values[2];
    read.on_demand = values[3];
    read.on_demand_largest = values[4];
    read.on_demand_bundles = static_cast<std::uint32_t>(values[5]);
    read.textures = static_cast<std::uint32_t>(values[6]);
    read.meshes = static_cast<std::uint32_t>(values[7]);
    read.unread = static_cast<std::uint32_t>(values[8]);
    vram = read;
    return true;
}

} // namespace dingosdk::mods
