#include "ebx_merge.h"

#include "ebx_carry.h"
#include "ebx_writer.h"

#include <cstdint>
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dingosdk::frostbite::ebx {
namespace {

// An exported instance carries its own identity in the low bits of `Flags`:
// every sublevel the game ships has `Flags == guid[0..3] & 0x01ffffff`. An
// authoring tool that clones a donor instance gives the copy a fresh guid but
// leaves the donor's id behind, which nothing notices until a second mod clones
// the same donor and the two copies collide. Re-deriving the id from the guid
// restores the invariant and is a no-op for an instance that already holds it.
constexpr std::uint32_t objectIdMask = 0x01ffffffU;

std::uint32_t object_id(const Guid& guid) {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[0]))) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[3])) << 24U);
}

bool renumber(Object& object, const Guid& guid, const Document& source) {
    for (auto& field : object.fields) {
        if (field.name != "Flags") continue;
        // The reader keeps a signed field as int64 and an unsigned one as
        // uint64; `Flags` is unsigned in this schema but not in every schema.
        const auto* signedValue = std::get_if<std::int64_t>(&field.value.data);
        const auto* unsignedValue = std::get_if<std::uint64_t>(&field.value.data);
        if (!signedValue && !unsignedValue) return false;
        const auto current = signedValue ? static_cast<std::uint32_t>(*signedValue)
                                         : static_cast<std::uint32_t>(*unsignedValue);
        const auto renumbered = (current & ~objectIdMask) | (object_id(guid) & objectIdMask);
        if (renumbered == current) return false;
        // On most types `Flags` is a genuine flag word. It is only an id to
        // re-derive when it holds the id of some exported instance, which is
        // what a clone inherits from its donor.
        bool cloned{};
        for (const auto& instance : source.instances)
            if (instance.exported &&
                (object_id(instance.instanceGuid) & objectIdMask) == (current & objectIdMask)) {
                cloned = true;
                break;
            }
        if (!cloned) return false;
        if (signedValue) field.value.data = static_cast<std::int64_t>(renumbered);
        else field.value.data = static_cast<std::uint64_t>(renumbered);
        return true;
    }
    return false;
}

std::string type_name(const Document& document, std::int32_t descriptor) {
    if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document.types.size())
        throw std::runtime_error("EBX type descriptor is out of range");
    return document.types[static_cast<std::size_t>(descriptor)].name;
}

Value::Array* root_array(Document& document, std::string_view name) {
    auto* root = const_cast<InstanceRecord*>(document.root());
    if (!root || !root->object) return nullptr;
    for (auto& field : root->object->fields)
        if (field.name == name) return std::get_if<Value::Array>(&field.value.data);
    return nullptr;
}

const Value::Array* root_array(const Document& document, std::string_view name) {
    const auto* root = document.root();
    if (!root || !root->object) return nullptr;
    if (const auto* field = root->object->find(name)) return std::get_if<Value::Array>(&field->value.data);
    return nullptr;
}

// A type's name, which is what two documents that number their types
// independently have in common. Empty for a descriptor that names none.
std::string type_or_none(const Document& document, std::int32_t descriptor) {
    return descriptor >= 0 && static_cast<std::size_t>(descriptor) < document.types.size()
        ? document.types[static_cast<std::size_t>(descriptor)].name : std::string{};
}

constexpr auto unmatched = static_cast<std::size_t>(-1);

// Which of an edit's instances each of the base's is: the root is the root, an
// exported one is found by its guid, and an internal one, which has nothing
// else to go by, by its place among the internal ones. An edit that added or
// dropped an internal instance has moved the rest along, so then none of them
// is matched. Two that are not of one type are not a match either.
std::vector<std::size_t> match_instances(const Document& base, const Document& edit) {
    std::vector<std::size_t> match(base.instances.size(), unmatched);
    if (base.instances.empty() || edit.instances.empty()) return match;
    match[0] = 0;
    std::map<Guid, std::size_t> exported;
    std::vector<std::size_t> internal, baseInternal;
    for (std::size_t index = 1; index < edit.instances.size(); ++index) {
        if (edit.instances[index].exported) exported.emplace(edit.instances[index].instanceGuid, index);
        else internal.push_back(index);
    }
    for (std::size_t index = 1; index < base.instances.size(); ++index) {
        if (!base.instances[index].exported) { baseInternal.push_back(index); continue; }
        if (const auto found = exported.find(base.instances[index].instanceGuid); found != exported.end())
            match[index] = found->second;
    }
    if (internal.size() == baseInternal.size())
        for (std::size_t at = 0; at < internal.size(); ++at) match[baseInternal[at]] = internal[at];
    for (std::size_t index = 0; index < match.size(); ++index)
        if (match[index] != unmatched && type_or_none(base, base.instances[index].descriptor) !=
                                             type_or_none(edit, edit.instances[match[index]].descriptor))
            match[index] = unmatched;
    return match;
}

bool same(double left, double right) { return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right); }
bool same(const ResourceReference& left, const ResourceReference& right) { return left.id == right.id; }
template <typename Type> bool same(const Type& left, const Type& right) { return left == right; }

// Finds what one edit changed in place. The base and the edit are walked side
// by side, along with the result: it began as a copy of the base, so it has the
// same shape, and may already hold what an earlier edit changed. Nothing is
// written until the whole edit has been seen, so an edit either has all its
// values taken or none of them.
class InPlace final {
public:
    InPlace(const Document& base, const Document& edit, const std::vector<std::size_t>& match)
        : base_(base), edit_(edit), match_(match) {}

    void fields(const Object& base, const Object& edit, Object& result, bool root) {
        if (base.fields.size() != edit.fields.size() || base.fields.size() != result.fields.size()) { uncarried = true; return; }
        for (std::size_t index = 0; index < base.fields.size(); ++index) {
            if (base.fields[index].name != edit.fields[index].name) { uncarried = true; return; }
            value(base.fields[index].value, edit.fields[index].value, result.fields[index].value, root);
        }
    }

    void apply() {
        for (const auto& [to, from] : changes_) to->data = from->data;
    }

    std::size_t values{};     // values the edit changed from the base
    std::size_t contested{};  // of those, ones the result already had changed to something else
    bool uncarried{};         // a change was seen that cannot be taken
    bool unread{};            // somewhere a change could not be looked for

private:
    template <typename Type> bool plain(const Value& base, const Value& edit, Value& result) {
        const auto* before = std::get_if<Type>(&base.data);
        if (!before) return false;
        const auto& after = std::get<Type>(edit.data);
        if (same(*before, after)) return true;
        ++values;
        const auto& current = std::get<Type>(result.data);
        if (same(current, after)) return true;
        if (!same(current, *before)) ++contested;
        changes_.emplace_back(&result, &edit);
        return true;
    }

    void value(const Value& base, const Value& edit, Value& result, bool rootList) {
        if (base.data.index() != edit.data.index() || base.data.index() != result.data.index()) { uncarried = true; return; }
        if (std::holds_alternative<std::monostate>(base.data)) return;
        if (plain<bool>(base, edit, result) || plain<std::int64_t>(base, edit, result) ||
            plain<std::uint64_t>(base, edit, result) || plain<double>(base, edit, result) ||
            plain<std::string>(base, edit, result) || plain<Guid>(base, edit, result) ||
            plain<Sha1>(base, edit, result) || plain<ResourceReference>(base, edit, result)) return;
        if (const auto* before = std::get_if<PointerReference>(&base.data)) {
            reference(*before, std::get<PointerReference>(edit.data));
            return;
        }
        if (const auto* before = std::get_if<std::shared_ptr<Object>>(&base.data)) {
            const auto& after = std::get<std::shared_ptr<Object>>(edit.data);
            const auto& mine = std::get<std::shared_ptr<Object>>(result.data);
            if (!*before && !after) return;
            if (!*before || !after || !mine) { uncarried = true; return; }
            fields(**before, *after, *mine, false);
            return;
        }
        if (const auto* before = std::get_if<Value::Array>(&base.data)) {
            const auto& after = std::get<Value::Array>(edit.data);
            auto& mine = std::get<Value::Array>(result.data);
            // Only a list on the root grows at its end, and what was appended
            // comes across on its own. A list that shrank, or one deeper in
            // that changed length, has entries no longer told apart by place.
            if (rootList ? after.size() < before->size() : after.size() != before->size()) { uncarried = true; return; }
            if (mine.size() < before->size()) { uncarried = true; return; }
            for (std::size_t index = 0; index < before->size(); ++index)
                value((*before)[index], after[index], mine[index], false);
            return;
        }
        if (const auto* before = std::get_if<TypeReference>(&base.data)) {
            const auto& after = std::get<TypeReference>(edit.data);
            if (before->primitive != after.primitive) uncarried = true;
            else if (before->primitive) uncarried |= before->primitiveType != after.primitiveType;
            else uncarried |= type_or_none(base_, before->descriptor) != type_or_none(edit_, after.descriptor);
            return;
        }
        if (const auto* before = std::get_if<BoxedReference>(&base.data)) {
            const auto& after = std::get<BoxedReference>(edit.data);
            // What a boxed value holds is not read, so a change inside one is not seen.
            if ((before->encodedType == 0) != (after.encodedType == 0)) uncarried = true;
            else if (before->encodedType != 0) unread = true;
        }
    }

    // A reference still has to lead to the same place; one pointed elsewhere
    // cannot be taken, because what it points at may not be in the result.
    void reference(const PointerReference& before, const PointerReference& after) {
        if (before.kind != after.kind) { uncarried = true; return; }
        if (before.kind == PointerKind::null) return;
        if (before.index < 0 || after.index < 0) { uncarried |= before.index != after.index; return; }
        const auto from = static_cast<std::size_t>(before.index), to = static_cast<std::size_t>(after.index);
        if (before.kind == PointerKind::external) {
            uncarried |= from >= base_.imports.size() || to >= edit_.imports.size() ||
                         base_.imports[from].fileGuid != edit_.imports[to].fileGuid ||
                         base_.imports[from].classGuid != edit_.imports[to].classGuid;
            return;
        }
        if (from >= match_.size()) { uncarried = true; return; }
        // An internal instance that could not be matched is not known to have moved.
        if (match_[from] == unmatched) (base_.instances[from].exported ? uncarried : unread) = true;
        else uncarried |= match_[from] != to;
    }

    const Document& base_;
    const Document& edit_;
    const std::vector<std::size_t>& match_;
    std::vector<std::pair<Value*, const Value*>> changes_;
};

template <typename Visit> void each_pointer(Value& value, const Visit& visit) {
    if (auto* pointer = std::get_if<PointerReference>(&value.data)) { visit(*pointer); return; }
    if (auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
        if (*nested) for (auto& field : (*nested)->fields) each_pointer(field.value, visit);
        return;
    }
    if (auto* list = std::get_if<Value::Array>(&value.data))
        for (auto& element : *list) each_pointer(element, visit);
}

} // namespace

namespace detail {

Carrier::Carrier(const Document& from, Document& to) : from_(from), to_(to) {
    for (std::size_t index = 0; index < to.types.size(); ++index)
        typeByName_.emplace(to.types[index].name, static_cast<std::int32_t>(index));
    for (std::size_t index = 0; index < to.instances.size(); ++index)
        instanceByGuid_.emplace(to.instances[index].instanceGuid, index);
}

void Carrier::alias(std::size_t from, std::size_t to) { aliases_[from] = to; }

std::int32_t Carrier::type(std::int32_t descriptor) const {
    const auto name = type_name(from_, descriptor);
    const auto found = typeByName_.find(name);
    if (found == typeByName_.end())
        throw std::runtime_error("EBX merge needs a type the base does not define: " + name);
    return found->second;
}

std::size_t Carrier::instance(std::size_t index) {
    if (index >= from_.instances.size()) throw std::runtime_error("EBX pointer is out of range");
    if (const auto aliased = aliases_.find(index); aliased != aliases_.end()) return aliased->second;
    const auto& source = from_.instances[index];
    if (source.exported) {
        if (const auto found = instanceByGuid_.find(source.instanceGuid); found != instanceByGuid_.end())
            return found->second;
    } else if (const auto found = carriedInternal_.find(index); found != carriedInternal_.end()) {
        return found->second;
    }
    if (!source.object) throw std::runtime_error("EBX instance has no parsed object");
    // Reserve the slot before copying, so a cycle back to this instance
    // resolves instead of recursing for ever.
    const auto descriptor = type(source.descriptor);
    InstanceRecord carried;
    carried.fixupType = ensure_fixup_type(to_, descriptor);
    carried.descriptor = descriptor;
    carried.exported = source.exported;
    carried.instanceGuid = source.instanceGuid;
    carried.object = std::make_shared<Object>();
    carried.object->descriptor = descriptor;
    to_.instances.push_back(std::move(carried));
    const auto at = to_.instances.size() - 1;
    // Internal instances carry no guid, so only exported ones are keyed.
    if (source.exported) instanceByGuid_.emplace(source.instanceGuid, at);
    else carriedInternal_.emplace(index, at);
    // The writer lays an instance down over its original image and only
    // then writes the parsed fields, so padding and anything the field walk
    // does not cover has to travel with it.
    to_.instances[at].rawImage = source.rawImage;
    // Each pointer followed recurses: a mod's long pointer chain must fail, not overflow the stack.
    if (depth_ >= 512) throw std::runtime_error("EBX pointers chain too deeply to merge");
    ++depth_;
    struct Leave { unsigned& depth; ~Leave() { --depth; } } leave{depth_};
    auto copied = object(*source.object);
    to_.instances[at].object->fields = std::move(copied->fields);
    if (source.exported) renumbered += renumber(*to_.instances[at].object, source.instanceGuid, from_);
    ++instances;
    return at;
}

std::int32_t Carrier::import(std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= from_.imports.size())
        throw std::runtime_error("EBX import pointer is out of range");
    const auto& reference = from_.imports[static_cast<std::size_t>(index)];
    for (std::size_t at = 0; at < to_.imports.size(); ++at)
        if (to_.imports[at].fileGuid == reference.fileGuid &&
            to_.imports[at].classGuid == reference.classGuid)
            return static_cast<std::int32_t>(at);
    to_.imports.push_back(reference);
    return static_cast<std::int32_t>(to_.imports.size() - 1);
}

std::shared_ptr<Object> Carrier::object(const Object& source) {
    auto result = std::make_shared<Object>();
    result->descriptor = type(source.descriptor);
    result->fields.reserve(source.fields.size());
    for (const auto& field : source.fields)
        result->fields.push_back({field.descriptor, field.name, value(field.value)});
    return result;
}

Value Carrier::value(const Value& source) {
    Value result;
    if (const auto* pointer = std::get_if<PointerReference>(&source.data)) {
        auto copy = *pointer;
        if (pointer->kind == PointerKind::internal && pointer->index >= 0)
            copy.index = static_cast<std::int32_t>(instance(static_cast<std::size_t>(pointer->index)));
        else if (pointer->kind == PointerKind::external)
            copy.index = import(pointer->index);
        result.data = copy;
        return result;
    }
    if (const auto* nested = std::get_if<std::shared_ptr<Object>>(&source.data)) {
        result.data = *nested ? object(**nested) : nullptr;
        return result;
    }
    if (const auto* array = std::get_if<Value::Array>(&source.data)) {
        Value::Array copied;
        copied.reserve(array->size());
        for (const auto& element : *array) copied.push_back(value(element));
        result.data = std::move(copied);
        return result;
    }
    if (const auto* reference = std::get_if<TypeReference>(&source.data)) {
        auto copy = *reference;
        if (!reference->primitive && reference->descriptor >= 0) copy.descriptor = type(reference->descriptor);
        result.data = copy;
        return result;
    }
    if (std::holds_alternative<BoxedReference>(source.data))
        throw std::runtime_error("EBX merge cannot carry a boxed value yet");
    result = source;
    return result;
}

std::shared_ptr<Object> clone_object(const Object& source) {
    auto result = std::make_shared<Object>();
    result->descriptor = source.descriptor;
    result->fields.reserve(source.fields.size());
    for (const auto& field : source.fields)
        result->fields.push_back({field.descriptor, field.name, clone_value(field.value)});
    return result;
}

Value clone_value(const Value& source) {
    Value result;
    if (const auto* nested = std::get_if<std::shared_ptr<Object>>(&source.data)) {
        result.data = *nested ? clone_object(**nested) : nullptr;
        return result;
    }
    if (const auto* array = std::get_if<Value::Array>(&source.data)) {
        Value::Array copied;
        copied.reserve(array->size());
        for (const auto& element : *array) copied.push_back(clone_value(element));
        result.data = std::move(copied);
        return result;
    }
    result = source;
    return result;
}

Document clone_document(const Document& source) {
    Document result = source;
    for (auto& instance : result.instances)
        if (instance.object) instance.object = clone_object(*instance.object);
    return result;
}

// Every document the game ships keeps its exported instances, after the root,
// ascending by stored guid, and the engine binary-searches that table to resolve
// a reference into the partition. An instance appended at the end leaves the
// table unsorted: one appended guid is usually still found by luck, two are not,
// and the reference that goes missing comes back as a null object. So carried
// instances are put where they belong and every internal pointer is remapped.
void sort_instances(Document& document) {
    if (document.instances.size() < 3) return;
    // The root keeps slot 0 and is not part of the searched range. Exported
    // instances follow it in guid order; internal ones, which have no guid and
    // must come last, keep their relative order behind them.
    std::vector<std::size_t> exported;
    std::vector<std::size_t> internal;
    for (std::size_t index = 1; index < document.instances.size(); ++index)
        (document.instances[index].exported ? exported : internal).push_back(index);
    std::stable_sort(exported.begin(), exported.end(), [&](std::size_t left, std::size_t right) {
        return std::memcmp(document.instances[left].instanceGuid.bytes.data(),
                           document.instances[right].instanceGuid.bytes.data(), 16) < 0;
    });
    std::vector<std::size_t> order{0};
    order.insert(order.end(), exported.begin(), exported.end());
    order.insert(order.end(), internal.begin(), internal.end());
    bool moved{};
    for (std::size_t index = 0; index < order.size(); ++index) moved |= order[index] != index;
    if (!moved) return;

    std::vector<std::int32_t> moveTo(order.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        moveTo[order[index]] = static_cast<std::int32_t>(index);

    std::vector<InstanceRecord> sorted;
    sorted.reserve(order.size());
    for (const auto from : order) sorted.push_back(std::move(document.instances[from]));
    document.instances = std::move(sorted);

    const auto remap = [&](auto&& self, Value& value) -> void {
        if (auto* pointer = std::get_if<PointerReference>(&value.data)) {
            if (pointer->kind == PointerKind::internal && pointer->index >= 0 &&
                static_cast<std::size_t>(pointer->index) < moveTo.size())
                pointer->index = moveTo[static_cast<std::size_t>(pointer->index)];
            return;
        }
        if (auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
            if (*nested) for (auto& field : (*nested)->fields) self(self, field.value);
            return;
        }
        if (auto* list = std::get_if<Value::Array>(&value.data))
            for (auto& element : *list) self(self, element);
    };
    for (auto& instance : document.instances)
        if (instance.object)
            for (auto& field : instance.object->fields) remap(remap, field.value);
}


} // namespace detail

Document merge_documents(const Document& base, const std::span<const Document* const> edits,
                         MergeSummary* summary, const CarryImport& carry) {
    auto result = detail::clone_document(base);
    const auto* baseRoot = base.root();
    if (!baseRoot || !baseRoot->object) throw std::runtime_error("EBX base has no root object");

    // Snapshot how long each root array started out, so every edit is measured
    // against the base rather than against the edits applied before it.
    std::map<std::string, std::size_t, std::less<>> baseLengths;
    for (const auto& field : baseRoot->object->fields)
        if (const auto* array = std::get_if<Value::Array>(&field.value.data))
            baseLengths.emplace(field.name, array->size());

    MergeSummary totals;
    // The references the edits so far appended to each root array.
    std::map<std::string, std::set<std::pair<PointerKind, std::int32_t>>, std::less<>> appended;
    for (std::size_t index = 0; index < edits.size(); ++index) {
        const auto* edit = edits[index];
        if (!edit) continue;
        const auto* editRoot = edit->root();
        if (!editRoot || !editRoot->object) throw std::runtime_error("EBX edit has no root object");

        // What the edit changed in place. The result's first instances are still
        // the base's, in the base's order: carried ones are added behind them,
        // and nothing is sorted until every edit is in.
        const auto match = match_instances(base, *edit);
        InPlace changes(base, *edit, match);
        for (std::size_t at = 0; at < base.instances.size(); ++at) {
            const auto& before = base.instances[at];
            if (match[at] == unmatched) {
                // An exported instance is found whenever the edit still has it.
                (before.exported ? changes.uncarried : changes.unread) = true;
                continue;
            }
            const auto& after = edit->instances[match[at]];
            if (!before.object || !after.object || !result.instances[at].object) {
                changes.unread |= before.object || after.object;
                continue;
            }
            changes.fields(*before.object, *after.object, *result.instances[at].object, at == 0);
        }
        if (changes.uncarried) {
            totals.uncarried.push_back(index);
        } else {
            changes.apply();
            totals.values += changes.values;
            totals.contested += changes.contested;
        }
        if (changes.unread) totals.unread.push_back(index);

        detail::Carrier mapper(*edit, result);
        std::map<std::string, std::set<std::pair<PointerKind, std::int32_t>>, std::less<>> mine;

        // Whatever an edit appended to one of the root's arrays comes across,
        // pulling in the instances those entries point at.
        for (const auto& field : editRoot->object->fields) {
            const auto* editArray = std::get_if<Value::Array>(&field.value.data);
            if (!editArray) continue;
            const auto known = baseLengths.find(field.name);
            const auto had = known == baseLengths.end() ? 0 : known->second;
            if (editArray->size() <= had) continue;
            auto* target = root_array(result, field.name);
            if (!target)
                throw std::runtime_error("EBX merge cannot find the base array " + field.name);
            for (auto at = had; at < editArray->size(); ++at) {
                const auto& entry = (*editArray)[at];
                const auto* pointer = carry ? std::get_if<PointerReference>(&entry.data) : nullptr;
                if (pointer && pointer->kind == PointerKind::external && pointer->index >= 0 &&
                    static_cast<std::size_t>(pointer->index) < edit->imports.size()) {
                    const auto& reference = edit->imports[static_cast<std::size_t>(pointer->index)];
                    if (!carry(reference)) {
                        totals.dropped.push_back({index, reference});
                        continue;
                    }
                }
                auto carried = mapper.value(entry);
                // Two edits that append the same thing mean it once. What one edit
                // lists twice on its own is left as its author wrote it.
                if (const auto* reference = std::get_if<PointerReference>(&carried.data);
                    reference && reference->kind != PointerKind::null) {
                    const std::pair key{reference->kind, reference->index};
                    if (const auto earlier = appended.find(field.name);
                        earlier != appended.end() && earlier->second.contains(key)) {
                        ++totals.repeated;
                        continue;
                    }
                    mine[field.name].insert(key);
                }
                target->push_back(std::move(carried));
                ++totals.arrayEntries;
            }
        }
        for (auto& [name, references] : mine) appended[name].merge(references);

        // An instance an edit added but never linked from the root still has to
        // exist, or the asset it belongs to goes missing.
        for (const auto& instance : edit->instances) {
            // Internal instances are only carried when something the edit added
            // points at them, which the array pass above has already done.
            if (!instance.exported) continue;
            bool known{};
            for (const auto& existing : result.instances)
                if (existing.exported && existing.instanceGuid == instance.instanceGuid) { known = true; break; }
            if (known) continue;
            static_cast<void>(mapper.instance(
                static_cast<std::size_t>(&instance - edit->instances.data())));
        }
        totals.instances += mapper.instances;
        totals.renumbered += mapper.renumbered;
    }
    detail::sort_instances(result);
    if (summary) *summary = totals;
    return result;
}

std::size_t drop_root_references(Document& document, const CarryImport& keep) {
    auto* root = const_cast<InstanceRecord*>(document.root());
    if (!keep || !root || !root->object) return 0;
    std::size_t dropped{};
    for (auto& field : root->object->fields) {
        auto* array = std::get_if<Value::Array>(&field.value.data);
        if (!array) continue;
        dropped += std::erase_if(*array, [&](const Value& entry) {
            const auto* pointer = std::get_if<PointerReference>(&entry.data);
            return pointer && pointer->kind == PointerKind::external && pointer->index >= 0 &&
                   static_cast<std::size_t>(pointer->index) < document.imports.size() &&
                   !keep(document.imports[static_cast<std::size_t>(pointer->index)]);
        });
    }
    // A boxed value is kept as the bytes it was read as, import numbers and
    // all, so a document with one keeps its imports where they are.
    if (!dropped || !document.boxedValues.empty()) return dropped;

    std::vector<bool> used(document.imports.size());
    for (auto& instance : document.instances)
        if (instance.object)
            for (auto& field : instance.object->fields)
                each_pointer(field.value, [&](const PointerReference& pointer) {
                    if (pointer.kind == PointerKind::external && pointer.index >= 0 &&
                        static_cast<std::size_t>(pointer.index) < used.size())
                        used[static_cast<std::size_t>(pointer.index)] = true;
                });
    std::vector<std::int32_t> moveTo(used.size(), -1);
    std::vector<ImportReference> kept;
    for (std::size_t index = 0; index < used.size(); ++index) {
        if (!used[index]) continue;
        moveTo[index] = static_cast<std::int32_t>(kept.size());
        kept.push_back(document.imports[index]);
    }
    if (kept.size() == document.imports.size()) return dropped;
    for (auto& instance : document.instances)
        if (instance.object)
            for (auto& field : instance.object->fields)
                each_pointer(field.value, [&](PointerReference& pointer) {
                    if (pointer.kind == PointerKind::external && pointer.index >= 0 &&
                        static_cast<std::size_t>(pointer.index) < moveTo.size())
                        pointer.index = moveTo[static_cast<std::size_t>(pointer.index)];
                });
    document.imports = std::move(kept);
    return dropped;
}

} // namespace dingosdk::frostbite::ebx
