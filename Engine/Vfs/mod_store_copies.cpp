#include "mod_store_copies.h"

#include "mod_catalog.h"
#include "mod_merge_internal.h"
#include "Engine/Resource/ebx_document.h"

#include <algorithm>
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

    // Every item the game ships; how many were read.
    [[nodiscard]] std::size_t read_game(StoreItems& items, const StoreItem& sold) {
        const auto* toc = game_toc(items_toc);
        if (!toc) return 0;
        std::size_t read{};
        for (const auto& [name, bundle] : *toc) {
            try {
                const auto listing = list_bundle(store_, baseRoot_, baseRoot_, *bundle, gameRoot_);
                if (!listing) continue;
                for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index) {
                    if (!item_name(lower(listing->manifest.ebx[index].name))) continue;
                    try {
                        const auto document = fb::ebx::read_document(
                            read_asset(store_, baseRoot_, baseRoot_, *listing, index, gameRoot_));
                        const auto item = item_content(document);
                        if (!item) continue;
                        items.add(*item, sold(lower(item->key)));
                        ++read;
                    } catch (const std::exception&) {}
                }
            } catch (const std::exception& failure) {
                note("The game's " + bundle->name + " could not be read for " + std::string(store_copies_check) + " (" + failure.what() + ")");
            }
        }
        return read;
    }

private:
    using Bundles = std::map<std::string, const fb::TocBundle*, std::less<>>;
    using Assets = std::map<std::string, fb::Sha1, std::less<>>;

    const Bundles* game_toc(std::string_view relative) {
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

    void note(std::string text) {
        if (notes_) notes_->push_back(std::move(text));
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
    // A store item with a look is told by that; its shape is anyone's to use.
    if (!sold || item.looks.empty())
        for (const auto& shape : item.shapes) file(shapes_, shape);
    if (!item.data.empty()) file(data_, item.data);
}

std::string StoreItems::original(const ItemContent& item) const {
    for (const auto& look : item.looks)
        if (const auto* key = looks_.owner(look)) return *key;
    if (item.looks.empty())
        for (const auto& shape : item.shapes)
            if (const auto* key = shapes_.owner(shape)) return *key;
    if (!item.data.empty())
        if (const auto* key = data_.owner(item.data)) return *key;
    return {};
}

StoreCopies check_store_copies(const Catalog& catalog, const StoreItem& sold, std::vector<std::string>* notes) noexcept {
    StoreCopies result;
    try {
        if (!sold || std::none_of(catalog.mods.begin(), catalog.mods.end(), [](const Mod& mod) { return mod.provides_layout; }))
            return result;
        Scan scan(catalog, notes);
        std::vector<std::pair<const Mod*, std::vector<ItemContent>>> added;
        for (const auto& mod : catalog.mods) {
            if (!mod.provides_layout) continue;
            if (auto items = scan.read_mod(mod); !items.empty()) added.emplace_back(&mod, std::move(items));
        }
        if (added.empty()) return result;
        StoreItems game;
        result.game_items = scan.read_game(game, sold);
        if (!result.game_items) {
            if (notes) notes->push_back(std::string(store_copies_check) + " could not read the game's own files and did not run");
            return result;
        }
        for (const auto& [mod, items] : added) {
            StoreCopies::Source source{mod->name};
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
