#include "ebx_writer.h"


#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace dingosdk::frostbite::ebx {

namespace {

std::size_t align_up(const std::size_t value, const std::size_t alignment) {
    if (alignment <= 1) return value;
    if (value > std::numeric_limits<std::size_t>::max() - alignment + 1) {
        throw std::overflow_error("EBX alignment overflow");
    }
    return (value + alignment - 1) / alignment * alignment;
}

template <typename Type>
Type number(const Value& value, const Type fallback = {}) {
    if (const auto* signedValue = std::get_if<std::int64_t>(&value.data)) {
        if constexpr (std::is_integral_v<Type> && !std::is_same_v<Type, bool>) {
            if constexpr (std::is_unsigned_v<Type>) {
                if (*signedValue < 0 || static_cast<std::uint64_t>(*signedValue) > std::numeric_limits<Type>::max())
                    throw std::overflow_error("EBX integer field exceeds its native width");
            } else if (*signedValue < std::numeric_limits<Type>::min() || *signedValue > std::numeric_limits<Type>::max()) {
                throw std::overflow_error("EBX integer field exceeds its native width");
            }
        }
        return static_cast<Type>(*signedValue);
    }
    if (const auto* unsignedValue = std::get_if<std::uint64_t>(&value.data)) {
        if constexpr (std::is_integral_v<Type> && !std::is_same_v<Type, bool>) {
            if (*unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<Type>::max()))
                throw std::overflow_error("EBX integer field exceeds its native width");
        }
        return static_cast<Type>(*unsignedValue);
    }
    if (const auto* floatValue = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*floatValue)) throw std::overflow_error("EBX floating-point field must be finite");
        if constexpr (std::is_floating_point_v<Type>) {
            if (*floatValue < std::numeric_limits<Type>::lowest() || *floatValue > std::numeric_limits<Type>::max())
                throw std::overflow_error("EBX floating-point field exceeds its native width");
        }
        return static_cast<Type>(*floatValue);
    }
    if (const auto* booleanValue = std::get_if<bool>(&value.data)) return static_cast<Type>(*booleanValue);
    return fallback;
}

std::size_t scalar_size(const FieldType type) {
    switch (type) {
    case FieldType::boolean:
    case FieldType::int8:
    case FieldType::uint8: return 1;
    case FieldType::int16:
    case FieldType::uint16: return 2;
    case FieldType::int32:
    case FieldType::uint32:
    case FieldType::float32:
    case FieldType::enumeration: return 4;
    case FieldType::int64:
    case FieldType::uint64:
    case FieldType::float64:
    case FieldType::resourceRef:
    case FieldType::pointer:
    case FieldType::cString:
    case FieldType::fileRef:
    case FieldType::typeRef:
    case FieldType::delegate: return 8;
    case FieldType::guid: return 16;
    case FieldType::sha1: return 20;
    case FieldType::string: return 32;
    case FieldType::boxedValueRef: return 16;
    default: return 8;
    }
}

class Builder final {
public:
    Builder(const Document& document, const bool refuseNewArrayHashes)
        : document_(document), refuse_(refuseNewArrayHashes) {}

    std::vector<std::byte> run() {
        validate();
        write_instances();
        write_arrays();
        write_boxed_values();
        const auto stringOffset = checked_u32(payload_.size(), "EBX string offset");
        write_strings();
        resolve_pointers();
        const auto payload = std::move(payload_);
        const auto fixup = write_fixup(stringOffset);
        const auto extra = write_extra();
        return write_riff(payload, fixup, extra);
    }

private:
    struct PendingArray {
        std::uint32_t slot{};
        const Value::Array* values{};
        std::size_t descriptor{};
        std::optional<std::uint32_t> hash;
        std::int32_t holder{-1};   // type of the object holding the field
        bool emitted{};
    };

    struct PendingString {
        std::uint32_t slot{};
        std::string value;
    };

    struct PendingPointer {
        std::uint32_t slot{};
        PointerReference value;
    };

    struct PendingBoxed {
        std::uint32_t slot{};
        BoxedReference value;
    };

    static std::uint32_t checked_u32(const std::size_t value, const char* label) {
        if (value > std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error(label);
        return static_cast<std::uint32_t>(value);
    }

    void validate() const {
        if (document_.reflectionData.empty()) throw std::runtime_error("EBX has no donor reflection table");
        if (document_.fixupTypeSignatures.size() > document_.fixupTypeGuids.size()) {
            throw std::runtime_error("EBX fixup type table is invalid");
        }
        bool internalSeen{};
        for (const auto& instance : document_.instances) {
            internalSeen |= !instance.exported;
            if (internalSeen && instance.exported) throw std::runtime_error("exported EBX instances must precede internal instances");
            if (instance.descriptor < 0 || static_cast<std::size_t>(instance.descriptor) >= document_.types.size()) {
                throw std::runtime_error("EBX instance has no valid type descriptor");
            }
            if (instance.fixupType >= document_.fixupTypeGuids.size()) {
                throw std::runtime_error("EBX instance has no valid fixup type");
            }
        }
    }

    void grow(const std::size_t end) {
        if (end > payload_.size()) payload_.resize(end);
    }

    template <typename Type>
    void put_number(const std::size_t offset, const Type value) {
        grow(offset + sizeof(value));
        std::memcpy(payload_.data() + offset, &value, sizeof(value));
    }

    void put(const std::size_t offset, const std::span<const std::byte> bytes) {
        grow(offset + bytes.size());
        std::ranges::copy(bytes, payload_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    const Value* value_for(const Object* object, const std::size_t descriptor) const {
        if (!object) return nullptr;
        const auto found = std::ranges::find_if(object->fields, [descriptor](const FieldValue& field) {
            return field.descriptor == descriptor;
        });
        return found == object->fields.end() ? nullptr : &found->value;
    }

    void write_instances() {
        std::size_t cursor{};
        instanceOffsets_.reserve(document_.instances.size());
        for (const auto& instance : document_.instances) {
            const auto& type = document_.types[static_cast<std::size_t>(instance.descriptor)];
            const auto start = align_up(cursor + (instance.exported ? 16 : 0), std::max<std::size_t>(type.alignment, 1));
            if (instance.exported) put(start - 16, instance.instanceGuid.bytes);
            grow(start + type.size);
            if (!instance.rawImage.empty()) {
                put(start, std::span(instance.rawImage).first(std::min<std::size_t>(instance.rawImage.size(), type.size)));
            }
            put_number(start, instance.fixupType);
            write_type(static_cast<std::size_t>(instance.descriptor), start, instance.object.get());
            instanceOffsets_.push_back(checked_u32(start, "EBX instance offset"));
            cursor = start + type.size;
        }
    }

    void write_type(const std::size_t typeIndex, const std::size_t start, const Object* object) {
        const auto& type = document_.types[typeIndex];
        grow(start + type.size);
        for (std::size_t ordinal = 0; ordinal < type.fieldCount; ++ordinal) {
            const auto descriptorIndex = static_cast<std::size_t>(type.fieldIndex) + ordinal;
            if (descriptorIndex >= document_.fields.size()) throw std::runtime_error("EBX field range is invalid");
            const auto& field = document_.fields[descriptorIndex];
            if (field.type() == FieldType::inherited) {
                if (field.classRef >= document_.types.size()) throw std::runtime_error("EBX base type is invalid");
                write_type(field.classRef, start, object);
                continue;
            }
            const auto position = start + field.dataOffset;
            const auto* value = value_for(object, descriptorIndex);
            if (field.category() == FieldCategory::array) {
                grow(position + 8);
                pointerOffsets_.push_back(checked_u32(position, "EBX array slot"));
                static const Value::Array empty;
                const auto* array = value ? std::get_if<Value::Array>(&value->data) : nullptr;
                arraysPending_.push_back({checked_u32(position, "EBX array slot"), array ? array : &empty,
                                          descriptorIndex, value ? value->arrayHash : std::nullopt,
                                          object ? object->descriptor : static_cast<std::int32_t>(typeIndex)});
            } else if (field.type() == FieldType::structure) {
                if (field.classRef >= document_.types.size()) throw std::runtime_error("EBX struct type is invalid");
                const auto nestedStart = align_up(position, std::max<std::size_t>(document_.types[field.classRef].alignment, 1));
                const auto* nested = value ? std::get_if<std::shared_ptr<Object>>(&value->data) : nullptr;
                write_type(field.classRef, nestedStart, nested && *nested ? nested->get() : nullptr);
            } else {
                write_scalar(field, position, value);
            }
        }
    }

    void write_scalar(const FieldDescriptor& field, const std::size_t position, const Value* value) {
        static const Value empty;
        const auto& source = value ? *value : empty;
        switch (field.type()) {
        case FieldType::boolean: put_number(position, static_cast<std::uint8_t>(number<bool>(source))); break;
        case FieldType::int8: put_number(position, number<std::int8_t>(source)); break;
        case FieldType::uint8: put_number(position, number<std::uint8_t>(source)); break;
        case FieldType::int16: put_number(position, number<std::int16_t>(source)); break;
        case FieldType::uint16: put_number(position, number<std::uint16_t>(source)); break;
        case FieldType::int32:
        case FieldType::enumeration: put_number(position, number<std::int32_t>(source)); break;
        case FieldType::uint32: put_number(position, number<std::uint32_t>(source)); break;
        case FieldType::int64: put_number(position, number<std::int64_t>(source)); break;
        case FieldType::uint64: put_number(position, number<std::uint64_t>(source)); break;
        case FieldType::float32: put_number(position, number<float>(source)); break;
        case FieldType::float64: put_number(position, number<double>(source)); break;
        case FieldType::guid: {
            const auto* guid = std::get_if<Guid>(&source.data);
            static const Guid zero;
            put(position, guid ? std::span(guid->bytes) : std::span(zero.bytes));
            break;
        }
        case FieldType::sha1: {
            const auto* sha = std::get_if<Sha1>(&source.data);
            static const Sha1 zero;
            put(position, sha ? std::span(sha->bytes) : std::span(zero.bytes));
            break;
        }
        case FieldType::string: {
            grow(position + 32);
            std::fill_n(payload_.begin() + static_cast<std::ptrdiff_t>(position), 32, std::byte{});
            const auto* string = std::get_if<std::string>(&source.data);
            if (string) {
                if (string->size() > 31) throw std::overflow_error("EBX fixed string exceeds 31 bytes");
                std::memcpy(payload_.data() + position, string->data(), string->size());
            }
            break;
        }
        case FieldType::cString:
        case FieldType::fileRef: {
            grow(position + 8);
            put_number(position, std::uint64_t{});
            pointerOffsets_.push_back(checked_u32(position, "EBX string slot"));
            const auto* string = std::get_if<std::string>(&source.data);
            if (string && string->size() > 1024 * 1024) throw std::overflow_error("EBX string exceeds the authoring limit");
            stringsPending_.push_back({checked_u32(position, "EBX string slot"), string ? *string : std::string{}});
            break;
        }
        case FieldType::resourceRef: {
            const auto* reference = std::get_if<ResourceReference>(&source.data);
            put_number(position, reference ? reference->id : std::uint64_t{});
            resourceOffsets_.push_back(checked_u32(position, "EBX resource slot"));
            break;
        }
        case FieldType::pointer: {
            grow(position + 8);
            put_number(position, std::uint64_t{});
            const auto* pointer = std::get_if<PointerReference>(&source.data);
            pointersPending_.push_back({checked_u32(position, "EBX pointer slot"), pointer ? *pointer : PointerReference{}});
            break;
        }
        case FieldType::typeRef:
        case FieldType::delegate: {
            grow(position + 8);
            const auto* reference = std::get_if<TypeReference>(&source.data);
            const auto encoded = reference ? reference->encoded : 0U;
            put_number(position, encoded);
            put_number(position + 4, std::uint32_t{});
            if (encoded != 0 && field.type() == FieldType::typeRef) {
                typeInfoOffsets_.push_back(checked_u32(position, "EBX type-info slot"));
            }
            break;
        }
        case FieldType::boxedValueRef: {
            grow(position + 16);
            const auto* reference = std::get_if<BoxedReference>(&source.data);
            if (!reference || reference->encodedType == 0) {
                std::fill_n(payload_.begin() + static_cast<std::ptrdiff_t>(position), 16, std::byte{});
                break;
            }
            put_number(position, reference->encodedType);
            put_number(position + 4, std::uint32_t{});
            typeInfoOffsets_.push_back(checked_u32(position, "EBX boxed type slot"));
            pointerOffsets_.push_back(checked_u32(position + 8, "EBX boxed pointer slot"));
            boxesPending_.push_back({checked_u32(position, "EBX boxed slot"), *reference});
            break;
        }
        default:
            grow(position + scalar_size(field.type()));
            break;
        }
    }

    std::size_t element_stride(const FieldDescriptor& field) const {
        if (field.type() == FieldType::structure) {
            if (field.classRef >= document_.types.size()) throw std::runtime_error("EBX array struct type is invalid");
            return std::max<std::size_t>(document_.types[field.classRef].size, 1);
        }
        return scalar_size(field.type());
    }

    std::size_t element_alignment(const FieldDescriptor& field) const {
        if (field.type() == FieldType::structure) {
            return std::max<std::size_t>(document_.types[field.classRef].alignment, 1);
        }
        return std::max<std::size_t>(std::min<std::size_t>(element_stride(field), 8), 1);
    }

    std::uint16_t fixup_slot_for_descriptor(const std::uint16_t descriptor) const {
        if (descriptor >= document_.types.size()) return 0xFFFF;
        const auto& type = document_.types[descriptor];
        const auto count = std::min(document_.fixupTypeGuids.size(), document_.fixupTypeSignatures.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (document_.fixupTypeGuids[index] == type.guid &&
                document_.fixupTypeSignatures[index] == type.signature) {
                return static_cast<std::uint16_t>(index);
            }
        }
        return 0xFFFF;
    }

    std::uint32_t array_hash(const FieldDescriptor& field) const {
        const auto rawType = static_cast<std::uint16_t>(field.encodedType << 1);
        const auto classRef = field.type() == FieldType::structure ? fixup_slot_for_descriptor(field.classRef) : 0xFFFF;
        const auto matching = std::ranges::find_if(document_.arrays, [&](const ArrayRecord& value) {
            return value.encodedType == rawType && value.classRef == classRef;
        });
        if (matching != document_.arrays.end()) return matching->hash;
        if (!document_.arrays.empty()) return document_.arrays.front().hash;
        return field.nameHash;
    }

    // A new array takes the hash of an array the document read for the same field of the same type;
    // failing that it borrows (array_hash), or is refused (see write_document).
    std::uint32_t new_array_hash(const PendingArray& pending, const FieldDescriptor& field) {
        if (!knownWalked_) {
            knownWalked_ = true;
            for (const auto& instance : document_.instances)
                if (instance.object) collect_hashes(*instance.object);
        }
        const auto found = knownHashes_.find({pending.holder, pending.descriptor});
        if (refuse_) {
            if (found == knownHashes_.end())
                throw std::runtime_error("EBX new array " + std::string(field.name) +
                                         " has no array of its field to take a hash from");
            if (document_.hashedReflectionNames &&
                std::ranges::none_of(document_.reflectionGroups,
                                     [&](const ReflectionGroup& group) { return group.hash == found->second; }))
                throw std::runtime_error("EBX array hash of " + std::string(field.name) +
                                         " is not in the reflection's group table");
        }
        return found != knownHashes_.end() ? found->second : array_hash(field);
    }

    void collect_hashes(const Object& object) {
        for (const auto& field : object.fields) {
            if (field.value.arrayHash)
                knownHashes_.emplace(std::pair{object.descriptor, field.descriptor}, *field.value.arrayHash);
            collect_value(field.value);
        }
    }

    void collect_value(const Value& value) {
        if (const auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
            if (*nested) collect_hashes(**nested);
        } else if (const auto* array = std::get_if<Value::Array>(&value.data)) {
            for (const auto& element : *array) collect_value(element);
        }
    }

    void write_arrays() {
        arraysOffset_ = checked_u32(align_up(payload_.size(), 16), "EBX array section");
        grow(static_cast<std::size_t>(arraysOffset_) + 32);
        emptyArrayTarget_ = arraysOffset_ + 16;
        for (std::size_t index = 0; index < arraysPending_.size(); ++index) {
            if (!arraysPending_[index].emitted) emit_array(index);
        }
        carry_boxed_arrays();
    }

    // What a boxed value points at is not in the document's values: its bytes are carried as they
    // were, so the arrays and strings its pointers reach are carried with them and the pointers
    // moved to the new places. Without this a rewrite dropped them (seen on the game's own
    // level roots: a boxed debug string and about 1 KB of arrays).

    enum class Target { array, string, instance, boxed, empty, unknown };

    std::pair<Target, std::uint32_t> old_target(const std::uint32_t slot) const {
        const auto& old = document_.payload;
        std::int32_t displacement{};
        std::memcpy(&displacement, old.data() + slot, sizeof(displacement));
        const auto target = static_cast<std::uint32_t>(static_cast<std::int64_t>(slot) + displacement);
        if (oldArrays_.contains(target)) return {Target::array, target};
        if (document_.stringsOffset <= target && target < old.size()) return {Target::string, target};
        if (const auto found = oldInstances_.find(target); found != oldInstances_.end())
            return {Target::instance, found->second};
        if (document_.boxedValuesOffset <= target && target < document_.stringsOffset) return {Target::boxed, target};
        if (target >= 4 && target <= old.size()) {
            std::uint32_t count{};
            std::memcpy(&count, old.data() + target - 4, sizeof(count));
            if (count == 0) return {Target::empty, target};
        }
        return {Target::unknown, target};
    }

    static std::vector<std::uint32_t> old_slots(const std::uint32_t start, const std::size_t length,
                                                std::vector<std::uint32_t> offsets) {
        std::ranges::sort(offsets);
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        std::vector<std::uint32_t> out;
        for (const auto value : offsets)
            if (value >= start && value - start < length) out.push_back(value);
        return out;
    }

    void carry_boxed_arrays() {
        if (document_.payload.empty()) return;
        for (const auto& record : document_.arrays) oldArrays_.emplace(record.offset, record);
        for (std::size_t index = 0; index < document_.instances.size(); ++index)
            oldInstances_.emplace(document_.instances[index].dataOffset, static_cast<std::uint32_t>(index));
        auto records = document_.boxedValues;
        std::ranges::sort(records, {}, &BoxedValueRecord::offset);
        for (const auto& source : records)
            for (const auto slot : old_slots(source.offset, source.rawBytes.size(), document_.pointerOffsets)) {
                const auto [kind, key] = old_target(slot);
                if (kind == Target::array) carry_array(oldArrays_.at(key));
            }
    }

    std::pair<std::size_t, std::size_t> array_element(const ArrayRecord& record) const {
        FieldDescriptor probe;
        probe.encodedType = static_cast<std::uint16_t>(record.encodedType >> 1);
        if (probe.type() == FieldType::structure) {
            const auto slot = record.classRef;
            if (slot < document_.fixupTypeGuids.size() && slot < document_.fixupTypeSignatures.size())
                for (const auto& type : document_.types)
                    if (type.guid == document_.fixupTypeGuids[slot] && type.signature == document_.fixupTypeSignatures[slot])
                        return {std::max<std::size_t>(type.size, 1), std::max<std::size_t>(type.alignment, 1)};
            throw std::runtime_error("EBX array record has an unknown struct type");
        }
        const auto stride = scalar_size(probe.type());
        return {stride, std::max<std::size_t>(std::min<std::size_t>(stride, 8), 1)};
    }

    void carry_array(const ArrayRecord& record) {
        if (carried_.contains(record.offset)) return;
        const auto [stride, alignment] = array_element(record);
        const auto length = stride * record.count;
        if (record.offset + length > document_.payload.size()) throw std::runtime_error("EBX carried array exceeds EBXD");
        const auto data = align_up(payload_.size() + 4, alignment);
        put_number(data - 4, record.count);
        put(data, std::span(document_.payload).subspan(record.offset, length));
        const auto placed = checked_u32(data, "EBX array data");
        carried_[record.offset] = placed;
        arrays_.push_back({placed, record.count, record.hash, record.encodedType, record.classRef});
        relocate(record.offset, length, data);
    }

    void relocate(const std::uint32_t oldStart, const std::size_t length, const std::size_t newStart) {
        const auto moved = [&](const std::uint32_t old) {
            return checked_u32(newStart + (old - oldStart), "EBX carried relocation");
        };
        for (const auto old : old_slots(oldStart, length, document_.resourceRefOffsets)) resourceOffsets_.push_back(moved(old));
        for (const auto old : old_slots(oldStart, length, document_.importOffsets)) importOffsets_.push_back(moved(old));
        for (const auto old : old_slots(oldStart, length, document_.typeInfoOffsets)) typeInfoOffsets_.push_back(moved(old));
        for (const auto old : old_slots(oldStart, length, document_.pointerOffsets)) {
            const auto slot = moved(old);
            pointerOffsets_.push_back(slot);
            if (document_.payload.empty()) continue;
            const auto [kind, key] = old_target(old);
            switch (kind) {
            case Target::array:
                carry_array(oldArrays_.at(key));
                put_number(slot, static_cast<std::uint32_t>(carried_.at(key) - slot));
                break;
            case Target::string: {
                const auto& source = document_.payload;
                auto end = static_cast<std::size_t>(key);
                while (end < source.size() && source[end] != std::byte{0}) ++end;
                stringsPending_.push_back({slot, std::string(reinterpret_cast<const char*>(source.data()) + key, end - key)});
                break;
            }
            case Target::instance:
                put_number(slot, static_cast<std::uint32_t>(instanceOffsets_.at(key) - slot));
                break;
            case Target::empty:
                put_number(slot, static_cast<std::uint32_t>(emptyArrayTarget_ - slot));
                break;
            case Target::boxed:
                boxedTargets_.emplace_back(slot, key);
                break;
            case Target::unknown:
                break;
            }
        }
    }

    void emit_array(const std::size_t index) {
        arraysPending_[index].emitted = true;
        const auto slot = arraysPending_[index].slot;
        const auto* values = arraysPending_[index].values;
        const auto descriptor = arraysPending_[index].descriptor;
        const auto& field = document_.fields[descriptor];
        if (values->empty()) {
            put_number(slot, static_cast<std::uint32_t>(emptyArrayTarget_ - slot));
            return;
        }
        const auto data = align_up(payload_.size() + 4, element_alignment(field));
        const auto stride = element_stride(field);
        put_number(data - 4, checked_u32(values->size(), "EBX array count"));
        grow(data + stride * values->size());
        put_number(slot, static_cast<std::uint32_t>(data - slot));
        const auto firstChild = arraysPending_.size();
        for (std::size_t element = 0; element < values->size(); ++element) {
            const auto elementStart = data + element * stride;
            const auto& value = (*values)[element];
            if (field.type() == FieldType::structure) {
                const auto* object = std::get_if<std::shared_ptr<Object>>(&value.data);
                write_type(field.classRef, elementStart, object && *object ? object->get() : nullptr);
            } else {
                write_scalar(field, elementStart, &value);
            }
        }
        // An array read from a record keeps that record's hash; a new one borrows (array_hash).
        auto hash = arraysPending_[index].hash;
        if (!hash) hash = new_array_hash(arraysPending_[index], field);
        arrays_.push_back({checked_u32(data, "EBX array data"), checked_u32(values->size(), "EBX array count"),
                           *hash, static_cast<std::uint16_t>(field.encodedType << 1),
                           field.type() == FieldType::structure
                               ? fixup_slot_for_descriptor(field.classRef)
                               : static_cast<std::uint16_t>(0xFFFF)});
        for (std::size_t child = firstChild; child < arraysPending_.size(); ++child) {
            if (!arraysPending_[child].emitted) emit_array(child);
        }
    }

    void write_boxed_values() {
        boxedOffset_ = checked_u32(align_up(payload_.size(), 16), "EBX boxed section");
        grow(boxedOffset_);
        std::unordered_map<std::uint32_t, std::uint32_t> moved;
        std::vector<BoxedValueRecord> records = document_.boxedValues;
        std::ranges::sort(records, {}, &BoxedValueRecord::offset);
        for (const auto& source : records) {
            if (source.offset < document_.boxedValuesOffset) throw std::runtime_error("EBX boxed record precedes its section");
            const auto offset = static_cast<std::size_t>(boxedOffset_) + source.offset - document_.boxedValuesOffset;
            put(offset, source.rawBytes);
            relocate(source.offset, source.rawBytes.size(), offset);
            moved[source.offset] = checked_u32(offset, "EBX boxed value");
            auto authored = source;
            authored.offset = checked_u32(offset, "EBX boxed value");
            boxed_.push_back(std::move(authored));
        }
        for (const auto& [slot, target] : boxedTargets_)
            put_number(slot, static_cast<std::uint32_t>(boxedOffset_ + (target - document_.boxedValuesOffset) - slot));
        for (const auto& pending : boxesPending_) {
            put_number(pending.slot, pending.value.encodedType);
            put_number(static_cast<std::size_t>(pending.slot) + 4, std::uint32_t{});
            const auto found = moved.find(static_cast<std::uint32_t>(pending.value.dataOffset));
            const auto relative = found == moved.end()
                ? pending.value.relativeOffset
                : static_cast<std::int64_t>(found->second) - static_cast<std::int64_t>(pending.slot + 8);
            put_number(static_cast<std::size_t>(pending.slot) + 8, relative);
        }
    }

    void write_strings() {
        std::unordered_map<std::string, std::uint32_t> offsets;
        for (const auto& pending : stringsPending_) {
            auto found = offsets.find(pending.value);
            if (found == offsets.end()) {
                const auto offset = checked_u32(payload_.size(), "EBX string table");
                found = offsets.emplace(pending.value, offset).first;
                const auto bytes = std::as_bytes(std::span(pending.value.data(), pending.value.size()));
                put(payload_.size(), bytes);
                grow(payload_.size() + 1);
            }
            put_number(pending.slot, static_cast<std::uint32_t>(found->second - pending.slot));
            put_number(static_cast<std::size_t>(pending.slot) + 4, std::uint32_t{});
        }
    }

    void resolve_pointers() {
        for (const auto& pending : pointersPending_) {
            switch (pending.value.kind) {
            case PointerKind::null: break;
            case PointerKind::internal:
                if (pending.value.index < 0 || static_cast<std::size_t>(pending.value.index) >= instanceOffsets_.size()) {
                    throw std::runtime_error("EBX internal pointer index is invalid");
                }
                put_number(pending.slot, static_cast<std::uint32_t>(instanceOffsets_[pending.value.index] - pending.slot));
                pointerOffsets_.push_back(pending.slot);
                break;
            case PointerKind::external:
                if (pending.value.index < 0 || static_cast<std::size_t>(pending.value.index) >= document_.imports.size()) {
                    throw std::runtime_error("EBX import pointer index is invalid");
                }
                put_number(pending.slot, (static_cast<std::uint32_t>(pending.value.index) << 1U) | 1U);
                importOffsets_.push_back(pending.slot);
                break;
            }
            put_number(static_cast<std::size_t>(pending.slot) + 4, std::uint32_t{});
        }
    }

    static void write_table(BinaryWriter& writer, std::vector<std::uint32_t> values) {
        std::ranges::sort(values);
        values.erase(std::unique(values.begin(), values.end()), values.end());
        writer.u32(checked_u32(values.size(), "EBX fixup table count"));
        for (const auto value : values) writer.u32(value);
    }

    std::vector<std::byte> write_fixup(const std::uint32_t stringOffset) const {
        BinaryWriter writer;
        writer.guid(document_.fileGuid);
        writer.u32(checked_u32(document_.fixupTypeGuids.size(), "EBX type GUID count"));
        for (const auto& value : document_.fixupTypeGuids) writer.guid(value);
        writer.u32(checked_u32(document_.fixupTypeSignatures.size(), "EBX signature count"));
        for (const auto value : document_.fixupTypeSignatures) writer.u32(value);
        writer.u32(checked_u32(std::ranges::count_if(document_.instances, &InstanceRecord::exported),
                               "EBX exported instance count"));
        write_table(writer, instanceOffsets_);
        write_table(writer, pointerOffsets_);
        write_table(writer, resourceOffsets_);
        writer.u32(checked_u32(document_.imports.size(), "EBX import count"));
        for (const auto& value : document_.imports) {
            writer.guid(value.fileGuid);
            writer.guid(value.classGuid);
        }
        write_table(writer, importOffsets_);
        write_table(writer, typeInfoOffsets_);
        writer.u32(arraysOffset_);
        writer.u32(boxedOffset_);
        writer.u32(stringOffset);

        auto oldSlots = document_.boxedValueSlots;
        std::vector<std::uint32_t> newSlots;
        newSlots.reserve(boxesPending_.size());
        for (const auto& pending : boxesPending_) newSlots.push_back(pending.slot);
        std::ranges::sort(oldSlots);
        std::ranges::sort(newSlots);
        std::vector<std::uint32_t> trailing;
        for (const auto old : document_.boxedReferenceOffsets) {
            const auto found = std::ranges::lower_bound(oldSlots, old);
            if (found == oldSlots.end() || *found != old) continue;
            const auto index = static_cast<std::size_t>(found - oldSlots.begin());
            if (index < newSlots.size()) trailing.push_back(newSlots[index]);
        }
        write_table(writer, std::move(trailing));
        return writer.take();
    }

    std::vector<std::byte> write_extra() const {
        BinaryWriter writer;
        writer.u32(checked_u32(arrays_.size(), "EBX array descriptor count"));
        writer.u32(checked_u32(boxed_.size(), "EBX boxed descriptor count"));
        auto arrays = arrays_;
        std::ranges::sort(arrays, {}, &ArrayRecord::offset);
        for (const auto& value : arrays) {
            writer.u32(value.offset);
            writer.u32(value.count);
            writer.u32(value.hash);
            writer.u16(value.encodedType);
            writer.u16(value.classRef);
        }
        auto boxed = boxed_;
        std::ranges::sort(boxed, {}, &BoxedValueRecord::offset);
        for (const auto& value : boxed) {
            writer.u32(value.offset);
            writer.u32(value.count);
            writer.u32(value.hash);
            writer.u16(value.encodedType);
            writer.u16(value.classRef);
        }
        return writer.take();
    }

    std::vector<std::byte> write_riff(const std::vector<std::byte>& payload,
                                      const std::vector<std::byte>& fixup,
                                      const std::vector<std::byte>& extra) const {
        BinaryWriter writer;
        writer.bytes(std::as_bytes(std::span("RIFF", 4)));
        writer.u32(0);
        // EBXS requires native-layout conversion when loaded by the game.
        writer.bytes(std::as_bytes(std::span(document_.serializedLayout ? "EBXS" : "EBX\0", 4)));
        auto chunk = [&](const std::string_view name, const std::span<const std::byte> bytes) {
            writer.bytes(std::as_bytes(std::span(name.data(), name.size())));
            writer.u32(checked_u32(bytes.size(), "EBX RIFF chunk size"));
            writer.bytes(bytes);
            if ((bytes.size() & 1U) != 0) writer.u8(0);
        };
        std::vector<std::byte> data(12);
        data.insert(data.end(), payload.begin(), payload.end());
        chunk("EBXD", data);
        chunk("EFIX", fixup);
        chunk("EBXX", extra);
        chunk(document_.hashedReflectionNames ? "RFL2" : "REFL", document_.reflectionData);
        auto result = writer.take();
        const auto size = checked_u32(result.size() - 8, "EBX RIFF size");
        std::memcpy(result.data() + 4, &size, sizeof(size));
        return result;
    }

    const Document& document_;
    std::vector<std::byte> payload_;
    std::vector<std::uint32_t> instanceOffsets_;
    std::vector<std::uint32_t> pointerOffsets_;
    std::vector<std::uint32_t> resourceOffsets_;
    std::vector<std::uint32_t> importOffsets_;
    std::vector<std::uint32_t> typeInfoOffsets_;
    std::vector<PendingArray> arraysPending_;
    std::vector<PendingString> stringsPending_;
    std::vector<PendingPointer> pointersPending_;
    std::vector<PendingBoxed> boxesPending_;
    std::vector<ArrayRecord> arrays_;
    std::vector<BoxedValueRecord> boxed_;
    std::uint32_t arraysOffset_{};
    std::uint32_t boxedOffset_{};
    std::uint32_t emptyArrayTarget_{};
    std::unordered_map<std::uint32_t, ArrayRecord> oldArrays_;
    std::unordered_map<std::uint32_t, std::uint32_t> oldInstances_;
    std::unordered_map<std::uint32_t, std::uint32_t> carried_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> boxedTargets_;
    bool refuse_{};
    bool knownWalked_{};
    std::map<std::pair<std::int32_t, std::size_t>, std::uint32_t> knownHashes_;   // (holder, field) -> hash
};

}

std::uint16_t ensure_fixup_type(Document& document, const std::int32_t descriptor) {
    if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document.types.size()) {
        throw std::out_of_range("EBX type descriptor is invalid");
    }
    const auto& type = document.types[static_cast<std::size_t>(descriptor)];
    const auto count = std::min(document.fixupTypeGuids.size(), document.fixupTypeSignatures.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (document.fixupTypeGuids[index] == type.guid &&
            document.fixupTypeSignatures[index] == type.signature) {
            if (index > std::numeric_limits<std::uint16_t>::max()) throw std::overflow_error("EBX type slot overflow");
            return static_cast<std::uint16_t>(index);
        }
    }
    if (count >= std::numeric_limits<std::uint16_t>::max()) throw std::overflow_error("EBX type slot overflow");
    document.fixupTypeGuids.insert(document.fixupTypeGuids.begin() + static_cast<std::ptrdiff_t>(count), type.guid);
    document.fixupTypeSignatures.push_back(type.signature);
    return static_cast<std::uint16_t>(count);
}

std::vector<std::byte> write_document(const Document& document, const bool refuseNewArrayHashes) {
    return Builder(document, refuseNewArrayHashes).run();
}

}
