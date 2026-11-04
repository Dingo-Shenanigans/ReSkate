#include "synchros_outfit_mod.h"
#include "game_archives.h"
#include "native_db.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Resource/bundle_ref_table.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/toc.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace dingosdk::synchros {
namespace fs = std::filesystem;
namespace fb = frostbite;

// ---- SHA-1 (FIPS 180-4), for the manifest hash of a payload this writes itself ----

std::array<std::uint8_t, 20> sha1(std::span<const std::byte> bytes) {
    std::uint32_t h[5]{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    const auto rotl = [](std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    std::vector<std::uint8_t> data(bytes.size());
    if (!bytes.empty()) std::memcpy(data.data(), bytes.data(), bytes.size());
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    for (std::size_t block = 0; block < data.size(); block += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p = &data[block + static_cast<std::size_t>(i) * 4];
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999U; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1U; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6U; }
            const auto t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}
std::string leaf(const std::string& name) { return name.substr(name.rfind('/') + 1); }

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path_utf8(path));
    std::vector<std::byte> out(static_cast<std::size_t>(fs::file_size(path)));
    if (!in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())))
        throw std::runtime_error("cannot read " + path_utf8(path));
    return out;
}
void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path_utf8(path));
}
void write_text(const fs::path& path, const std::string& text) {
    write_file(path, std::as_bytes(std::span(text.data(), text.size())));
}

std::vector<std::byte> to_bytes(std::span<const std::uint8_t> in) {
    std::vector<std::byte> out(in.size());
    if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
    return out;
}
fb::Sha1 to_sha1(const std::array<std::uint8_t, 20>& in) {
    fb::Sha1 out;
    std::memcpy(out.bytes.data(), in.data(), 20);
    return out;
}
fb::Guid to_guid(const std::array<std::uint8_t, 16>& in) {
    fb::Guid out;
    std::memcpy(out.bytes.data(), in.data(), 16);
    return out;
}

// "win32/items" -> "Win32/items.toc"
fs::path toc_path(const std::string& superbundle) {
    const auto stem = superbundle.substr(superbundle.rfind('/') + 1);
    return fs::path(L"Win32") / fs::path(stem + ".toc");
}

// The game's TOCs, read once each.
struct GameTocs {
    fs::path data;
    std::map<std::string, std::optional<fb::TocDocument>> read;
    const fb::TocDocument* get(const std::string& superbundle) {
        auto [it, fresh] = read.try_emplace(superbundle);
        if (fresh) {
            try {
                it->second = fb::read_toc(read_file(data / toc_path(superbundle)));
            } catch (const std::exception&) {
                it->second.reset();
            }
        }
        return it->second ? &*it->second : nullptr;
    }
};

// One bundle as this mod writes it.
struct Entry {
    fb::BundleAsset asset;
    fb::BundleFileInfo file;
    std::vector<std::byte> record;   // a received chunk's record (a list of one), or empty
    bool received{};
};
struct BundleCopy {
    std::string name;                 // as the game names it
    std::uint32_t install_chunk{};
    std::vector<Entry> ebx, resources, chunks;
    std::vector<std::byte> chunk_metadata;   // the game's list
    std::size_t game_chunks{};
    bool known{};                     // the game ships it
};

// A chunk-metadata list's records, kept alive with the bytes they view.
struct Records {
    std::vector<unsigned char> bytes;
    native_db::Node root;
    bool ok{};
};
Records read_records(std::span<const std::byte> list) {
    Records r;
    if (list.empty()) return r;
    r.bytes.resize(list.size());
    std::memcpy(r.bytes.data(), list.data(), list.size());
    try {
        r.root = native_db::read(r.bytes, "chunk metadata", nullptr, {.unique_fields = false, .max_entries = r.bytes.size()});
        r.ok = true;
    } catch (const std::exception&) {
        r.ok = false;
    }
    return r;
}

} // namespace

OutfitMod write_outfit_mod(const fs::path& directory, std::uint64_t wearer, const fs::path& game,
                           const std::vector<OutfitItem>& items, const ReadReceived& read) {
    OutfitMod result;
    try {
        const auto data = game / "Data";
        const auto layout = vfs::read_layout(data / "layout.toc");
        vfs::GameArchives archives(data, layout.root);
        GameTocs tocs{data, {}};

        // Only items whose every file is in the cache.
        std::vector<const OutfitItem*> whole;
        std::map<std::array<std::uint8_t, 20>, std::vector<std::byte>> stored;
        for (const auto& item : items) {
            bool complete = true;
            for (const auto& file : item.files) {
                if (stored.contains(file.id)) continue;
                auto bytes = read(file.id);
                if (!bytes || bytes->size() != file.size) { complete = false; break; }
                stored.emplace(file.id, to_bytes(*bytes));
            }
            if (complete) whole.push_back(&item);
        }
        if (whole.empty()) {
            result.problem = "nothing received is complete yet";
            return result;
        }

        // The archives: one per install chunk, cas_01.cas in that chunk's package directory.
        std::map<std::uint32_t, std::vector<std::byte>> cas;
        const auto store = [&](std::uint32_t chunk, std::span<const std::byte> encoded) {
            auto& archive = cas[chunk];
            const fb::BundleFileInfo at{{true, chunk, 1}, static_cast<std::uint32_t>(archive.size()),
                                        static_cast<std::uint32_t>(encoded.size())};
            archive.insert(archive.end(), encoded.begin(), encoded.end());
            result.bytes += encoded.size();
            return at;
        };

        // Each bundle any received file or preset row needs, starting from the game's copy.
        std::map<std::pair<std::string, std::string>, BundleCopy> bundles;   // (superbundle, bundle lower) -> copy
        std::map<std::string, std::vector<fb::TocChunk>> toc_chunks;          // superbundle -> chunks
        const auto copy_of = [&](const std::string& superbundle_in, const std::string& bundle_in) -> BundleCopy& {
            const auto superbundle = lower(superbundle_in), bundle = lower(bundle_in);
            auto [it, fresh] = bundles.try_emplace({superbundle, bundle});
            auto& copy = it->second;
            if (!fresh) return copy;
            copy.name = bundle;
            const auto* toc = tocs.get(superbundle);
            if (!toc) throw std::runtime_error("the game has no " + superbundle + " superbundle");
            for (const auto& shipped : toc->bundles) {
                if (lower(shipped.name) != bundle) continue;
                auto region = fb::read_bundle_region(shipped.region);
                fb::BinaryBundle manifest;
                if (!region.inlineManifest.empty()) {
                    manifest = fb::read_binary_bundle(region.inlineManifest);
                } else {
                    if (region.files.empty()) break;
                    const auto first = region.files.front();
                    manifest = archives.read_manifest(data, first.location, first.offset, first.size, game);
                    region.files.erase(region.files.begin());
                }
                if (manifest.ebx.size() + manifest.resources.size() + manifest.chunks.size() != region.files.size())
                    throw std::runtime_error(shipped.name + ": the game's manifest does not match its files");
                std::size_t at = 0;
                for (const auto& asset : manifest.ebx) copy.ebx.push_back({asset, region.files[at++], {}, false});
                for (const auto& asset : manifest.resources) copy.resources.push_back({asset, region.files[at++], {}, false});
                for (const auto& asset : manifest.chunks) copy.chunks.push_back({asset, region.files[at++], {}, false});
                copy.chunk_metadata = manifest.chunkMetadata;
                copy.game_chunks = manifest.chunks.size();
                copy.name = shipped.name;
                copy.known = true;
                if (!region.files.empty()) copy.install_chunk = region.files.front().location.installChunk;
                break;
            }
            if (!copy.known) {
                // A bundle the game does not ship: its archive goes with the superbundle's others.
                for (const auto& shipped : toc->bundles) {
                    const auto region = fb::read_bundle_region(shipped.region);
                    if (region.files.empty()) continue;
                    copy.install_chunk = region.files.front().location.installChunk;
                    break;
                }
            }
            return copy;
        };
        const auto put = [](std::vector<Entry>& list, Entry entry, const auto& same) {
            const auto found = std::find_if(list.begin(), list.end(), same);
            if (found != list.end()) *found = std::move(entry);
            else list.push_back(std::move(entry));
        };

        std::set<std::string> keys;
        std::map<std::tuple<std::string, std::string, std::string>, std::vector<fb::bundle_ref::Preset>> rows;
        for (const auto* item : whole) {
            keys.insert(lower(leaf(item->item)));
            for (const auto& file : item->files) {
                const auto& bytes = stored.at(file.id);
                fb::BundleAsset asset;
                asset.name = file.name;
                asset.sha1 = to_sha1(file.sha1);
                asset.guid = to_guid(file.chunk_guid);
                asset.resourceId = file.resource_id;
                asset.resourceType = file.resource_type;
                asset.resourceMeta = to_bytes(file.resource_meta);
                asset.originalSize = file.original_size;
                asset.logicalOffset = file.logical_offset;
                asset.logicalSize = file.logical_size;
                if (file.kind == OutfitFile::toc_chunk) {
                    const auto* toc = tocs.get(lower(file.superbundle));
                    std::uint32_t chunk = 0;
                    if (toc)
                        for (const auto& shipped : toc->bundles) {
                            const auto region = fb::read_bundle_region(shipped.region);
                            if (region.files.empty()) continue;
                            chunk = region.files.front().location.installChunk;
                            break;
                        }
                    const auto at = store(chunk, bytes);
                    auto& list = toc_chunks[lower(file.superbundle)];
                    if (std::none_of(list.begin(), list.end(), [&](const fb::TocChunk& c) { return c.guid == asset.guid; }))
                        list.push_back({asset.guid, at.location, at.offset, at.size, false});
                    ++result.files;
                    continue;
                }
                auto& copy = copy_of(file.superbundle, file.bundle);
                Entry entry{asset, store(copy.install_chunk, bytes), {}, true};
                const auto name = lower(file.name);
                switch (file.kind) {
                case OutfitFile::ebx:
                    entry.asset.kind = fb::AssetKind::ebx;
                    put(copy.ebx, std::move(entry), [&](const Entry& e) { return lower(e.asset.name) == name; });
                    break;
                case OutfitFile::resource:
                    entry.asset.kind = fb::AssetKind::resource;
                    put(copy.resources, std::move(entry), [&](const Entry& e) { return lower(e.asset.name) == name; });
                    break;
                default:
                    entry.asset.kind = fb::AssetKind::chunk;
                    entry.record = to_bytes(file.chunk_record);
                    put(copy.chunks, std::move(entry), [&](const Entry& e) { return e.asset.guid == asset.guid; });
                    break;
                }
                ++result.files;
            }
            for (const auto& row : item->rows)
                rows[{lower(row.table_superbundle), lower(row.table_bundle), lower(row.table)}].push_back({row.path, row.bundle});
        }

        // Received presets go into the game's own table, in the bundle the game keeps it in.
        for (auto& [where, presets] : rows) {
            const auto& [superbundle, bundle, table] = where;
            auto& copy = copy_of(superbundle, bundle);
            const auto found = std::find_if(copy.resources.begin(), copy.resources.end(),
                                            [&](const Entry& e) { return lower(e.asset.name) == table; });
            if (found == copy.resources.end() || found->received)
                throw std::runtime_error(table + " is not in the game's " + bundle);
            const auto base = fb::decode_cas(archives.read(data, found->file.location, found->file.offset, found->file.size), {game});
            const auto merged = fb::bundle_ref::with_presets({base, found->asset.resourceMeta}, presets);
            found->asset.resourceMeta = merged.resourceMeta;
            found->asset.originalSize = merged.resource.size();
            found->asset.sha1 = to_sha1(sha1(merged.resource));
            found->file = store(copy.install_chunk, fb::encode_cas(merged.resource, {.compression = fb::CasCompression::raw}));
            found->received = true;
        }

        // The TOCs: each bundle with its manifest first, as the game and Studio write them.
        std::map<std::string, std::vector<fb::TocBundle>> written;
        for (auto& [where, copy] : bundles) {
            fb::BinaryBundle manifest;
            std::vector<fb::BundleFileInfo> files;
            for (const auto* list : {&copy.ebx, &copy.resources, &copy.chunks})
                for (const auto& entry : *list) {
                    (list == &copy.ebx ? manifest.ebx : list == &copy.resources ? manifest.resources : manifest.chunks).push_back(entry.asset);
                    files.push_back(entry.file);
                }
            // Chunk metadata as a mod's copy has it: the game's list, with a received chunk's record
            // at the place the chunk is listed (the merge takes it from there, merge_chunk_metadata).
            auto list = read_records(copy.chunk_metadata);
            std::vector<Records> mine;
            bool any_record = false;
            for (const auto& entry : copy.chunks) any_record = any_record || !entry.record.empty();
            if (any_record && (list.ok || copy.chunk_metadata.empty())) {
                const bool fresh = copy.chunk_metadata.empty();
                if (!fresh || copy.game_chunks == 0) {
                    std::vector<native_db::Node> children = fresh ? std::vector<native_db::Node>{} : list.root.children;
                    bool complete = true;
                    for (std::size_t index = 0; index < copy.chunks.size(); ++index) {
                        const auto& entry = copy.chunks[index];
                        if (!entry.received) continue;
                        auto& one = mine.emplace_back(read_records(entry.record));
                        if (!one.ok || one.root.children.size() != 1) {
                            if (index >= children.size()) complete = false;
                            continue;
                        }
                        if (index < children.size()) children[index] = one.root.children.front();
                        else if (index == children.size()) children.push_back(one.root.children.front());
                        else complete = false;
                    }
                    if (complete) {
                        auto root = fresh ? mine.front().root : list.root;
                        root.children = std::move(children);
                        const auto bytes = native_db::write(root);
                        manifest.chunkMetadata.resize(bytes.size());
                        std::memcpy(manifest.chunkMetadata.data(), bytes.data(), bytes.size());
                    } else {
                        manifest.chunkMetadata = copy.chunk_metadata;
                    }
                } else {
                    manifest.chunkMetadata = copy.chunk_metadata;
                }
            } else {
                manifest.chunkMetadata = copy.chunk_metadata;
            }
            const auto manifest_bytes = fb::write_binary_bundle(manifest);
            files.insert(files.begin(), store(copy.install_chunk, manifest_bytes));
            written[where.first].push_back({copy.name, fb::write_bundle_region(files), 1});
            ++result.bundles;
        }

        // Written beside, then put in place whole.
        auto building = directory;
        building += ".building";
        std::error_code ec;
        fs::remove_all(building, ec);
        fs::create_directories(building);
        std::set<std::string> superbundles;
        for (const auto& [superbundle, list] : written) superbundles.insert(superbundle);
        for (const auto& [superbundle, list] : toc_chunks) superbundles.insert(superbundle);
        for (const auto& superbundle : superbundles) {
            const auto found = written.find(superbundle);
            const auto chunks = toc_chunks.find(superbundle);
            const std::span<const fb::TocBundle> b = found == written.end() ? std::span<const fb::TocBundle>{} : found->second;
            const std::span<const fb::TocChunk> c = chunks == toc_chunks.end() ? std::span<const fb::TocChunk>{} : chunks->second;
            write_file(building / toc_path(superbundle), fb::write_patch_toc(b, c));
        }
        for (const auto& [chunk, bytes] : cas)
            write_file(building / L"Win32" / fs::path(archives.directory(chunk)) / L"cas_01.cas", bytes);
        fs::copy_file(data / "layout.toc", building / "layout.toc", fs::copy_options::overwrite_existing);
        // The game version stamp every mod that ships game data needs (mod_list.h, studio_marker_file).
        write_text(building / ".reskate-studio-patch",
                   "ReSkate Skate Synchros\nskate_sha256=" + std::string(supported_build::game_sha256) + "\n");
        std::string marker = "wearer=" + std::to_string(wearer) + "\n";
        for (const auto& key : keys) marker += "item=" + key + "\n";
        write_text(building / outfit_marker_file, marker);

        fs::remove_all(directory, ec);
        fs::create_directories(directory.parent_path(), ec);
        fs::rename(building, directory, ec);
        if (ec) {
            fs::remove_all(building, ec);
            result.problem = "could not put the outfit mod in place";
            return result;
        }
        result.items.assign(keys.begin(), keys.end());
        result.written = true;
    } catch (const std::exception& failure) {
        result.problem = failure.what();
        result.written = false;
    }
    return result;
}

std::vector<OutfitFolder> outfit_folders(const fs::path& root) {
    std::vector<OutfitFolder> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        std::ifstream in(it->path() / outfit_marker_file);
        if (!in) continue;
        OutfitFolder folder;
        try {
            folder.wearer = std::stoull(name);
        } catch (...) {
            continue;
        }
        folder.directory = it->path();
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("item=")) folder.items.push_back(lower(line.substr(5)));
        }
        out.push_back(std::move(folder));
    }
    std::sort(out.begin(), out.end(), [](const OutfitFolder& a, const OutfitFolder& b) { return a.wearer < b.wearer; });
    return out;
}

} // namespace dingosdk::synchros
