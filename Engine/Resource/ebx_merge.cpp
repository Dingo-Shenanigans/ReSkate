#include "ebx_merge.h"

#include "ebx_carry.h"
#include "ebx_writer.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

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
                         MergeSummary* summary) {
    auto result = detail::clone_document(base);
    const auto* baseRoot = base.root();
    if (!baseRoot || !baseRoot->object) throw std::runtime_error("EBX base has no root object");

    // A lone edit has no other edit to disagree with, so its in-place changes
    // are as safe to honour as its additions: the root is replaced with the
    // edit's root instead of only receiving appended entries. Anything the
    // root points at is carried like an appended entry would carry it. With
    // two or more edits this stays off, because two edits to one value cannot
    // both be honoured.
    if (edits.size() == 1 && edits[0]) {
        const auto* edit = edits[0];
        const auto* editRoot = edit->root();
        if (!editRoot || !editRoot->object) throw std::runtime_error("EBX edit has no root object");
        detail::Carrier mapper(*edit, result);
        MergeSummary totals;
        auto& root = *const_cast<InstanceRecord*>(result.root());
        root.object = mapper.object(*editRoot->object);
        ++totals.rootReplacements;

        // An instance an edit added but never linked from the root still has to
        // exist, or the asset it belongs to goes missing.
        for (const auto& instance : edit->instances) {
            // Internal instances are only carried when something the edit added
            // points at them, which the root replacement above has already done.
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
        detail::sort_instances(result);
        if (summary) *summary = totals;
        return result;
    }

    // Snapshot how long each root array started out, so every edit is measured
    // against the base rather than against the edits applied before it.
    std::map<std::string, std::size_t, std::less<>> baseLengths;
    for (const auto& field : baseRoot->object->fields)
        if (const auto* array = std::get_if<Value::Array>(&field.value.data))
            baseLengths.emplace(field.name, array->size());

    MergeSummary totals;
    for (const auto* edit : edits) {
        if (!edit) continue;
        const auto* editRoot = edit->root();
        if (!editRoot || !editRoot->object) throw std::runtime_error("EBX edit has no root object");
        detail::Carrier mapper(*edit, result);

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
                target->push_back(mapper.value((*editArray)[at]));
                ++totals.arrayEntries;
            }
        }

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

} // namespace dingosdk::frostbite::ebx
