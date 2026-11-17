#include "mod_merge_internal.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/mesh_set.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

// Reading a mod's own bundles for what its files alone decide (ScanCache::Bundle): every EBX
// decoded and its type references checked, a map's meshes sized, its textures looked at. It is
// most of what a merge reads, and none of it depends on any other mod, so it is done before the
// superbundles are merged, a bundle at a time on every thread there is. The merge of a
// superbundle then only reads again what this found.
namespace dingosdk::mods::detail {
namespace {
// A MeshSet resource: a mesh and its levels of detail.
constexpr std::uint32_t mesh_set_type = 0x49b156d4;
// A map's own level bundles (Studio names them after the level).
constexpr std::string_view map_level_bundles = "win32/levels/game/dingolevel_reskate_";

// Every level of detail of a mesh, vertices and indices.
std::uint64_t mesh_bytes(const CasStore& store, const fs::path& root, const fb::BundleFileInfo& file, const fs::path& gameRoot) {
    const auto payload = fb::decode_cas(store.read(root, file.location, file.offset, file.size), {gameRoot});
    std::size_t lods{};
    const auto first = fb::any_mesh_lod_geometry(payload, 0, &lods);
    std::uint64_t bytes = std::uint64_t{first.vertex_bytes} + first.index_bytes;
    for (std::size_t lod = 1; lod < lods; ++lod) {
        const auto geometry = fb::any_mesh_lod_geometry(payload, lod);
        bytes += std::uint64_t{geometry.vertex_bytes} + geometry.index_bytes;
    }
    return bytes;
}
} // namespace

// The sizes of one mod's meshes by where each lies in its archives: a level's bundles list a
// mesh many times over, and it is decoded once.
class MeshSizes {
public:
    std::uint64_t of(const CasStore& store, const fs::path& root, const fb::BundleFileInfo& file, const fs::path& gameRoot) {
        const std::tuple key{file.location.installChunk, file.location.archive, file.offset};
        {
            std::lock_guard lock(mutex_);
            if (const auto found = sizes_.find(key); found != sizes_.end()) return found->second;
        }
        const auto bytes = mesh_bytes(store, root, file, gameRoot);   // (throws: nothing is remembered)
        std::lock_guard lock(mutex_);
        return sizes_.try_emplace(key, bytes).first->second;
    }

private:
    std::mutex mutex_;
    std::map<std::tuple<std::uint32_t, std::uint16_t, std::uint32_t>, std::uint64_t> sizes_;
};

BundleScan scan_bundle(const CasStore& store, const fs::path& root, std::string_view key, fb::BinaryBundle& manifest,
                       std::vector<fb::BundleFileInfo>& files, std::size_t listed, const std::set<fb::Guid>* tocChunks,
                       const fs::path& gameRoot, MeshSizes* sizes) {
    BundleScan scan;
    auto& found = scan.found;
    for (std::size_t index = 0; index < manifest.ebx.size() && listed + index < files.size(); ++index) {
        const auto at = listed + index;
        const auto& file = files[at];
        if (!file.location.patch) continue;   // the game's own copy
        try {
            auto encoded = store.read(root, file.location, file.offset, file.size);
            fb::BundleAsset checked;
            checked.originalSize = manifest.ebx[index].originalSize;
            if (repair_ebx_payload(encoded, checked, gameRoot)) found.repair.push_back(static_cast<std::uint32_t>(at));
        } catch (const std::exception&) {
            // Not kept, and read again by the merge, which says why it could not be.
            scan.whole = false;
            found.repair.push_back(static_cast<std::uint32_t>(at));
        }
    }
    if (!key.starts_with(map_level_bundles)) return scan;
    // A map's level bundle: its own meshes, every level of detail, for the mesh streaming pool
    // the runtime gives the map (reskate-map-meshes.json).
    const auto resourcesAt = listed + manifest.ebx.size();
    for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
        if (manifest.resources[index].resourceType != mesh_set_type || resourcesAt + index >= files.size()) continue;
        const auto& file = files[resourcesAt + index];
        if (!file.location.patch) continue;
        try {
            const auto bytes = sizes ? sizes->of(store, root, file, gameRoot) : mesh_bytes(store, root, file, gameRoot);
            found.meshes.push_back({file.location.installChunk, file.location.archive, file.offset, bytes});
        } catch (const std::exception&) {
            scan.whole = false;
        }
    }
    // Its textures that never stream (mod_merge_textures.cpp).
    if (tocChunks) {
        const auto streamed = stream_resident_textures(store, root, manifest, files, listed, *tocChunks, gameRoot);
        for (const auto& [at, encoded] : streamed.headers) found.textures.push_back(static_cast<std::uint32_t>(at));
        for (const auto at : streamed.offsets) found.textures.push_back(static_cast<std::uint32_t>(at));
    }
    return scan;
}

void scan_mods(const std::vector<std::pair<const Mod*, std::string>>& tocs, const CasStore& store, const fs::path& baseRoot,
               const fs::path& gameRoot, ScanCache& scans, std::size_t threads, bool background) noexcept {
    // One of a mod's TOCs, read by the thread that takes it. Its bundles are then taken one at a
    // time by that thread and by every thread that has no TOC left to take, so a map's level,
    // which is most of its mod, is not left to one thread while the others have nothing to do.
    struct Loaded {
        fb::TocDocument toc;
        std::set<fb::Guid> chunks;   // what a texture streams its mips from
        MeshSizes sizes;
        std::atomic<std::size_t> next{0};
    };
    struct Task {
        const Mod* mod{};
        std::string relative, name;
        std::uintmax_t bytes{};
        std::mutex mutex;
        std::shared_ptr<Loaded> loaded;   // while its bundles are being read
    };
    try {
        std::vector<std::unique_ptr<Task>> tasks;
        for (const auto& [mod, relative] : tocs) {
            auto name = path_utf8(mod->directory.filename());
            if (scans.covers(name, ascii_lower(relative))) continue;
            std::error_code error;
            const auto bytes = fs::file_size(mod->directory / fs::path(relative), error);
            tasks.push_back(std::make_unique<Task>());
            tasks.back()->mod = mod;
            tasks.back()->relative = relative;
            tasks.back()->name = std::move(name);
            tasks.back()->bytes = error ? 0 : bytes;
        }
        if (tasks.empty()) return;
        // The largest first: the others are then free to help with it sooner.
        std::ranges::stable_sort(tasks, [](const auto& left, const auto& right) { return left->bytes > right->bytes; });

        const auto drain = [&](const Task& task, Loaded& loaded) {
            const auto prefix = ascii_lower(task.relative) + '|';
            for (;;) {
                const auto index = loaded.next.fetch_add(1);
                if (index >= loaded.toc.bundles.size()) return;
                const auto& bundle = loaded.toc.bundles[index];
                try {
                    auto listing = list_bundle(store, task.mod->directory, baseRoot, bundle, gameRoot);
                    if (!listing) continue;
                    const auto key = ascii_lower(bundle.name);
                    auto scan = scan_bundle(store, task.mod->directory, key, listing->manifest, listing->files, listing->first,
                                            &loaded.chunks, gameRoot, &loaded.sizes);
                    if (scan.whole) scans.keep(task.name, prefix + key, std::move(scan.found));
                } catch (const std::exception&) {
                    // Left for the merge, which reads the bundle itself and says what is wrong with it.
                }
            }
        };
        std::atomic<std::size_t> next{0};
        const auto work = [&]() noexcept {
            try {
                for (;;) {
                    const auto mine = next.fetch_add(1);
                    if (mine >= tasks.size()) break;
                    auto& task = *tasks[mine];
                    auto loaded = std::make_shared<Loaded>();
                    try {
                        loaded->toc = fb::read_toc(read_file(task.mod->directory / fs::path(task.relative)));
                    } catch (const std::exception&) {
                        continue;   // the merge reads it too, and that is where it is the mod's problem
                    }
                    for (const auto& chunk : loaded->toc.chunks)
                        if (!chunk.removed) loaded->chunks.insert(chunk.guid);
                    {
                        std::lock_guard lock(task.mutex);
                        task.loaded = loaded;
                    }
                    drain(task, *loaded);
                    {
                        std::lock_guard lock(task.mutex);
                        task.loaded.reset();   // (a thread still helping keeps its own hold on it)
                    }
                    scans.cover(task.name, ascii_lower(task.relative));
                }
                // No TOC left to take: help with the ones still being read.
                for (const auto& task : tasks) {
                    std::shared_ptr<Loaded> loaded;
                    {
                        std::lock_guard lock(task->mutex);
                        loaded = task->loaded;
                    }
                    if (loaded) drain(*task, *loaded);
                }
            } catch (...) {}   // out of memory: what was not read here is read by the merge
        };
        run_on_threads(threads, background, work);
    } catch (...) {}
}

} // namespace dingosdk::mods::detail
