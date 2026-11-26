#include "ebx_boxed.h"

#include "ebx_writer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dingosdk::frostbite::ebx {
namespace {

struct Unreadable {};

FieldType type_of(std::uint32_t word) { return static_cast<FieldType>((word >> 5) & 0x1FU); }
bool is_list(std::uint32_t word) { return ((word >> 1) & 0x0FU) == static_cast<std::uint32_t>(FieldCategory::array); }

std::size_t scalar_size(FieldType type) {
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
    case FieldType::guid: return 16;
    case FieldType::sha1: return 20;
    case FieldType::string: return 32;
    case FieldType::boxedValueRef: return 16;
    default: return 8;
    }
}

// The type a fix-up row names, among the types the document describes; -1 for none.
std::int32_t described(const Document& document, std::size_t row) {
    if (row >= document.fixupTypeGuids.size()) return -1;
    const auto& guid = document.fixupTypeGuids[row];
    const auto signed_ = row < document.fixupTypeSignatures.size();
    for (std::size_t index = 0; index < document.types.size(); ++index)
        if (document.types[index].guid == guid &&
            (!signed_ || document.types[index].signature == document.fixupTypeSignatures[row]))
            return static_cast<std::int32_t>(index);
    return -1;
}

// A type reference's word, as something two documents can compare: the primitive's type word,
// or the guid of the row it names (rows are numbered by each document on its own).
std::string type_identity(const Document& document, std::uint32_t word) {
    if (word == 0) return "none";
    if ((word & 0x80000000U) != 0) return "primitive " + std::to_string(word & 0x7FFFFFFFU);
    const auto row = static_cast<std::size_t>(word >> 2);
    if (row >= document.fixupTypeGuids.size()) throw Unreadable{};
    return document.fixupTypeGuids[row].string();
}

const BoxedValueRecord* record_at(const Document& document, std::int64_t offset) {
    const auto found = std::ranges::find_if(document.boxedValues, [&](const BoxedValueRecord& record) {
        return static_cast<std::int64_t>(record.offset) == offset;
    });
    return found == document.boxedValues.end() ? nullptr : &*found;
}

class Walk final {
public:
    explicit Walk(const Document& document) : document_(document), payload_(document.payload) {}

    void boxed(const BoxedReference& reference) {
        if (++depth_ > 32) throw Unreadable{};
        const auto* record = reference.encodedType == 0 ? nullptr : record_at(document_, reference.dataOffset);
        if (!record) throw Unreadable{};
        const auto word = static_cast<std::uint32_t>(record->encodedType);
        const auto type = type_of(word);
        std::int32_t descriptor = -1;
        if (type == FieldType::structure) {
            descriptor = described(document_, record->classRef);
            if (descriptor < 0) throw Unreadable{};
        }
        shape("box " + std::to_string(word) + " " + type_identity(document_, reference.encodedType));
        if (is_list(word)) {
            list(record->offset, type, descriptor);
        } else {
            element(type, descriptor, record->offset);
        }
        --depth_;
    }

    std::vector<BoxedLeaf> leaves;

private:
    template <typename Type> Type read(std::size_t offset) const {
        if (offset > payload_.size() || payload_.size() - offset < sizeof(Type)) throw Unreadable{};
        Type value{};
        std::memcpy(&value, payload_.data() + offset, sizeof(Type));
        return value;
    }

    void shape(std::string text) {
        BoxedLeaf leaf;
        leaf.kind = BoxedLeaf::Kind::shape;
        leaf.text = std::move(text);
        leaves.push_back(std::move(leaf));
    }

    static std::uint32_t at(std::size_t offset) {
        if (offset > 0xFFFFFFFFU) throw Unreadable{};
        return static_cast<std::uint32_t>(offset);
    }

    // A list's slot holds the distance to its first entry; the count is just before it.
    void list(std::size_t slot, FieldType type, std::int32_t descriptor) {
        const auto data = static_cast<std::int64_t>(slot) + read<std::int32_t>(slot);
        if (data < 4 || static_cast<std::uint64_t>(data) > payload_.size()) throw Unreadable{};
        const auto count = read<std::uint32_t>(static_cast<std::size_t>(data) - 4);
        const auto stride = type == FieldType::structure ? std::max<std::size_t>(document_.types[static_cast<std::size_t>(descriptor)].size, 1)
                                                         : scalar_size(type);
        if (count > (payload_.size() - static_cast<std::size_t>(data)) / stride) throw Unreadable{};
        shape("list " + std::to_string(count));
        for (std::uint32_t index = 0; index < count; ++index)
            element(type, descriptor, static_cast<std::size_t>(data) + index * stride);
    }

    void structure(std::int32_t descriptor, std::size_t start) {
        if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document_.types.size()) throw Unreadable{};
        if (++depth_ > 64) throw Unreadable{};
        shape("struct " + document_.types[static_cast<std::size_t>(descriptor)].name);
        fields(document_.types[static_cast<std::size_t>(descriptor)], start);
        --depth_;
    }

    void fields(const TypeDescriptor& type, std::size_t start) {
        for (std::size_t ordinal = 0; ordinal < type.fieldCount; ++ordinal) {
            const auto index = static_cast<std::size_t>(type.fieldIndex) + ordinal;
            if (index >= document_.fields.size()) throw Unreadable{};
            const auto& field = document_.fields[index];
            if (field.type() == FieldType::inherited) {
                if (field.classRef >= document_.types.size() || ++depth_ > 64) throw Unreadable{};
                fields(document_.types[field.classRef], start);
                --depth_;
                continue;
            }
            const auto position = start + field.dataOffset;
            if (field.category() == FieldCategory::array) list(position, field.type(), field.classRef);
            else element(field.type(), field.classRef, position);
        }
    }

    void element(FieldType type, std::int32_t descriptor, std::size_t offset) {
        BoxedLeaf leaf;
        leaf.offset = at(offset);
        switch (type) {
        case FieldType::structure: structure(descriptor, offset); return;
        case FieldType::cString:
        case FieldType::fileRef: {
            leaf.kind = BoxedLeaf::Kind::text;
            const auto displacement = read<std::uint32_t>(offset);
            if (displacement != 0xFFFFFFFFU) {
                auto end = offset + displacement;
                if (end >= payload_.size()) throw Unreadable{};
                const auto begin = end;
                while (end < payload_.size() && payload_[end] != std::byte{}) ++end;
                if (end == payload_.size()) throw Unreadable{};
                leaf.text.assign(reinterpret_cast<const char*>(payload_.data()) + begin, end - begin);
            }
            break;
        }
        case FieldType::pointer: {
            leaf.kind = BoxedLeaf::Kind::pointer;
            const auto encoded = read<std::int32_t>(offset);
            if (encoded == 0) break;
            if ((encoded & 1) != 0) {
                const auto index = encoded >> 1;
                if (index < 0 || static_cast<std::size_t>(index) >= document_.imports.size()) throw Unreadable{};
                leaf.pointer = {PointerKind::external, index};
                break;
            }
            const auto target = static_cast<std::int64_t>(offset) + encoded;
            const auto found = std::ranges::find_if(document_.instances, [&](const InstanceRecord& instance) {
                return static_cast<std::int64_t>(instance.dataOffset) == target;
            });
            if (found == document_.instances.end()) throw Unreadable{};
            leaf.pointer = {PointerKind::internal, static_cast<std::int32_t>(found - document_.instances.begin())};
            break;
        }
        case FieldType::typeRef:
        case FieldType::delegate:
            leaf.kind = BoxedLeaf::Kind::type;
            leaf.text = type_identity(document_, read<std::uint32_t>(offset));
            break;
        case FieldType::boxedValueRef: {
            const auto encoded = read<std::uint32_t>(offset);
            if (encoded == 0) { shape("box none"); return; }
            const auto target = static_cast<std::int64_t>(offset) + 8 + read<std::int64_t>(offset + 8);
            boxed({encoded, target, 0});
            return;
        }
        case FieldType::inherited:
        case FieldType::dbObject:
        case FieldType::array:
        case FieldType::fixedArray:
        case FieldType::function:
        case FieldType::interfaceType:
            throw Unreadable{};
        default: {
            leaf.kind = BoxedLeaf::Kind::bytes;
            const auto size = scalar_size(type);
            if (offset > payload_.size() || payload_.size() - offset < size) throw Unreadable{};
            leaf.bytes.assign(payload_.begin() + static_cast<std::ptrdiff_t>(offset),
                              payload_.begin() + static_cast<std::ptrdiff_t>(offset + size));
            break;
        }
        }
        leaves.push_back(std::move(leaf));
    }

    const Document& document_;
    std::span<const std::byte> payload_;
    unsigned depth_{};
};

} // namespace

std::optional<std::vector<BoxedLeaf>> read_boxed(const Document& document, const BoxedReference& reference) {
    try {
        Walk walk(document);
        walk.boxed(reference);
        return std::move(walk.leaves);
    } catch (const Unreadable&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

void write_boxed_bytes(Document& document, std::uint32_t offset, std::span<const std::byte> bytes) {
    if (static_cast<std::size_t>(offset) + bytes.size() > document.payload.size())
        throw std::runtime_error("EBX boxed value write is outside the payload");
    std::ranges::copy(bytes, document.payload.begin() + offset);
    for (auto& record : document.boxedValues) {
        if (offset < record.offset || offset - record.offset >= record.rawBytes.size()) continue;
        const auto within = static_cast<std::size_t>(offset - record.offset);
        const auto length = std::min(bytes.size(), record.rawBytes.size() - within);
        std::copy_n(bytes.begin(), length, record.rawBytes.begin() + static_cast<std::ptrdiff_t>(within));
    }
}

void write_boxed_text(Document& document, std::uint32_t slot, const std::string& text) {
    const auto at = document.payload.size();
    if (at > 0x7FFFFFFFU) throw std::runtime_error("EBX payload is too large for another string");
    const auto* characters = reinterpret_cast<const std::byte*>(text.data());
    document.payload.insert(document.payload.end(), characters, characters + text.size());
    document.payload.push_back(std::byte{});
    const auto displacement = static_cast<std::uint32_t>(at - slot);
    std::byte word[4];
    std::memcpy(word, &displacement, sizeof(word));
    write_boxed_bytes(document, slot, word);
}

BoxedReference carry_boxed(const Document& from, const BoxedReference& reference, Document& to,
                           const std::function<std::optional<std::uint16_t>(std::uint16_t)>& typeRow) {
    if (reference.encodedType == 0) return reference;
    const auto leaves = read_boxed(from, reference);
    if (!leaves) throw std::runtime_error("EBX merge cannot read a boxed value of an instance it carries");
    std::size_t boxes{};
    for (const auto& leaf : *leaves) {
        if (leaf.kind == BoxedLeaf::Kind::shape) {
            if (leaf.text.starts_with("box")) ++boxes;
            if (leaf.text.starts_with("list")) boxes = 2;
        }
        if (leaf.kind == BoxedLeaf::Kind::text || leaf.kind == BoxedLeaf::Kind::pointer ||
            leaf.kind == BoxedLeaf::Kind::type || boxes > 1)
            throw std::runtime_error("EBX merge cannot carry a boxed value that points at something");
    }
    const auto* record = record_at(from, reference.dataOffset);
    const auto word = static_cast<std::uint32_t>(record->encodedType);
    const auto type = type_of(word);
    std::size_t size = scalar_size(type), alignment = type == FieldType::guid ? 4 : std::min<std::size_t>(size, 8);
    if (type == FieldType::structure) {
        const auto& described_ = from.types[static_cast<std::size_t>(described(from, record->classRef))];
        size = std::max<std::size_t>(described_.size, 1);
        alignment = std::max<std::size_t>(described_.alignment, 1);
    }
    BoxedValueRecord carried;
    carried.count = record->count;
    carried.hash = record->hash;
    carried.encodedType = record->encodedType;
    carried.classRef = 0xFFFF;
    if (record->classRef != 0xFFFF) {
        const auto row = typeRow(record->classRef);
        if (!row) throw std::runtime_error("EBX merge cannot name the type of a boxed value it carries");
        carried.classRef = *row;
    }
    auto encoded = reference.encodedType;
    if ((encoded & 0x80000000U) == 0) {
        const auto row = typeRow(static_cast<std::uint16_t>(encoded >> 2));
        if (!row) throw std::runtime_error("EBX merge cannot name the type of a boxed value it carries");
        encoded = (static_cast<std::uint32_t>(*row) << 2) | 2U;
    }
    // The game matches a record to its field by the hash of its place, which this document's
    // reflection has to list (docs/ebx.md, "Groups and mappings").
    if (to.hashedReflectionNames &&
        std::ranges::none_of(to.reflectionGroups, [&](const ReflectionGroup& group) { return group.hash == record->hash; }))
        throw std::runtime_error("EBX merge cannot carry a boxed value to a place the document does not describe");

    // The value goes at the end of the boxed section, where the strings start: the strings move
    // along, and every slot that pointed at one is moved with them.
    const auto strings = static_cast<std::size_t>(to.stringsOffset);
    if (strings > to.payload.size()) throw std::runtime_error("EBX string section is outside the payload");
    const auto place = (strings + alignment - 1) / alignment * alignment;
    const auto grown = place - strings + size;
    std::vector<std::byte> bytes(from.payload.begin() + record->offset, from.payload.begin() + record->offset + static_cast<std::ptrdiff_t>(size));
    std::vector<std::byte> inserted(grown);
    std::ranges::copy(bytes, inserted.begin() + static_cast<std::ptrdiff_t>(place - strings));
    to.payload.insert(to.payload.begin() + static_cast<std::ptrdiff_t>(strings), inserted.begin(), inserted.end());
    for (const auto slot : to.pointerOffsets) {
        if (slot >= strings || static_cast<std::size_t>(slot) + 4 > to.payload.size()) continue;
        std::int32_t displacement{};
        std::memcpy(&displacement, to.payload.data() + slot, sizeof(displacement));
        if (static_cast<std::int64_t>(slot) + displacement < static_cast<std::int64_t>(strings)) continue;
        displacement += static_cast<std::int32_t>(grown);
        std::byte word4[4];
        std::memcpy(word4, &displacement, sizeof(word4));
        write_boxed_bytes(to, slot, word4);
    }
    to.stringsOffset = static_cast<std::uint32_t>(strings + grown);
    carried.offset = static_cast<std::uint32_t>(place);
    carried.rawBytes = std::move(bytes);
    to.boxedValues.push_back(std::move(carried));
    std::ranges::sort(to.boxedValues, {}, &BoxedValueRecord::offset);
    return {encoded, static_cast<std::int64_t>(place), 0};
}

} // namespace dingosdk::frostbite::ebx
