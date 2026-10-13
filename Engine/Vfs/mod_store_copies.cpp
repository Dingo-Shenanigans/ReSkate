#include "mod_store_copies.h"

#include "mod_catalog.h"
#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/mesh_set.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace dingosdk::mods {
using namespace detail;
namespace {
// Where the game keeps its items, the store's among them, and how it names them:
// most under items/, the rest (player-card art, skin tones) after their key,
// and every key the store sells starts with own_. A cheap way to leave the
// textures and presets that fill the rest of the bundle unread.
constexpr std::string_view items_toc = "Win32/items.toc";
bool item_name(std::string_view name) {
    const auto slash = name.rfind('/');
    return name.starts_with("items/") ||
           name.substr(slash == std::string_view::npos ? 0 : slash + 1).starts_with("own_");
}

// The game's whole costumes, the ones a skater wears instead of clothes: where their items
// are, and how a costume's own bundle is named after the preset its item is fitted on (an
// "asset 1" shape). The bundle holds the costume's mesh, a MeshSet resource; its coarser levels
// of detail are chunks of the bundle and the finest is one of the superbundle's own.
constexpr std::string_view costume_items = "items/cust_fullbodycostume/";
constexpr std::string_view costume_shape = "asset 1 ";
constexpr std::string_view costume_bundle_suffix = "_cas_main_bundlereftable";
constexpr std::uint32_t mesh_set_type = 0x49b156d4;
// The levels of detail of a mod's mesh that are read: a copy has the finest ones.
constexpr std::size_t mod_mesh_lods = 3;

// An item's AssetPaths say what kind of asset each one is: 1 a morph preset and
// 2 a base mesh, which are what the item is fitted on; every other kind (3, an
// appearance preset, for nearly all of them) is what it looks like. Of the
// game's own items, store and free ones share shapes and never a look.
constexpr bool shape_kind(std::int64_t kind) noexcept { return kind == 1 || kind == 2; }

std::string hex(const fb::Sha1& sha1) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto value : sha1.bytes) {
        const auto byte = static_cast<unsigned>(value);
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

const std::string* text_of(const fb::ebx::Object& object, std::string_view name) {
    const auto* field = object.find(name);
    return field ? std::get_if<std::string>(&field->value.data) : nullptr;
}
std::optional<std::int64_t> integer_of(const fb::ebx::Object& object, std::string_view name) {
    const auto* field = object.find(name);
    if (!field) return std::nullopt;
    if (const auto* value = std::get_if<std::int64_t>(&field->value.data)) return *value;
    if (const auto* value = std::get_if<std::uint64_t>(&field->value.data)) return static_cast<std::int64_t>(*value);
    return std::nullopt;
}

// Walks an item's data: every field goes into `text`, in order and with what
// its pointers lead to, and the fields that say what the item is go into its
// looks and shapes.
class ItemReader final {
public:
    ItemReader(const fb::ebx::Document& document, ItemContent& item, std::string& text)
        : document_(document), item_(item), text_(text) {}

    void value(const fb::ebx::Value& value, int depth = 0) {
        // Deeper than any item's data goes; the walk has to end on any document.
        if (depth > 32) { text_ += '?'; return; }
        struct Visit {
            ItemReader& reader;
            int depth;
            void operator()(std::monostate) const { reader.text_ += '~'; }
            void operator()(bool held) const { reader.text_ += held ? "true" : "false"; }
            void operator()(std::int64_t held) const { reader.text_ += std::to_string(held); }
            void operator()(std::uint64_t held) const { reader.text_ += std::to_string(held); }
            void operator()(double held) const { reader.text_ += std::to_string(held); }
            void operator()(const std::string& held) const { reader.text_ += '"' + lower(held) + '"'; }
            void operator()(const fb::Guid& held) const { reader.text_ += held.string(); }
            void operator()(const fb::Sha1& held) const { reader.text_ += hex(held); }
            void operator()(const fb::ebx::ResourceReference& held) const { reader.text_ += "resource " + std::to_string(held.id); }
            void operator()(const fb::ebx::PointerReference& held) const { reader.pointer(held, depth); }
            void operator()(const fb::ebx::TypeReference& held) const { reader.text_ += "type " + std::to_string(held.encoded); }
            void operator()(const fb::ebx::BoxedReference& held) const { reader.text_ += "boxed " + std::to_string(held.encodedType); }
            void operator()(const std::shared_ptr<fb::ebx::Object>& held) const {
                if (held) reader.object(*held, depth);
                else reader.text_ += "null";
            }
            void operator()(const fb::ebx::Value::Array& held) const {
                reader.text_ += '[';
                for (const auto& entry : held) { reader.value(entry, depth + 1); reader.text_ += ','; }
                reader.text_ += ']';
            }
        };
        std::visit(Visit{*this, depth}, value.data);
    }
    // Whether the data has a field at all; the player-card items have none.
    [[nodiscard]] bool any() const noexcept { return fields_ != 0; }

private:
    [[nodiscard]] const fb::ebx::ImportReference* import(const fb::ebx::PointerReference& reference) const {
        if (reference.kind != fb::ebx::PointerKind::external || reference.index < 0 ||
            static_cast<std::size_t>(reference.index) >= document_.imports.size())
            return nullptr;
        return &document_.imports[static_cast<std::size_t>(reference.index)];
    }
    static std::string named(const fb::ebx::ImportReference& reference) {
        return reference.fileGuid.string() + '/' + reference.classGuid.string();
    }

    void pointer(const fb::ebx::PointerReference& reference, int depth) {
        if (reference.kind == fb::ebx::PointerKind::null) { text_ += "null"; return; }
        if (const auto* other = import(reference)) { text_ += "import " + named(*other); return; }
        const auto index = static_cast<std::size_t>(reference.index);
        if (reference.kind != fb::ebx::PointerKind::internal || reference.index < 0 ||
            index >= document_.instances.size() || !document_.instances[index].object) {
            text_ += "nothing";
            return;
        }
        // An instance that leads back to itself is written once.
        if (std::find(open_.begin(), open_.end(), index) != open_.end()) { text_ += "again"; return; }
        open_.push_back(index);
        object(*document_.instances[index].object, depth);
        open_.pop_back();
    }

    void object(const fb::ebx::Object& object, int depth) {
        parts(object);
        if (object.descriptor >= 0 && static_cast<std::size_t>(object.descriptor) < document_.types.size())
            text_ += document_.types[static_cast<std::size_t>(object.descriptor)].name;
        text_ += '{';
        for (const auto& field : object.fields) {
            ++fields_;
            text_ += field.name;
            text_ += '=';
            value(field.value, depth + 1);
            text_ += ';';
        }
        text_ += '}';
    }

    void parts(const fb::ebx::Object& object) {
        // Clothing, decks, wheels, trucks: the assets the item is made of, by name.
        if (const auto* name = text_of(object, "AssetName"); name && !name->empty())
            if (const auto kind = integer_of(object, "AssetTypeId"))
                (shape_kind(*kind) ? item_.shapes : item_.looks)
                    .push_back("asset " + std::to_string(*kind) + ' ' + lower(*name));
        // Stickers and tattoos: the textures of the decal.
        if (const auto* name = text_of(object, "TextureName"); name && !name->empty())
            item_.looks.push_back("texture " + lower(*name));
        // Quick Drop objects: what is spawned, and the bundle it is in.
        if (const auto* name = text_of(object, "BuildKitPrefabName"); name && !name->empty())
            item_.looks.push_back("prefab " + lower(*name));
        if (const auto* name = text_of(object, "BuildKitBundleName"); name && !name->empty())
            item_.looks.push_back("bundle " + lower(*name));
        // Player-card backgrounds and icons: the picture, which is another asset.
        if (const auto* texture = object.find("ItemTexture"))
            if (const auto* reference = std::get_if<fb::ebx::PointerReference>(&texture->value.data))
                if (const auto* other = import(*reference)) item_.looks.push_back("picture " + named(*other));
        // Emotes: a value of the game's gesture state, which is the animation played.
        if (const auto* state = object.find("GameState"))
            if (const auto gesture = integer_of(object, "Value"))
                if (const auto* held = std::get_if<std::shared_ptr<fb::ebx::Object>>(&state->value.data); held && *held)
                    if (const auto* asset = (*held)->find("AssetRef"))
                        if (const auto* reference = std::get_if<fb::ebx::PointerReference>(&asset->value.data))
                            if (const auto* other = import(*reference))
                                item_.looks.push_back("gesture " + named(*other) + ' ' + std::to_string(*gesture));
    }

    const fb::ebx::Document& document_;
    ItemContent& item_;
    std::string& text_;
    std::vector<std::size_t> open_;
    std::size_t fields_{};
};

class Scan final {
public:
    Scan(const Catalog& catalog, std::vector<std::string>* notes)
        : baseRoot_(catalog.data_root / L"Data"), gameRoot_(catalog.data_root),
          layout_(vfs::read_layout(baseRoot_ / L"layout.toc")),
          store_(baseRoot_, catalog.root / generated_folder, layout_.root), notes_(notes) {}

    // The meshes a mod ships, each level of detail read as a mesh of its own: every MeshSet in
    // a bundle of its own files, whatever it is called and whether an item names it.
    struct Mesh {
        std::string name;
        std::vector<std::uint64_t> points;
    };
    [[nodiscard]] std::vector<Mesh> read_mod_meshes(const Mod& mod) {
        std::vector<Mesh> meshes;
        for (const auto& relative : scan(mod.directory).tocs) {
            fb::TocDocument own;
            try {
                own = fb::read_toc(read_file(mod.directory / fs::path(relative)));
            } catch (const std::exception&) {
                continue;  // (read_mod notes it)
            }
            for (const auto& bundle : own.bundles) {
                try {
                    const auto region = fb::read_bundle_region(bundle.region);
                    if (std::none_of(region.files.begin(), region.files.end(),
                                     [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                        continue;
                    const auto listing = list_bundle(store_, mod.directory, baseRoot_, bundle, gameRoot_);
                    if (!listing) continue;
                    const auto& manifest = listing->manifest;
                    for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
                        const auto at = listing->first + manifest.ebx.size() + index;
                        if (manifest.resources[index].resourceType != mesh_set_type || at >= listing->files.size() ||
                            !listing->files[at].location.patch)
                            continue;
                        read_mesh(mod.directory, *listing, manifest.ebx.size() + index, own, mod_mesh_lods,
                                  [&](std::vector<std::uint64_t> points) {
                                      meshes.push_back({manifest.resources[index].name, std::move(points)});
                                  });
                    }
                } catch (const std::exception& failure) {
                    note(mod.name + ": " + bundle.name + " could not be read for " + std::string(store_copies_check) + " (" + failure.what() + ")");
                }
            }
        }
        return meshes;
    }

    // The items a mod adds to the game's bundles, and the game's own it changes.
    // An item only loads from a bundle the game loads, and the lists the game
    // reads its items from are in one of those, so a mod's own bundles (a map's
    // levels) are not looked at.
    [[nodiscard]] std::vector<ItemContent> read_mod(const Mod& mod) {
        std::vector<ItemContent> items;
        for (const auto& relative : scan(mod.directory).tocs) {
            const auto* game = game_toc(relative);
            if (!game) continue;
            fb::TocDocument own;
            try {
                own = fb::read_toc(read_file(mod.directory / fs::path(relative)));
            } catch (const std::exception& failure) {
                note(mod.name + ": " + relative + " could not be read for " + std::string(store_copies_check) + " (" + failure.what() + ")");
                continue;
            }
            for (const auto& bundle : own.bundles) {
                const auto shipped = game->find(lower(bundle.name));
                if (shipped == game->end()) continue;
                try {
                    // A bundle the mod only passes through reads every file from the game.
                    const auto region = fb::read_bundle_region(bundle.region);
                    if (std::none_of(region.files.begin(), region.files.end(),
                                     [](const fb::BundleFileInfo& file) { return file.location.patch; }))
                        continue;
                    const auto listing = list_bundle(store_, mod.directory, baseRoot_, bundle, gameRoot_);
                    const auto* known = listing ? game_assets(relative, *shipped->second) : nullptr;
                    if (!known) continue;
                    for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index) {
                        const auto& asset = listing->manifest.ebx[index];
                        const auto at = listing->first + index;
                        // The mod's own payloads are in its own archives.
                        if (at >= listing->files.size() || !listing->files[at].location.patch) continue;
                        const auto original = known->find(lower(asset.name));
                        if (original != known->end() && original->second == asset.sha1) continue;
                        try {
                            const auto document = fb::ebx::read_document(
                                read_asset(store_, mod.directory, baseRoot_, *listing, index, gameRoot_));
                            if (auto item = item_content(document)) items.push_back(std::move(*item));
                        } catch (const std::exception&) {
                            // Not an asset this reads; the game may not read it either.
                        }
                    }
                } catch (const std::exception& failure) {
                    note(mod.name + ": " + bundle.name + " could not be read for " + std::string(store_copies_check) + " (" + failure.what() + ")");
                }
            }
        }
        return items;
    }

    // Every item the game ships, and with `meshes` the meshes its items have: the whole
    // costumes' under their items, every other one as nobody's (a costume is partly made of
    // clothes anyone has). How many items were read, and how many costumes' meshes.
    //
    // Each of the game's item bundles is read by itself, on up to `threads` threads beside
    // this one; what was read is then filed in the bundles' own order, so a part two items
    // share is the same item's whatever the count.
    struct GameCounts {
        std::size_t items{}, costumes{};
    };
    [[nodiscard]] GameCounts read_game(StoreItems& items, const StoreItem& sold, StoreMeshes* meshes, std::size_t threads,
                                       bool background) {
        const auto* toc = game_toc(items_toc);
        if (!toc) return {};
        const auto& document = tocs_.at(lower(items_toc)).document;
        struct Read {
            struct Item {
                ItemContent content;
                bool costume{};   // one of the game's whole costumes, by where its asset is
            };
            std::vector<Item> items;
            struct Mesh {
                std::string resource;
                std::vector<std::vector<std::uint64_t>> levels;
            };
            std::vector<Mesh> meshes;
            std::string note;
        };
        std::vector<const fb::TocBundle*> bundles;
        for (const auto& [name, bundle] : *toc) bundles.push_back(bundle);
        std::vector<Read> reads(bundles.size());
        const auto read_bundle = [&](std::size_t at) {
            auto& read = reads[at];
            try {
                const auto listing = list_bundle(store_, baseRoot_, baseRoot_, *bundles[at], gameRoot_);
                if (!listing) return;
                const auto& manifest = listing->manifest;
                for (std::size_t index = 0; index < manifest.ebx.size(); ++index) {
                    const auto name = lower(manifest.ebx[index].name);
                    if (!item_name(name)) continue;
                    try {
                        const auto asset = fb::ebx::read_document(read_asset(store_, baseRoot_, baseRoot_, *listing, index, gameRoot_));
                        if (auto item = item_content(asset)) read.items.push_back({std::move(*item), name.starts_with(costume_items)});
                    } catch (const std::exception&) {}
                }
                if (!meshes) return;
                for (std::size_t index = 0; index < manifest.resources.size(); ++index) {
                    if (manifest.resources[index].resourceType != mesh_set_type) continue;
                    Read::Mesh mesh{manifest.resources[index].name, {}};
                    read_mesh(baseRoot_, *listing, manifest.ebx.size() + index, document, 16,
                              [&](std::vector<std::uint64_t> points) { mesh.levels.push_back(std::move(points)); });
                    if (!mesh.levels.empty()) read.meshes.push_back(std::move(mesh));
                }
            } catch (const std::exception& failure) {
                read.note = "The game's " + bundles[at]->name + " could not be read for " + std::string(store_copies_check) + " (" + failure.what() + ")";
            }
        };
        {
            std::atomic<std::size_t> next{0};
            const auto work = [&] {
                for (;;) {
                    const auto mine = next.fetch_add(1);
                    if (mine >= bundles.size()) return;
                    read_bundle(mine);
                }
            };
            std::vector<std::jthread> workers;
            try {
                // Threads of their own, not the system's pool (see the superbundles in mod_merge.cpp).
                for (std::size_t index = 0; index < threads && bundles.size() > 1; ++index)
                    workers.emplace_back([&] {
                        if (background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                        work();
                    });
            } catch (const std::system_error&) {} // fewer threads, or none: this one does the rest
            work();
        }
        GameCounts counts;
        for (auto& read : reads) {
            if (!read.note.empty()) note(std::move(read.note));
            for (const auto& item : read.items) {
                const bool store = sold(lower(item.content.key));
                items.add(item.content, store);
                ++counts.items;
                if (!item.costume) continue;
                for (const auto& shape : item.content.shapes)
                    if (shape.starts_with(costume_shape))
                        costumes_.insert_or_assign(shape.substr(costume_shape.size()), Costume{item.content.key, store});
            }
        }
        if (!meshes || costumes_.empty()) return counts;
        // A costume's own bundle is named after the preset its item is fitted on.
        for (std::size_t at = 0; at < bundles.size(); ++at) {
            const auto name = lower(bundles[at]->name);
            const Costume* costume{};
            if (name.ends_with(costume_bundle_suffix)) {
                const auto stem = name.substr(0, name.size() - costume_bundle_suffix.size());
                const auto slash = stem.rfind('/');
                const auto found = costumes_.find(stem.substr(slash == std::string::npos ? 0 : slash + 1));
                if (found != costumes_.end()) costume = &found->second;
            }
            for (const auto& mesh : reads[at].meshes)
                for (const auto& level : mesh.levels) {
                    // (Each other mesh under its own name, so two of them sharing a point is also seen.)
                    if (costume) meshes->add(costume->key, costume->sold, level);
                    else meshes->add("mesh " + mesh.resource, false, level);
                }
            counts.costumes += costume && !reads[at].meshes.empty();
        }
        return counts;
    }

private:
    using Bundles = std::map<std::string, const fb::TocBundle*, std::less<>>;
    using Assets = std::map<std::string, fb::Sha1, std::less<>>;

    // (Both of these are asked by several threads while the mods are read: one at a time,
    // and what they hand out stays where it is.)
    const Bundles* game_toc(std::string_view relative) {
        std::lock_guard lock(cache_mutex_);
        const auto key = lower(relative);
        if (const auto found = tocs_.find(key); found != tocs_.end())
            return found->second.index.empty() ? nullptr : &found->second.index;
        auto& entry = tocs_[key];
        std::error_code error;
        const auto path = baseRoot_ / fs::path(std::string(relative));
        if (!fs::is_regular_file(path, error)) return nullptr;
        try {
            entry.document = fb::read_toc(read_file(path));
        } catch (const std::exception& failure) {
            note("The game's " + std::string(relative) + " could not be read for " + std::string(store_copies_check) + " (" + failure.what() + ")");
            return nullptr;
        }
        for (const auto& bundle : entry.document.bundles) entry.index.emplace(lower(bundle.name), &bundle);
        return entry.index.empty() ? nullptr : &entry.index;
    }

    // The game's copy of a bundle: each EBX it holds, to tell a mod's additions
    // and changes from what it carries untouched. Null when it cannot be read.
    const Assets* game_assets(std::string_view relative, const fb::TocBundle& bundle) {
        std::lock_guard lock(cache_mutex_);
        const auto key = lower(relative) + '|' + lower(bundle.name);
        if (const auto found = assets_.find(key); found != assets_.end())
            return found->second ? &*found->second : nullptr;
        auto& entry = assets_[key];
        const auto listing = list_bundle(store_, baseRoot_, baseRoot_, bundle, gameRoot_);
        if (!listing) return nullptr;
        entry.emplace();
        for (const auto& asset : listing->manifest.ebx) entry->emplace(lower(asset.name), asset.sha1);
        return &*entry;
    }

    // One MeshSet (the listing's `file`th file) as points, a level of detail at a time, up to
    // `lods` of them. A level's geometry is a chunk of the bundle or of its superbundle `toc`. A
    // mesh or a level this cannot read is left out: most are not laid out as a costume's is.
    template <class Each>
    void read_mesh(const fs::path& root, const Listing& listing, std::size_t file, const fb::TocDocument& toc, std::size_t lods,
                   Each&& each) {
        std::vector<std::byte> resource;
        std::size_t count{};
        try {
            resource = read_asset(store_, root, baseRoot_, listing, file, gameRoot_);
            count = std::min(fb::mesh_lod_count(resource), lods);
        } catch (const std::exception&) {
            return;
        }
        const auto& manifest = listing.manifest;
        for (std::size_t lod = 0; lod < count; ++lod) {
            try {
                const auto wanted = fb::mesh_lod_geometry(resource, lod).chunk;
                std::vector<std::byte> geometry;
                for (std::size_t chunk = 0; chunk < manifest.chunks.size() && geometry.empty(); ++chunk)
                    if (manifest.chunks[chunk].guid == wanted)
                        geometry = read_asset(store_, root, baseRoot_, listing, manifest.ebx.size() + manifest.resources.size() + chunk, gameRoot_);
                for (auto chunk = toc.chunks.begin(); chunk != toc.chunks.end() && geometry.empty(); ++chunk)
                    if (chunk->guid == wanted && !chunk->removed)
                        geometry = fb::decode_cas(store_.read(chunk->location.patch ? root : baseRoot_, chunk->location,
                                                              chunk->offset, chunk->size), {gameRoot_});
                if (geometry.empty()) continue;
                auto points = mesh_points(fb::read_mesh_positions(resource, lod, geometry));
                if (!points.empty()) each(std::move(points));
            } catch (const std::exception&) {}
        }
    }

    // What a mod's reading has to say is kept with that mod while the mods are read at once
    // (notes_for), so the notes come out in the mods' order however the threads ran.
    void note(std::string text) {
        if (note_sink) note_sink->push_back(std::move(text));
        else if (notes_) notes_->push_back(std::move(text));
    }

    struct GameToc {
        fb::TocDocument document;
        Bundles index;
    };
    fs::path baseRoot_, gameRoot_;
    vfs::Layout layout_;
    CasStore store_;
    std::vector<std::string>* notes_;
    std::map<std::string, GameToc, std::less<>> tocs_;
    std::map<std::string, std::optional<Assets>, std::less<>> assets_;
    std::mutex cache_mutex_;
    static inline thread_local std::vector<std::string>* note_sink = nullptr;

public:
    // While one lives, this thread's notes go into `kept`.
    struct NotesFor {
        explicit NotesFor(std::vector<std::string>& kept) { note_sink = &kept; }
        ~NotesFor() { note_sink = nullptr; }
        NotesFor(const NotesFor&) = delete;
        NotesFor& operator=(const NotesFor&) = delete;
    };
    void say(std::vector<std::string>& kept) {
        if (notes_) notes_->insert(notes_->end(), std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()));
    }

private:
    // The game's whole costumes: the preset each is fitted on -> its item.
    struct Costume {
        std::string key;
        bool sold{};
    };
    std::map<std::string, Costume, std::less<>> costumes_;
};
} // namespace

std::optional<ItemContent> item_content(const fb::ebx::Document& document) {
    const auto* root = document.root();
    if (!root || !root->object) return std::nullopt;
    const auto* key = text_of(*root->object, "Key");
    const auto* data = root->object->find("ItemData");
    if (!key || key->empty() || !data) return std::nullopt;
    ItemContent item;
    item.key = *key;
    if (const auto* name = text_of(*root->object, "Name")) item.costume = lower(*name).starts_with(costume_items);
    std::string text;
    ItemReader reader(document, item, text);
    reader.value(data->value);
    for (auto* parts : {&item.looks, &item.shapes}) {
        std::sort(parts->begin(), parts->end());
        parts->erase(std::unique(parts->begin(), parts->end()), parts->end());
    }
    // Data with no field in it is every such item's alike, and says nothing.
    if (reader.any()) item.data = hex(sha1_of(std::as_bytes(std::span(text))));
    return item;
}

const std::string* StoreItems::Parts::owner(std::string_view part) const {
    if (free.contains(part)) return nullptr;
    const auto found = sold.find(part);
    return found == sold.end() ? nullptr : &found->second;
}

void StoreItems::add(const ItemContent& item, bool sold) {
    const auto file = [&](Parts& parts, const std::string& part) {
        if (sold) parts.sold.try_emplace(part, item.key);
        else parts.free.insert(part);
    };
    for (const auto& look : item.looks) file(looks_, look);
    // A store item with a look is told by that; its shape is anyone's to use. A whole
    // costume's is not: the shape is the costume's own mesh, with or without looks on it.
    if (!sold || item.looks.empty() || item.costume)
        for (const auto& shape : item.shapes) file(shapes_, shape);
    if (!item.data.empty()) file(data_, item.data);
}

std::string StoreItems::original(const ItemContent& item) const {
    for (const auto& look : item.looks)
        if (const auto* key = looks_.owner(look)) return *key;
    // (Only a store item with no look files its shapes: a whole costume. A look of the
    // mod's own over one does not make the costume the mod's.)
    for (const auto& shape : item.shapes)
        if (const auto* key = shapes_.owner(shape)) return *key;
    if (!item.data.empty())
        if (const auto* key = data_.owner(item.data)) return *key;
    return {};
}

std::uint64_t mesh_point(const std::array<float, 3>& position) noexcept {
    // Half a millimetre a step, 21 bits an axis: a kilometre either way, far past any skater.
    std::uint64_t point{};
    for (const float coordinate : position) {
        const auto step = std::isfinite(coordinate) ? std::llround(std::clamp(coordinate, -500.0f, 500.0f) * 2000.0f) : 0;
        point = point << 21 | (static_cast<std::uint64_t>(step + (1 << 20)) & 0x1fffff);
    }
    return point;
}

std::vector<std::uint64_t> mesh_points(const std::vector<std::array<float, 3>>& positions) {
    std::vector<std::uint64_t> points;
    points.reserve(positions.size());
    for (const auto& position : positions) points.push_back(mesh_point(position));
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

void StoreMeshes::add(std::string_view key, bool sold, const std::vector<std::uint64_t>& points) {
    auto found = by_key_.find(key);
    if (found == by_key_.end()) {
        found = by_key_.emplace(std::string(key), static_cast<std::uint32_t>(owners_.size())).first;
        owners_.push_back({std::string(key), sold, 0});
    }
    const auto owner = found->second;
    owners_[owner].largest = std::max(owners_[owner].largest, points.size());
    for (const auto point : points) {
        const auto [place, fresh] = points_.try_emplace(point, owner);
        // A second mesh has it too: it is neither's.
        if (!fresh && place->second != owner) place->second = nobody;
    }
}

std::string StoreMeshes::original(const std::vector<std::uint64_t>& points) const {
    if (points.size() < shared_least) return {};
    std::vector<std::size_t> shared(owners_.size());
    for (const auto point : points)
        if (const auto found = points_.find(point); found != points_.end() && found->second != nobody) ++shared[found->second];
    const std::string* most{};
    std::size_t count{};
    for (std::size_t owner = 0; owner < owners_.size(); ++owner) {
        const auto& costume = owners_[owner];
        if (!costume.sold || shared[owner] < shared_least || shared[owner] <= count) continue;
        if (shared[owner] * 10 >= costume.largest || shared[owner] * 10 >= points.size() * 3) most = &costume.key, count = shared[owner];
    }
    return most ? *most : std::string();
}

StoreMeshes::Closest StoreMeshes::closest(const std::vector<std::uint64_t>& points) const {
    std::vector<std::size_t> shared(owners_.size());
    for (const auto point : points)
        if (const auto found = points_.find(point); found != points_.end() && found->second != nobody) ++shared[found->second];
    Closest most;
    for (std::size_t owner = 0; owner < owners_.size(); ++owner)
        if (owners_[owner].sold && shared[owner] > most.shared) most = {owners_[owner].key, shared[owner], owners_[owner].largest};
    return most;
}

StoreCopies check_store_copies(const Catalog& catalog, const StoreItem& sold, std::vector<std::string>* notes,
                               std::size_t threads, bool background) noexcept {
    StoreCopies result;
    try {
        if (!sold || std::none_of(catalog.mods.begin(), catalog.mods.end(), [](const Mod& mod) { return mod.provides_layout; }))
            return result;
        Scan scan(catalog, notes);
        struct Added {
            const Mod* mod{};
            std::vector<ItemContent> items;
            std::vector<Scan::Mesh> meshes;
        };
        std::vector<Added> added;
        {
            // Each mod is read by itself, several at a time; they are gone through in their own
            // order afterwards.
            std::vector<const Mod*> asked;
            for (const auto& mod : catalog.mods)
                if (mod.provides_layout) asked.push_back(&mod);
            std::vector<Added> reads(asked.size());
            std::vector<std::vector<std::string>> said(asked.size());
            std::atomic<std::size_t> next{0};
            const auto work = [&] {
                for (;;) {
                    const auto mine = next.fetch_add(1);
                    if (mine >= asked.size()) return;
                    const Scan::NotesFor kept(said[mine]);
                    try {
                        reads[mine] = {asked[mine], scan.read_mod(*asked[mine]), scan.read_mod_meshes(*asked[mine])};
                    } catch (...) {
                        reads[mine] = {asked[mine], {}, {}};   // (neither reader throws; a mod that cannot be read adds nothing)
                    }
                }
            };
            {
                std::vector<std::jthread> workers;
                try {
                    for (std::size_t index = 0; index < threads && asked.size() > 1; ++index)
                        workers.emplace_back([&] {
                            if (background) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                            work();
                        });
                } catch (const std::system_error&) {} // fewer threads, or none: this one does the rest
                work();
            }
            for (std::size_t index = 0; index < asked.size(); ++index) {
                scan.say(said[index]);
                result.mod_meshes += reads[index].meshes.size();
                if (!reads[index].items.empty() || !reads[index].meshes.empty()) added.push_back(std::move(reads[index]));
            }
        }
        if (added.empty()) return result;
        // The costumes' meshes are only read when a mod ships a mesh at all.
        StoreItems game;
        StoreMeshes costumes;
        const auto counts = scan.read_game(game, sold, result.mod_meshes ? &costumes : nullptr, threads, background);
        result.game_items = counts.items;
        result.game_costumes = counts.costumes;
        if (!result.game_items) {
            if (notes) notes->push_back(std::string(store_copies_check) + " could not read the game's own files and did not run");
            return result;
        }
        for (const auto& [mod, items, meshes] : added) {
            StoreCopies::Source source{mod->name};
            for (const auto& mesh : meshes) {
                result.meshes_seen.push_back({mod->name, mesh.name, mesh.points.size(), costumes.closest(mesh.points)});
                const auto original = costumes.original(mesh.points);
                if (original.empty()) continue;
                if (!source.count++) {
                    source.example = mesh.name;
                    source.original = original;
                    source.by_mesh = true;
                }
                result.items.insert_or_assign(lower(mesh.name), lower(original));
            }
            for (const auto& item : items) {
                const auto key = lower(item.key);
                // A store item under its own key is held by that key already.
                if (sold(key)) continue;
                const auto original = game.original(item);
                if (original.empty()) continue;
                if (!source.count++) {
                    source.example = item.key;
                    source.original = original;
                }
                result.items.insert_or_assign(key, lower(original));
            }
            if (source.count) result.mods.push_back(std::move(source));
        }
    } catch (const std::exception& failure) {
        if (notes) notes->push_back(std::string(store_copies_check) + " could not run: " + failure.what());
        result = {};
    } catch (...) {
        result = {};
    }
    return result;
}

} // namespace dingosdk::mods
