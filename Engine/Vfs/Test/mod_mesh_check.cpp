// The meshes a mod ships, checked as the GPU would read them (frostbite::check_mesh_lod): a
// section whose vertices or indices run past their buffer, or a triangle naming a vertex outside
// its section, sends a draw, or a ray traced against the mesh, to memory that is not the mesh's.
// For finding what in a map hangs or faults the GPU when it loads.
//
// Arguments: <Skate folder> <mod folder> [<bundle filter>]. Every MeshSet the mod's own files
// hold is read, each level of detail checked; what is wrong is listed by bundle and mesh, then
// how many meshes each bundle has and how many are wrong.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/mod_merge_internal.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <cstring>
#include <string>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk::mods::detail;

namespace {
constexpr std::uint32_t mesh_set_type = 0x49b156d4;

struct Tally {
    std::size_t meshes{}, wrong{}, unreadable{};
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "Usage: dingosdk_mod_mesh_check <Skate folder> <mod folder> [<bundle filter>]\n";
        return 2;
    }
    const fs::path game_root = argv[1], mod = argv[2];
    std::string filter;
    if (argc > 3) for (const wchar_t* c = argv[3]; *c; ++c) filter.push_back(static_cast<char>(*c));
    const auto base_root = game_root / L"Data";
    try {
        const auto layout = dingosdk::vfs::read_layout(base_root / L"layout.toc");
        // (Nothing is written: the store is only read through.)
        const auto scratch = fs::temp_directory_path() / L"reskate-mesh-check";
        CasStore store(base_root, scratch, layout.root);
        std::map<std::string, Tally> tallies;
        std::map<std::uint32_t, std::size_t> formats;
        for (const auto& relative : scan(mod).tocs) {
            fb::TocDocument own;
            try {
                own = fb::read_toc(read_file(mod / fs::path(relative)));
            } catch (const std::exception& error) {
                std::cout << "unreadable toc " << fs::path(relative).string() << ": " << error.what() << "\n";
                continue;
            }
            for (const auto& bundle : own.bundles) {
                if (!filter.empty() && bundle.name.find(filter) == std::string::npos) continue;
                std::optional<Listing> listing;
                try {
                    listing = list_bundle(store, mod, base_root, bundle, game_root);
                } catch (const std::exception& error) {
                    std::cout << "unreadable bundle " << bundle.name << ": " << error.what() << "\n";
                    continue;
                }
                if (!listing) continue;
                const auto& manifest = listing->manifest;
                auto& tally = tallies[bundle.name];
                for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
                    const auto file = manifest.ebx.size() + index;
                    const auto at = listing->first + file;
                    if (manifest.resources[index].resourceType != mesh_set_type || at >= listing->files.size() ||
                        !listing->files[at].location.patch)
                        continue;  // (the game's own meshes are not the mod's to check)
                    const auto& name = manifest.resources[index].name;
                    ++tally.meshes;
                    std::vector<std::byte> resource;
                    std::size_t lods{};
                    try {
                        resource = read_asset(store, mod, base_root, *listing, file, game_root);
                        (void)fb::any_mesh_lod_geometry(resource, 0, &lods);
                    } catch (const std::exception& error) {
                        ++tally.unreadable;
                        // (Each layout this reader does not know, once: its first words and size.)
                        static std::map<std::string, bool> shown;
                        std::string head;
                        for (std::size_t word = 0; word < 16 && (word + 1) * 4 <= resource.size(); ++word) {
                            std::uint32_t value{};
                            std::memcpy(&value, resource.data() + word * 4, 4);
                            head += std::to_string(value) + " ";
                        }
                        if (!shown[head.substr(0, 48)]) {
                            shown[head.substr(0, 48)] = true;
                            std::cout << "layout " << head << "(" << resource.size() << " bytes) e.g. " << name << ": " << error.what() << "\n";
                            // (RESKATE_MESH_DUMP=1: the first one whole, eight bytes a line, for working out its layout.)
                            static bool dumped{};
                            if (!dumped && GetEnvironmentVariableW(L"RESKATE_MESH_DUMP", nullptr, 0)) {
                                dumped = true;
                                for (std::size_t offset = 0; offset + 8 <= resource.size(); offset += 8) {
                                    std::uint32_t low{}, high{};
                                    std::memcpy(&low, resource.data() + offset, 4);
                                    std::memcpy(&high, resource.data() + offset + 4, 4);
                                    std::cout << "  +" << at << ": " << low << " " << high << "\n";
                                }
                            }
                        }
                        continue;
                    }
                    // (RESKATE_MESH_DUMP_NAME=<part of a name>: that mesh's resource, offset and 32-bit word a line.)
                    if (wchar_t wanted_name[256]{}; GetEnvironmentVariableW(L"RESKATE_MESH_DUMP_NAME", wanted_name, 256)) {
                        std::string part;
                        for (const wchar_t* c = wanted_name; *c; ++c) part.push_back(static_cast<char>(*c));
                        if (name.find(part) != std::string::npos)
                            for (std::size_t offset = 0; offset + 4 <= resource.size(); offset += 4) {
                                std::uint32_t value{};
                                std::memcpy(&value, resource.data() + offset, 4);
                                std::cout << "dump " << offset << " " << value << "\n";
                            }
                    }
                    bool wrong{};
                    for (std::size_t lod = 0; lod < lods; ++lod) {
                        try {
                            const auto wanted = fb::any_mesh_lod_geometry(resource, lod).chunk;
                            std::vector<std::byte> geometry;
                            for (std::size_t chunk = 0; chunk < manifest.chunks.size() && geometry.empty(); ++chunk)
                                if (manifest.chunks[chunk].guid == wanted)
                                    geometry = read_asset(store, mod, base_root, *listing,
                                                          manifest.ebx.size() + manifest.resources.size() + chunk, game_root);
                            for (auto chunk = own.chunks.begin(); chunk != own.chunks.end() && geometry.empty(); ++chunk)
                                if (chunk->guid == wanted && !chunk->removed)
                                    geometry = fb::decode_cas(store.read(chunk->location.patch ? mod : base_root, chunk->location,
                                                                         chunk->offset, chunk->size), {game_root});
                            if (geometry.empty()) {
                                std::cout << bundle.name << " | " << name << " | lod " << lod << " | its geometry chunk is in neither the bundle nor its superbundle\n";
                                wrong = true;
                                continue;
                            }
                            std::uint32_t format{};
                            const auto problems = fb::check_mesh_lod(resource, lod, geometry, &format);
                            // (RESKATE_MESH_STATS=1: each level's size, for comparing a map's meshes with another's:
                            // sections, buffer bytes, and the most vertices and triangles a section has.)
                            if (GetEnvironmentVariableW(L"RESKATE_MESH_STATS", nullptr, 0)) {
                                const auto word = [&](std::size_t offset) {
                                    std::uint64_t value{};
                                    std::memcpy(&value, resource.data() + offset, 8);
                                    return value;
                                };
                                const auto lod_at = static_cast<std::size_t>(word(48 + lod * 8)) + 16;
                                const auto sections = static_cast<std::uint32_t>(word(lod_at + 8));
                                const auto table = static_cast<std::size_t>(word(lod_at + 12)) + 16;
                                const auto sizes = fb::any_mesh_lod_geometry(resource, lod);
                                // (Static meshes' sections are 384 or 416 bytes; the larger section counts read by either.)
                                std::uint64_t most_vertices{}, most_triangles{};
                                for (std::uint32_t section = 0; section < sections; ++section)
                                    for (const std::size_t stride : {384u, 416u}) {
                                        const auto at_section = table + section * stride;
                                        if (at_section + 48 > resource.size()) continue;
                                        std::uint32_t triangles{}, vertices{};
                                        std::memcpy(&triangles, resource.data() + at_section + 32, 4);
                                        std::memcpy(&vertices, resource.data() + at_section + 44, 4);
                                        if (vertices <= sizes.vertex_bytes && triangles * 6ull <= sizes.index_bytes + 6ull) {
                                            most_vertices = std::max<std::uint64_t>(most_vertices, vertices);
                                            most_triangles = std::max<std::uint64_t>(most_triangles, triangles);
                                        }
                                    }
                                std::cout << "stats " << bundle.name << " " << name << " " << lod << " " << sections << " " << sizes.vertex_bytes
                                          << " " << sizes.index_bytes << " " << most_vertices << " " << most_triangles << "\n";
                            }
                            ++formats[format];
                            for (const auto& problem : problems) std::cout << bundle.name << " | " << name << " | lod " << lod << " | " << problem << "\n";
                            wrong = wrong || !problems.empty();
                        } catch (const std::exception& error) {
                            std::cout << bundle.name << " | " << name << " | lod " << lod << " | unreadable: " << error.what() << "\n";
                        }
                    }
                    tally.wrong += wrong;
                }
            }
        }
        std::cout << "\nBundle | meshes | wrong | unreadable\n";
        for (const auto& [name, tally] : tallies)
            if (tally.meshes) std::cout << name << " | " << tally.meshes << " | " << tally.wrong << " | " << tally.unreadable << "\n";
        std::cout << "Index formats seen:";
        for (const auto& [format, count] : formats) std::cout << " " << format << " (" << count << " levels)";
        std::cout << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
