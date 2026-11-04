// outfit-probe: for every mod in a Mods folder, what showing one worn item would cost.
//
// For each cosmetic item a mod ships (an EBX asset with a Key and ItemData), it follows the item
// to everything it is made of that the mod itself provides: the EBX assets it imports, their
// resources, and the chunks those name. Files the game ships are left out, since every player
// has them. The sum of what is left is what outfit sharing (contrib/outfit-sharing.md) would send
// for that item. Reads only; writes a report.
//
//   outfit-probe.exe [game folder] [report file]
//
// The game folder (with Skate.exe, Data and Mods) defaults to the one above this program.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/game_archives.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace ebx = dingosdk::frostbite::ebx;

namespace {
std::ofstream report;
void say(const std::string& line) {
    report << line << '\n';
    report.flush();
    std::fputs((line + "\n").c_str(), stdout);
}
std::string mb(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f MB", static_cast<double>(bytes) / 1e6);
    return text;
}
std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::byte> out(raw.size());
    if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}
std::string guid_key(const fb::Guid& guid) { return std::string(reinterpret_cast<const char*>(guid.bytes.data()), 16); }

// One file of the mod's own (its payload is in the mod's archives).
struct Node {
    enum Kind { ebx_asset, resource, chunk } kind{};
    std::string name;
    std::uint64_t resource_id{};
    std::string guid;            // ebx: file guid; chunk: chunk guid
    fb::BundleFileInfo file{};
    bool toc_chunk{};
    std::vector<std::size_t> edges;
    bool item{};
    std::string root_type;
};

struct Mod {
    std::vector<Node> nodes;
    std::unordered_map<std::string, std::size_t> by_guid;      // ebx file guid / chunk guid
    std::unordered_map<std::uint64_t, std::size_t> by_resource;
    std::unordered_map<std::string, std::vector<std::size_t>> res_by_name;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> chunk_by_prefix; // a chunk guid's first 8 bytes
    std::uint64_t own_bytes{};
    std::size_t errors{};
};

std::uint64_t placement_key(const fb::BundleFileInfo& f) {
    return (static_cast<std::uint64_t>(f.location.archive) << 48) ^ (static_cast<std::uint64_t>(f.location.installChunk) << 32) ^ f.offset;
}

void walk(const ebx::Value& value, const ebx::Document& doc, std::vector<std::uint64_t>& resources, std::vector<std::string>& guids);
void walk(const ebx::Object& object, const ebx::Document& doc, std::vector<std::uint64_t>& resources, std::vector<std::string>& guids) {
    for (const auto& field : object.fields) walk(field.value, doc, resources, guids);
}
void walk(const ebx::Value& value, const ebx::Document& doc, std::vector<std::uint64_t>& resources, std::vector<std::string>& guids) {
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fb::ebx::ResourceReference>) resources.push_back(v.id);
        else if constexpr (std::is_same_v<T, fb::Guid>) guids.push_back(guid_key(v));
        else if constexpr (std::is_same_v<T, fb::ebx::PointerReference>) {
            if (v.kind == fb::ebx::PointerKind::external && v.index >= 0 && static_cast<std::size_t>(v.index) < doc.imports.size())
                guids.push_back(guid_key(doc.imports[static_cast<std::size_t>(v.index)].fileGuid));
        } else if constexpr (std::is_same_v<T, std::shared_ptr<fb::ebx::Object>>) {
            if (v) walk(*v, doc, resources, guids);
        } else if constexpr (std::is_same_v<T, fb::ebx::Value::Array>) {
            for (const auto& item : v) walk(item, doc, resources, guids);
        }
    }, value.data);
}

// Every 16-byte run that is one of the mod's chunk guids, every 8-byte run that is one of its
// resource ids: how resources (textures, meshes) name the chunks holding their data.
void scan(std::span<const std::byte> bytes, const Mod& mod, std::vector<std::size_t>& out) {
    for (std::size_t at = 0; at + 8 <= bytes.size(); ++at) {
        std::uint64_t id;
        std::memcpy(&id, bytes.data() + at, 8);
        if (!id) continue;
        if (const auto found = mod.by_resource.find(id); found != mod.by_resource.end()) out.push_back(found->second);
        if (at + 16 > bytes.size()) continue;
        if (const auto found = mod.chunk_by_prefix.find(id); found != mod.chunk_by_prefix.end())
            for (const auto index : found->second)
                if (std::memcmp(mod.nodes[index].guid.data(), bytes.data() + at, 16) == 0) out.push_back(index);
    }
}

void probe(const fs::path& mod_dir, const fs::path& game, dingosdk::vfs::GameArchives& archives) {
    const auto started = std::chrono::steady_clock::now();
    Mod mod;
    std::set<std::uint64_t> counted;
    std::vector<fs::path> tocs;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(mod_dir / "Win32", error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
        if (it->is_regular_file(error)) {
            // Cosmetics are in the items and globals superbundles; levels hold maps.
            const auto name = lower(it->path().filename().string());
            if (name == "items.toc" || name == "globals.toc") tocs.push_back(it->path());
        }
    if (tocs.empty()) return;  // a plugin or a map: nothing to wear
    say("-- reading " + mod_dir.filename().string());

    const fs::path data = game / "Data";
    for (const auto& toc_path : tocs) {
        fb::TocDocument toc;
        try { toc = fb::read_toc(read_file(toc_path)); } catch (const std::exception& e) {
            say("  ! " + toc_path.filename().string() + ": " + e.what()); ++mod.errors; continue;
        }
        for (const auto& chunk : toc.chunks) {
            if (!chunk.location.patch || chunk.removed) continue;
            Node n; n.kind = Node::chunk; n.guid = guid_key(chunk.guid); n.toc_chunk = true;
            n.file = {chunk.location, chunk.offset, chunk.size};
            mod.by_guid.emplace(n.guid, mod.nodes.size());
            if (counted.insert(placement_key(n.file)).second) mod.own_bytes += n.file.size;
            mod.nodes.push_back(std::move(n));
        }
        for (const auto& bundle : toc.bundles) {
            try {
                auto region = fb::read_bundle_region(bundle.region);
                if (std::none_of(region.files.begin(), region.files.end(), [](const auto& f) { return f.location.patch; })) continue;
                fb::BinaryBundle manifest;
                std::size_t first = 0;
                if (!region.inlineManifest.empty()) manifest = fb::read_binary_bundle(region.inlineManifest);
                else {
                    bool found = false;
                    const auto expected = region.files.size() - 1;
                    for (std::size_t c = 0; c < region.files.size() && !found; ++c) {
                        const auto& f = region.files[c];
                        if (f.size > (32u << 20)) continue;
                        try {
                            auto m = archives.read_manifest(f.location.patch ? mod_dir : data, f.location, f.offset, f.size, game);
                            if (m.ebx.size() + m.resources.size() + m.chunks.size() != expected) continue;
                            manifest = std::move(m);
                            std::rotate(region.files.begin(), region.files.begin() + static_cast<std::ptrdiff_t>(c), region.files.begin() + static_cast<std::ptrdiff_t>(c) + 1);
                            first = 1;
                            found = true;
                        } catch (const std::exception&) {}
                    }
                    if (!found) { say("  ! " + bundle.name + ": no manifest found"); ++mod.errors; continue; }
                }
                auto add = [&](Node::Kind kind, const fb::BundleAsset& asset, std::size_t at) {
                    if (at >= region.files.size() || !region.files[at].location.patch) return;
                    Node n; n.kind = kind; n.name = lower(asset.name); n.file = region.files[at];
                    if (kind == Node::resource) n.resource_id = asset.resourceId;
                    if (kind == Node::chunk) n.guid = guid_key(asset.guid);
                    const auto index = mod.nodes.size();
                    if (kind == Node::resource) { mod.by_resource.emplace(n.resource_id, index); mod.res_by_name[n.name].push_back(index); }
                    if (kind == Node::chunk) mod.by_guid.emplace(n.guid, index);
                    if (counted.insert(placement_key(n.file)).second) mod.own_bytes += n.file.size;
                    mod.nodes.push_back(std::move(n));
                };
                for (std::size_t i = 0; i < manifest.ebx.size(); ++i) add(Node::ebx_asset, manifest.ebx[i], first + i);
                for (std::size_t i = 0; i < manifest.resources.size(); ++i) add(Node::resource, manifest.resources[i], first + manifest.ebx.size() + i);
                for (std::size_t i = 0; i < manifest.chunks.size(); ++i)
                    add(Node::chunk, manifest.chunks[i], first + manifest.ebx.size() + manifest.resources.size() + i);
            } catch (const std::exception& e) {
                say("  ! " + bundle.name + ": " + e.what()); ++mod.errors;
            }
        }
    }

    for (std::size_t i = 0; i < mod.nodes.size(); ++i)
        if (mod.nodes[i].kind == Node::chunk && mod.nodes[i].guid.size() == 16) {
            std::uint64_t prefix;
            std::memcpy(&prefix, mod.nodes[i].guid.data(), 8);
            mod.chunk_by_prefix[prefix].push_back(i);
        }
    say("   " + std::to_string(mod.nodes.size()) + " files of its own, " + mb(mod.own_bytes));

    // EBX are small: read every one, to know the items and the guid each is imported by.
    std::map<std::string, std::size_t> root_types;
    std::vector<char> linked(mod.nodes.size());
    std::vector<ebx::Document> docs(mod.nodes.size());
    std::vector<char> has_doc(mod.nodes.size());
    for (std::size_t i = 0; i < mod.nodes.size(); ++i) {
        auto& n = mod.nodes[i];
        if (n.kind != Node::ebx_asset) continue;
        try {
            const auto bytes = fb::decode_cas(archives.read(mod_dir, n.file.location, n.file.offset, n.file.size), {game});
            docs[i] = fb::ebx::read_document(bytes);
            has_doc[i] = 1;
            n.guid = guid_key(docs[i].fileGuid);
            n.root_type = docs[i].rootType;
            ++root_types[n.root_type];
            if (const auto* root = docs[i].root(); root && root->object)
                n.item = root->object->find("Key") && root->object->find("ItemData");
            mod.by_guid.emplace(n.guid, i);
            scan(bytes, mod, n.edges);
        } catch (const std::exception&) { ++mod.errors; }
    }
    // What a file names, worked out when an item first reaches it (resources are read only then).
    auto link = [&](std::size_t i) {
        if (linked[i]) return;
        linked[i] = 1;
        auto& n = mod.nodes[i];
        if (n.kind == Node::chunk) return;
        if (n.kind == Node::resource) {
            try { scan(fb::decode_cas(archives.read(mod_dir, n.file.location, n.file.offset, n.file.size), {game}), mod, n.edges); }
            catch (const std::exception&) { ++mod.errors; }
            return;
        }
        if (!has_doc[i]) return;
        const auto& doc = docs[i];
        for (const auto& imp : doc.imports)
            if (auto f = mod.by_guid.find(guid_key(imp.fileGuid)); f != mod.by_guid.end()) n.edges.push_back(f->second);
        std::vector<std::uint64_t> resources; std::vector<std::string> guids;
        for (const auto& inst : doc.instances) if (inst.object) walk(*inst.object, doc, resources, guids);
        for (auto id : resources) if (auto f = mod.by_resource.find(id); f != mod.by_resource.end()) n.edges.push_back(f->second);
        for (const auto& g : guids) if (auto f = mod.by_guid.find(g); f != mod.by_guid.end()) n.edges.push_back(f->second);
        if (auto f = mod.res_by_name.find(n.name); f != mod.res_by_name.end()) n.edges.insert(n.edges.end(), f->second.begin(), f->second.end());
    };

    // Each item's closure.
    struct ItemCost { std::string name; std::uint64_t bytes{}; std::size_t files{}; };
    std::vector<ItemCost> items;
    for (std::size_t i = 0; i < mod.nodes.size(); ++i) {
        if (!mod.nodes[i].item) continue;
        std::vector<char> seen(mod.nodes.size());
        std::deque<std::size_t> queue{i};
        seen[i] = 1;
        std::set<std::uint64_t> placed;
        ItemCost cost{mod.nodes[i].name};
        while (!queue.empty()) {
            const auto at = queue.front(); queue.pop_front();
            link(at);
            const auto& n = mod.nodes[at];
            if (placed.insert(placement_key(n.file)).second) { cost.bytes += n.file.size; ++cost.files; }
            for (auto next : n.edges) if (!seen[next]) { seen[next] = 1; queue.push_back(next); }
        }
        items.push_back(std::move(cost));
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::ostringstream head;
    head << "== " << mod_dir.filename().string() << ": " << items.size() << " items, mod's own files " << mb(mod.own_bytes)
         << " (" << mod.nodes.size() << " files), " << ms << " ms" << (mod.errors ? ", " + std::to_string(mod.errors) + " unreadable" : "");
    say(head.str());
    if (!items.empty()) {
        std::uint64_t total{};
        for (const auto& it : items) total += it.bytes;
        const auto median = items[items.size() / 2].bytes;
        say("   per worn item: largest " + mb(items.front().bytes) + ", median " + mb(median) + ", average " + mb(total / items.size()));
        for (std::size_t k = 0; k < items.size() && k < 8; ++k)
            say("     " + mb(items[k].bytes) + "  " + std::to_string(items[k].files) + " files  " + items[k].name);
        if (items.size() > 8) say("     ... and " + std::to_string(items.size() - 8) + " more");
    } else {
        std::string types;
        for (const auto& [t, c] : root_types) if (types.size() < 300) types += t + " x" + std::to_string(c) + ", ";
        say("   no items found; root types: " + types);
    }
}
} // namespace

int main(int argc, char** argv) {
    fs::path self = fs::absolute(argv[0]).parent_path();
    fs::path game = argc > 1 ? fs::path(argv[1]) : self.parent_path();
    fs::path out = argc > 2 ? fs::path(argv[2]) : self / "outfit-probe-report.txt";
    report.open(out);
    say("outfit-probe: game " + game.string());
    dingosdk::vfs::Layout layout;
    try { layout = dingosdk::vfs::read_layout(game / "Data" / "layout.toc"); } catch (const std::exception& e) {
        say(std::string("Cannot read the game's layout.toc: ") + e.what()); return 1;
    }
    dingosdk::vfs::GameArchives archives(game / "Data", layout.root);
    archives.keep_open();
    std::vector<fs::path> mods;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(game / "Mods", error))
        if (entry.is_directory() && entry.path().filename().string().front() != '.') mods.push_back(entry.path());
    std::sort(mods.begin(), mods.end());
    for (const auto& mod : mods) {
        try { probe(mod, game, archives); } catch (const std::exception& e) { say("== " + mod.filename().string() + ": failed, " + e.what()); }
    }
    say("done. Report: " + out.string());
    return 0;
}
