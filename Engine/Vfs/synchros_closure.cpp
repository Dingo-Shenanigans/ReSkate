#include "synchros_closure.h"
#include "Engine/Resource/bundle_ref_table.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/native_db.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <variant>

namespace dingosdk::synchros {
namespace fs = std::filesystem;
namespace fb = frostbite;
namespace ebx = frostbite::ebx;

namespace {
std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.filename().string());
    in.seekg(0, std::ios::end);
    std::vector<std::byte> out(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return out;
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}
std::string guid_key(const fb::Guid& guid) { return std::string(reinterpret_cast<const char*>(guid.bytes.data()), 16); }
std::string leaf(const std::string& name) { return name.substr(name.rfind('/') + 1); }
std::string without_win32(std::string name) { return name.starts_with("win32/") ? name.substr(6) : name; }
std::uint64_t placement(std::size_t source, const fb::BundleFileInfo& f) {
    return (static_cast<std::uint64_t>(source) << 56) ^ (static_cast<std::uint64_t>(f.location.archive) << 40) ^
           (static_cast<std::uint64_t>(f.location.installChunk) << 32) ^ f.offset;
}

void walk(const ebx::Value& value, const ebx::Document& doc, std::vector<std::uint64_t>& resources,
          std::vector<std::string>& guids, std::vector<std::string>& names);
void walk(const ebx::Object& object, const ebx::Document& doc, std::vector<std::uint64_t>& resources,
          std::vector<std::string>& guids, std::vector<std::string>& names) {
    for (const auto& field : object.fields) walk(field.value, doc, resources, guids, names);
}
void walk(const ebx::Value& value, const ebx::Document& doc, std::vector<std::uint64_t>& resources,
          std::vector<std::string>& guids, std::vector<std::string>& names) {
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ebx::ResourceReference>) resources.push_back(v.id);
        else if constexpr (std::is_same_v<T, fb::Guid>) guids.push_back(guid_key(v));
        else if constexpr (std::is_same_v<T, std::string>) { if (!v.empty()) names.push_back(lower(v)); }
        else if constexpr (std::is_same_v<T, ebx::PointerReference>) {
            if (v.kind == ebx::PointerKind::external && v.index >= 0 && static_cast<std::size_t>(v.index) < doc.imports.size())
                guids.push_back(guid_key(doc.imports[static_cast<std::size_t>(v.index)].fileGuid));
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ebx::Object>>) {
            if (v) walk(*v, doc, resources, guids, names);
        } else if constexpr (std::is_same_v<T, ebx::Value::Array>) {
            for (const auto& entry : v) walk(entry, doc, resources, guids, names);
        }
    }, value.data);
}
// The record at `index` of a chunk-metadata list, as a list of its own; empty when there is none.
std::vector<std::byte> one_record(const std::vector<std::byte>& list, std::size_t index) {
    if (list.empty()) return {};
    try {
        const std::span<const unsigned char> bytes(reinterpret_cast<const unsigned char*>(list.data()), list.size());
        auto root = native_db::read(bytes, "chunk metadata", nullptr, {.unique_fields = false, .max_entries = bytes.size()});
        if (index >= root.children.size()) return {};
        auto row = std::move(root.children[index]);
        root.children.clear();
        root.children.push_back(std::move(row));
        const auto written = native_db::write(root);
        const auto* first = reinterpret_cast<const std::byte*>(written.data());
        return {first, first + written.size()};
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace

struct Index::State {
    struct Node {
        Piece piece;
        std::string guid;                 // ebx: file guid; chunk: chunk guid
        std::vector<std::size_t> edges;
        bool linked{};
        bool item{};
        std::string key;                  // an item's Key
        std::shared_ptr<ebx::Document> doc;
    };
    std::vector<Source> sources;
    fs::path game;
    vfs::Layout layout;
    std::unique_ptr<vfs::GameArchives> archives;
    std::vector<Node> nodes;
    std::unordered_map<std::string, std::size_t> by_guid, by_name, res_by_name, by_key;
    // The game's item lists a mod adds its items to (items/itemmastercollection, a season's
    // 0.30.0_itemcollection), by the file GUID of each item they list: a received item is only in
    // the viewer's catalogue when one of them lists it (mod_merge_combine.cpp, item_list_name).
    std::map<std::string, std::vector<std::size_t>> collections_of;
    std::unordered_map<std::string, std::vector<std::size_t>> by_leaf, bundle_assets;
    std::unordered_map<std::uint64_t, std::size_t> by_resource;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> chunk_by_prefix;
    std::size_t unreadable{};
    // Each mod's bundle-reference table copies, read when a cut first needs them: per source,
    // table name -> its presets (lower-case path -> bundle index).
    std::vector<Piece> tables;        // every mod's copy: each holds that mod's own presets
    std::map<std::pair<std::size_t, std::string>, std::map<std::string, std::uint32_t>> presets;
    // Where each table is: (source, table name) -> (bundle, superbundle).
    std::map<std::pair<std::size_t, std::string>, std::pair<std::string, std::string>> table_homes;
    bool presets_read{};

    void read_presets() {
        if (presets_read) return;
        presets_read = true;
        for (const auto& p : tables) {
            try {
                const auto bytes = fb::decode_cas(encoded(p), {game});
                const fb::bundle_ref::Table table{bytes, p.asset.resourceMeta};
                auto& held = presets[{p.source, p.name}];
                table_homes[{p.source, p.name}] = {p.bundle, p.superbundle};
                for (const auto& [path, bundle] : fb::bundle_ref::presets(table)) held.emplace(lower(path), bundle);
            } catch (const std::exception&) {
                ++unreadable;
            }
        }
    }

    std::vector<std::byte> encoded(const Piece& p) const {
        return archives->read(sources.at(p.source).directory, p.file.location, p.file.offset, p.file.size);
    }

    // Every 8-byte run that is a resource id and every 16-byte run that is a chunk guid.
    void scan(std::span<const std::byte> bytes, std::vector<std::size_t>& out) const {
        for (std::size_t at = 0; at + 8 <= bytes.size(); ++at) {
            std::uint64_t id;
            std::memcpy(&id, bytes.data() + at, 8);
            if (!id) continue;
            if (const auto f = by_resource.find(id); f != by_resource.end()) out.push_back(f->second);
            if (at + 16 > bytes.size()) continue;
            if (const auto f = chunk_by_prefix.find(id); f != chunk_by_prefix.end())
                for (const auto index : f->second)
                    if (std::memcmp(nodes[index].guid.data(), bytes.data() + at, 16) == 0) out.push_back(index);
        }
    }

    void add_names(const std::vector<std::string>& names, std::vector<std::size_t>& edges) const {
        for (auto name : names) {
            name = without_win32(name);
            if (const auto f = by_name.find(name); f != by_name.end()) edges.push_back(f->second);
            // A look named by the last part of its path, with or without the preset's suffix.
            if (name.size() >= 6 && name.find(' ') == std::string::npos) {
                const auto last = leaf(name);
                for (const std::string& candidate : {last, last + "_ap", last + "_c"})
                    if (const auto f = by_leaf.find(candidate); f != by_leaf.end()) edges.insert(edges.end(), f->second.begin(), f->second.end());
            }
            // A bundle named whole is what loads; the shared bundle holds every item and is not one.
            if (name.find("sharedbundle") == std::string::npos)
                if (const auto b = bundle_assets.find(name); b != bundle_assets.end()) edges.insert(edges.end(), b->second.begin(), b->second.end());
        }
    }

    void link(std::size_t i) {
        auto& n = nodes[i];
        if (n.linked) return;
        n.linked = true;
        if (n.piece.kind == Piece::chunk || n.piece.kind == Piece::toc_chunk) return;
        if (n.piece.kind == Piece::resource) {
            try { scan(fb::decode_cas(encoded(n.piece), {game}), n.edges); } catch (const std::exception&) { ++unreadable; }
            return;
        }
        if (!n.doc) return;
        const auto& doc = *n.doc;
        std::vector<std::uint64_t> resources;
        std::vector<std::string> guids, names;
        for (const auto& imp : doc.imports) guids.push_back(guid_key(imp.fileGuid));
        for (const auto& inst : doc.instances) if (inst.object) walk(*inst.object, doc, resources, guids, names);
        for (const auto id : resources) if (const auto f = by_resource.find(id); f != by_resource.end()) n.edges.push_back(f->second);
        for (const auto& g : guids) if (const auto f = by_guid.find(g); f != by_guid.end()) n.edges.push_back(f->second);
        // A texture's or mesh's data is the resource of the asset's own name.
        if (const auto f = res_by_name.find(n.piece.name); f != res_by_name.end()) n.edges.push_back(f->second);
        add_names(names, n.edges);
        n.doc.reset();  // read again if ever needed; the cut has what it says
    }

    void index_source(std::size_t s) {
        const auto& dir = sources[s].directory;
        std::vector<fs::path> tocs;
        std::error_code error;
        for (auto it = fs::recursive_directory_iterator(dir / "Win32", error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
            if (it->is_regular_file(error)) {
                const auto name = lower(it->path().filename().string());
                if (name == "items.toc" || name == "globals.toc") tocs.push_back(it->path());
            }
        const fs::path data = game / "Data";
        for (const auto& path : tocs) {
            const std::string superbundle = "win32/" + lower(path.stem().string());
            fb::TocDocument toc;
            try { toc = fb::read_toc(read_file(path)); } catch (const std::exception&) { ++unreadable; continue; }
            for (const auto& chunk : toc.chunks) {
                if (!chunk.location.patch || chunk.removed) continue;
                const auto guid = guid_key(chunk.guid);
                if (by_guid.contains(guid)) continue;  // a higher mod's
                Node n;
                n.piece.kind = Piece::toc_chunk;
                n.piece.source = s;
                n.piece.file = {chunk.location, chunk.offset, chunk.size};
                n.piece.superbundle = superbundle;
                n.piece.asset.guid = chunk.guid;
                n.guid = guid;
                by_guid.emplace(guid, nodes.size());
                nodes.push_back(std::move(n));
            }
            for (const auto& bundle : toc.bundles) {
                try {
                    auto region = fb::read_bundle_region(bundle.region);
                    if (std::none_of(region.files.begin(), region.files.end(), [](const auto& f) { return f.location.patch; })) continue;
                    fb::BinaryBundle manifest;
                    std::size_t first = 0;
                    if (!region.inlineManifest.empty()) {
                        manifest = fb::read_binary_bundle(region.inlineManifest);
                    } else {
                        bool found = false;
                        const auto expected = region.files.size() - 1;
                        for (std::size_t c = 0; c < region.files.size() && !found; ++c) {
                            const auto& f = region.files[c];
                            if (f.size > (32u << 20)) continue;
                            try {
                                auto m = archives->read_manifest(f.location.patch ? dir : data, f.location, f.offset, f.size, game);
                                if (m.ebx.size() + m.resources.size() + m.chunks.size() != expected) continue;
                                manifest = std::move(m);
                                std::rotate(region.files.begin(), region.files.begin() + static_cast<std::ptrdiff_t>(c),
                                            region.files.begin() + static_cast<std::ptrdiff_t>(c) + 1);
                                first = 1;
                                found = true;
                            } catch (const std::exception&) {}
                        }
                        if (!found) { ++unreadable; continue; }
                    }
                    const auto bundle_name = lower(bundle.name);
                    auto& in_bundle = bundle_assets[without_win32(bundle_name)];
                    auto add = [&](Piece::Kind kind, const fb::BundleAsset& asset, std::size_t at) {
                        if (at >= region.files.size() || !region.files[at].location.patch) return;  // the game's own
                        const auto name = lower(asset.name);
                        // Every mod ships its own copy of a bundle-reference table under the
                        // game's name, with its own presets added: all of them are read, whichever
                        // wins the name, and none is ever a piece of an item.
                        if (kind == Piece::resource && fb::bundle_ref::is_table(name)) {
                            Piece table;
                            table.kind = Piece::resource;
                            table.source = s;
                            table.file = region.files[at];
                            table.name = name;
                            table.bundle = bundle_name;
                            table.superbundle = superbundle;
                            table.asset = asset;
                            tables.push_back(std::move(table));
                            return;
                        }
                        if (kind == Piece::chunk) {
                            if (by_guid.contains(guid_key(asset.guid))) return;
                        } else if (kind == Piece::resource ? res_by_name.contains(name)
                                                           : by_name.contains(name) && nodes[by_name[name]].piece.kind == Piece::ebx) {
                            return;  // a higher mod's
                        }
                        Node n;
                        n.piece.kind = kind;
                        n.piece.source = s;
                        n.piece.file = region.files[at];
                        n.piece.name = kind == Piece::chunk ? std::string() : name;
                        n.piece.bundle = bundle_name;
                        n.piece.superbundle = superbundle;
                        n.piece.asset = asset;
                        if (kind == Piece::chunk) n.piece.chunk_record = one_record(manifest.chunkMetadata, at - first - manifest.ebx.size() - manifest.resources.size());
                        const auto index = nodes.size();
                        if (kind == Piece::chunk) {
                            n.guid = guid_key(asset.guid);
                            by_guid.emplace(n.guid, index);
                        } else {
                            // EBX first by name; a resource only where no EBX has the name.
                            if (kind == Piece::ebx || !by_name.contains(name)) by_name[name] = index;
                            if (kind == Piece::resource) res_by_name.emplace(name, index);
                            by_leaf[leaf(name)].push_back(index);
                            if (kind == Piece::resource) by_resource.emplace(asset.resourceId, index);
                        }
                        in_bundle.push_back(index);
                        nodes.push_back(std::move(n));
                    };
                    for (std::size_t i = 0; i < manifest.ebx.size(); ++i) add(Piece::ebx, manifest.ebx[i], first + i);
                    for (std::size_t i = 0; i < manifest.resources.size(); ++i)
                        add(Piece::resource, manifest.resources[i], first + manifest.ebx.size() + i);
                    for (std::size_t i = 0; i < manifest.chunks.size(); ++i)
                        add(Piece::chunk, manifest.chunks[i], first + manifest.ebx.size() + manifest.resources.size() + i);
                } catch (const std::exception&) {
                    ++unreadable;
                }
            }
        }
    }

    void read_ebx() {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto& n = nodes[i];
            if (n.piece.kind != Piece::ebx) continue;
            try {
                const auto bytes = fb::decode_cas(encoded(n.piece), {game});
                auto doc = std::make_shared<ebx::Document>(ebx::read_document(bytes));
                n.guid = guid_key(doc->fileGuid);
                by_guid.emplace(n.guid, i);
                if (const auto* root = doc->root(); root && root->object) {
                    const auto* key = root->object->find("Key");
                    n.item = key && root->object->find("ItemData");
                    if (n.item)
                        if (const auto* text = std::get_if<std::string>(&key->value.data)) {
                            n.key = lower(*text);
                            by_key.emplace(n.key, i);
                        }
                }
                if (lower(doc->rootType) == "delmaritemcollectionasset")
                    for (const auto& listed : doc->imports) {
                        auto& lists = collections_of[guid_key(listed.fileGuid)];
                        if (std::find(lists.begin(), lists.end(), i) == lists.end()) lists.push_back(i);
                    }
                scan(bytes, n.edges);
                n.doc = std::move(doc);
            } catch (const std::exception&) {
                ++unreadable;
            }
        }
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if ((nodes[i].piece.kind == Piece::chunk || nodes[i].piece.kind == Piece::toc_chunk) && nodes[i].guid.size() == 16) {
                std::uint64_t prefix;
                std::memcpy(&prefix, nodes[i].guid.data(), 8);
                chunk_by_prefix[prefix].push_back(i);
            }
    }
};

Index::Index(std::vector<Source> sources, fs::path game) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.sources = std::move(sources);
    s.game = std::move(game);
    s.layout = vfs::read_layout(s.game / "Data" / "layout.toc");
    s.archives = std::make_unique<vfs::GameArchives>(s.game / "Data", s.layout.root);
    s.archives->keep_open();
    for (std::size_t i = 0; i < s.sources.size(); ++i) s.index_source(i);
    s.read_ebx();
}

Index::~Index() = default;

std::vector<std::string> Index::items() const {
    std::vector<std::string> out;
    for (const auto& n : state_->nodes) if (n.item) out.push_back(n.piece.name);
    return out;
}

std::optional<Cut> Index::cut(std::string_view item) {
    auto& s = *state_;
    const auto name = lower(std::string(item));
    auto found = s.by_name.find(name);
    std::size_t start{};
    if (found != s.by_name.end() && s.nodes[found->second].item) start = found->second;
    else if (const auto k = s.by_key.find(name); k != s.by_key.end()) start = k->second;
    else return std::nullopt;

    Cut cut{s.nodes[start].piece.name, {}, {}, 0};
    std::vector<char> seen(s.nodes.size());
    std::set<std::uint64_t> placed;
    std::deque<std::size_t> queue{start};
    seen[start] = 1;
    while (!queue.empty()) {
        const auto at = queue.front();
        queue.pop_front();
        s.link(at);
        const auto& n = s.nodes[at];
        // Another item reached by name (a collection, a variant's own item) is not part of this one,
        // and a whole table is not either: the rows below say what it adds for this item.
        if (at != start && n.item) continue;
        if (n.piece.kind == Piece::resource && fb::bundle_ref::is_table(n.piece.name)) continue;
        if (placed.insert(placement(n.piece.source, n.piece.file)).second) {
            cut.pieces.push_back(n.piece);
            cut.bytes += n.piece.file.size;
        }
        for (const auto next : n.edges)
            if (!seen[next]) { seen[next] = 1; queue.push_back(next); }
    }
    // The item lists that list it, so the viewer's catalogue has it: its own mod's copy of each list
    // (the merge keeps of a list only the entries whose items are in the bundle), another mod's when
    // its own mod has none.
    if (const auto lists = s.collections_of.find(s.nodes[start].guid); lists != s.collections_of.end()) {
        std::map<std::string, std::size_t> chosen;   // list name -> node
        for (const auto index : lists->second) {
            const auto& list = s.nodes[index];
            const auto [at, fresh] = chosen.try_emplace(list.piece.name, index);
            if (!fresh && s.nodes[at->second].piece.source != s.nodes[start].piece.source &&
                list.piece.source == s.nodes[start].piece.source)
                at->second = index;
        }
        for (const auto& [name, index] : chosen) {
            const auto& list = s.nodes[index].piece;
            if (placed.insert(placement(list.source, list.file)).second) {
                cut.pieces.push_back(list);
                cut.bytes += list.file.size;
            }
        }
    }
    // The presets among the pieces, as the mods' tables list them: the item's own mod first, then
    // the others in priority order.
    s.read_presets();
    const auto home = s.nodes[start].piece.source;
    std::set<std::string> done;
    for (const auto& piece : cut.pieces) {
        if (piece.kind != Piece::ebx || done.contains(piece.name)) continue;
        for (int pass = 0; pass < 2 && !done.contains(piece.name); ++pass)
            for (const auto& [where, held] : s.presets) {
                if ((pass == 0) != (where.first == home)) continue;
                if (const auto f = held.find(piece.name); f != held.end()) {
                    const auto& home_of = s.table_homes[where];
                    cut.rows.push_back({where.second, piece.name, f->second, home_of.first, home_of.second});
                    done.insert(piece.name);
                    break;
                }
            }
    }
    return cut;
}

std::vector<std::byte> Index::read(const Piece& piece) const { return state_->encoded(piece); }
std::size_t Index::unreadable() const noexcept { return state_->unreadable; }
std::pair<std::size_t, std::size_t> Index::tables() {
    state_->read_presets();
    std::size_t presets = 0;
    for (const auto& [where, held] : state_->presets) presets += held.size();
    return {state_->presets.size(), presets};
}
const std::vector<Source>& Index::sources() const noexcept { return state_->sources; }

std::vector<std::array<std::string, 3>> Index::referrers(std::string_view item) {
    auto& s = *state_;
    std::vector<std::array<std::string, 3>> out;
    const auto name = lower(std::string(item));
    const auto found = s.by_name.find(name);
    if (found == s.by_name.end()) return out;
    const auto& guid = s.nodes[found->second].guid;
    for (const auto& n : s.nodes) {
        if (!n.doc) continue;
        for (const auto& imp : n.doc->imports)
            if (guid_key(imp.fileGuid) == guid) {
                out.push_back({n.piece.name, n.doc->rootType, s.sources[n.piece.source].name});
                break;
            }
    }
    return out;
}
} // namespace dingosdk::synchros
