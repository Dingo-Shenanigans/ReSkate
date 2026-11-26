#include "mesh_variation_merge.h"

#include "ebx_carry.h"

#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace dingosdk::frostbite::ebx {
namespace {

constexpr std::string_view database_type = "MeshVariationDatabase";
constexpr std::string_view database_leaf = "meshvariationdb_win32";

Value::Array* entries_of(Document& document) {
    auto* root = const_cast<InstanceRecord*>(document.root());
    if (!root || !root->object) return nullptr;
    for (auto& field : root->object->fields)
        if (field.name == "Entries") return std::get_if<Value::Array>(&field.value.data);
    return nullptr;
}
const Value::Array* entries_of(const Document& document) { return entries_of(const_cast<Document&>(document)); }

std::uint64_t number(const Object& entry, std::string_view name) {
    const auto* field = entry.find(name);
    if (!field) return 0;
    if (const auto* value = std::get_if<std::int64_t>(&field->value.data)) return static_cast<std::uint64_t>(*value);
    if (const auto* value = std::get_if<std::uint64_t>(&field->value.data)) return *value;
    return 0;
}

// What the game registers an entry by: its mesh, and its two variation hashes. A mesh is always
// another document's; an entry whose mesh is not (none seen) has no key and is always kept.
using Key = std::tuple<Guid, Guid, std::uint64_t, std::uint64_t>;
std::optional<Key> key_of(const Document& document, const Value& entry) {
    const auto* object = std::get_if<std::shared_ptr<Object>>(&entry.data);
    if (!object || !*object) return std::nullopt;
    const auto* mesh = (*object)->find("Mesh");
    const auto* pointer = mesh ? std::get_if<PointerReference>(&mesh->value.data) : nullptr;
    if (!pointer || pointer->kind != PointerKind::external || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.imports.size()) return std::nullopt;
    const auto& import = document.imports[static_cast<std::size_t>(pointer->index)];
    return Key{import.fileGuid, import.classGuid, number(**object, "VariationAssetNameHash"),
               number(**object, "SubVariationNameHash")};
}

std::string type_name(const Document& document, std::int32_t descriptor) {
    if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document.types.size()) return {};
    return document.types[static_cast<std::size_t>(descriptor)].name;
}

// Whether two values, each in its own document, say the same: what an internal pointer points at
// is compared by content, as the two documents number their instances apart.
class Same final {
public:
    Same(const Document& left, const Document& right) : left_(left), right_(right) {}

    bool values(const Value& a, const Value& b, unsigned depth = 0) const {
        if (depth > 64 || a.data.index() != b.data.index()) return false;
        if (const auto* x = std::get_if<PointerReference>(&a.data)) {
            const auto& y = std::get<PointerReference>(b.data);
            if (x->kind != y.kind) return false;
            if (x->kind == PointerKind::null) return true;
            if (x->kind == PointerKind::external) {
                const auto* i = import(left_, *x);
                const auto* j = import(right_, y);
                return i && j && i->fileGuid == j->fileGuid && i->classGuid == j->classGuid;
            }
            const auto* i = instance(left_, *x);
            const auto* j = instance(right_, y);
            if (!i || !j || i->exported != j->exported || (i->exported && i->instanceGuid != j->instanceGuid)) return false;
            return i->object && j->object && objects(*i->object, *j->object, depth + 1);
        }
        if (const auto* x = std::get_if<std::shared_ptr<Object>>(&a.data)) {
            const auto& y = std::get<std::shared_ptr<Object>>(b.data);
            if (!*x || !y) return !*x && !y;
            return objects(**x, *y, depth + 1);
        }
        if (const auto* x = std::get_if<Value::Array>(&a.data)) {
            const auto& y = std::get<Value::Array>(b.data);
            if (x->size() != y.size()) return false;
            for (std::size_t index = 0; index < x->size(); ++index)
                if (!values((*x)[index], y[index], depth + 1)) return false;
            return true;
        }
        if (const auto* x = std::get_if<TypeReference>(&a.data)) {
            const auto& y = std::get<TypeReference>(b.data);
            if (x->primitive != y.primitive) return false;
            return x->primitive ? x->primitiveType == y.primitiveType
                                : type_name(left_, x->descriptor) == type_name(right_, y.descriptor);
        }
        if (const auto* x = std::get_if<ResourceReference>(&a.data)) return x->id == std::get<ResourceReference>(b.data).id;
        // A boxed value is not in an entry the game writes; one that is counts as different.
        if (std::holds_alternative<BoxedReference>(a.data)) return false;
        if (const auto* x = std::get_if<bool>(&a.data)) return *x == std::get<bool>(b.data);
        if (const auto* x = std::get_if<std::int64_t>(&a.data)) return *x == std::get<std::int64_t>(b.data);
        if (const auto* x = std::get_if<std::uint64_t>(&a.data)) return *x == std::get<std::uint64_t>(b.data);
        if (const auto* x = std::get_if<double>(&a.data)) return *x == std::get<double>(b.data);
        if (const auto* x = std::get_if<std::string>(&a.data)) return *x == std::get<std::string>(b.data);
        if (const auto* x = std::get_if<Guid>(&a.data)) return *x == std::get<Guid>(b.data);
        if (const auto* x = std::get_if<Sha1>(&a.data)) return *x == std::get<Sha1>(b.data);
        return std::holds_alternative<std::monostate>(a.data);
    }

private:
    bool objects(const Object& a, const Object& b, unsigned depth) const {
        if (type_name(left_, a.descriptor) != type_name(right_, b.descriptor) || a.fields.size() != b.fields.size()) return false;
        for (std::size_t index = 0; index < a.fields.size(); ++index)
            if (a.fields[index].name != b.fields[index].name || !values(a.fields[index].value, b.fields[index].value, depth))
                return false;
        return true;
    }
    static const ImportReference* import(const Document& document, const PointerReference& pointer) {
        if (pointer.index < 0 || static_cast<std::size_t>(pointer.index) >= document.imports.size()) return nullptr;
        return &document.imports[static_cast<std::size_t>(pointer.index)];
    }
    static const InstanceRecord* instance(const Document& document, const PointerReference& pointer) {
        if (pointer.index < 0 || static_cast<std::size_t>(pointer.index) >= document.instances.size()) return nullptr;
        return &document.instances[static_cast<std::size_t>(pointer.index)];
    }

    const Document& left_;
    const Document& right_;
};

} // namespace

bool variation_database_name(const std::string_view assetName) noexcept {
    if (assetName.size() < database_leaf.size() + 1) return false;
    const auto tail = assetName.substr(assetName.size() - database_leaf.size() - 1);
    if (tail.front() != '/') return false;
    for (std::size_t index = 0; index < database_leaf.size(); ++index) {
        auto character = tail[index + 1];
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (character != database_leaf[index]) return false;
    }
    return true;
}

bool variation_database(const Document& document) noexcept {
    return document.rootType == database_type && entries_of(document);
}

VariationMerge merge_variation_databases(const Document* game, const std::span<const Document* const> copies) {
    for (const auto* copy : copies)
        if (!copy || !variation_database(*copy)) throw std::runtime_error("a copy is not a mesh variation database");
    if (game && !variation_database(*game)) throw std::runtime_error("the game's copy is not a mesh variation database");
    if (!game && copies.empty()) throw std::runtime_error("no mesh variation database to combine");

    // Started from the game's copy, or with none from the highest mod's, which then keeps all of
    // its own entries.
    const auto none = copies.size();   // the owner of what the game's copy holds
    const auto start = game ? none : copies.size() - 1;
    VariationMerge result;
    result.document = detail::clone_document(game ? *game : *copies[start]);
    std::map<Key, std::size_t> at;          // where each key's entry is in the combined list
    std::vector<std::size_t> owner;         // by entry: the copy it came from (`none`: the game's)
    {
        const auto& entries = *entries_of(result.document);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (const auto key = key_of(result.document, entries[index])) at.try_emplace(*key, index);
            owner.push_back(start);
        }
    }

    // The game's own entries, to tell a copy that carries one unchanged from one that changes it.
    std::map<Key, std::size_t> gameAt;
    if (game) {
        const auto& entries = *entries_of(*game);
        for (std::size_t index = 0; index < entries.size(); ++index)
            if (const auto key = key_of(*game, entries[index])) gameAt.try_emplace(*key, index);
    }

    // Highest first: a lower copy's entry for a key a higher copy gave is the higher one's to keep.
    for (std::size_t copy = copies.size(); copy-- > 0;) {
        if (copy == start) continue;
        const auto& from = *copies[copy];
        // Carried whole or not at all: a copy that fails half way leaves nothing of it behind.
        auto document = detail::clone_document(result.document);
        auto keys = at;
        auto owners = owner;
        std::size_t added{}, changed{};
        std::vector<VariationMerge::Clash> clashes;
        try {
            detail::Carrier carrier(from, document);
            auto& entries = *entries_of(document);
            for (const auto& entry : *entries_of(from)) {
                const auto key = key_of(from, entry);
                const auto known = key ? keys.find(*key) : keys.end();
                if (known == keys.end()) {
                    if (key) keys.emplace(*key, entries.size());
                    entries.push_back(carrier.value(entry));
                    owners.push_back(copy);
                    ++added;
                    continue;
                }
                const auto index = known->second;
                if (Same(from, document).values(entry, entries[index])) continue;
                // The game's entry as the game has it changes nothing, whoever changed it above.
                if (const auto shipped = gameAt.find(*key);
                    shipped != gameAt.end() && Same(from, *game).values(entry, (*entries_of(*game))[shipped->second])) continue;
                if (owners[index] == none) {
                    entries[index] = carrier.value(entry);
                    owners[index] = copy;
                    ++changed;
                } else {
                    clashes.push_back({owners[index], copy, std::get<0>(*key).string()});
                }
            }
        } catch (const std::exception& failure) {
            result.left.push_back({copy, failure.what()});
            continue;
        }
        result.document = std::move(document);
        at = std::move(keys);
        owner = std::move(owners);
        result.added += added;
        result.changed += changed;
        result.clashes.insert(result.clashes.end(), clashes.begin(), clashes.end());
    }
    detail::sort_instances(result.document);
    result.entries = entries_of(result.document)->size();
    return result;
}

} // namespace dingosdk::frostbite::ebx
