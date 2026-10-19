#include "ebx_document.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace dingosdk::frostbite::ebx {

namespace {

constexpr std::uint32_t riff = 0x46464952;
constexpr std::uint32_t ebx = 0x45425800;
constexpr std::uint32_t ebxs = 0x45425853;
constexpr std::uint32_t ebxd = 0x45425844;
constexpr std::uint32_t efix = 0x45464958;
constexpr std::uint32_t ebxx = 0x45425858;
constexpr std::uint32_t refl = 0x5245464C;
constexpr std::uint32_t rfl2 = 0x52464C32;

struct Chunk {
    std::uint32_t id{};
    std::size_t begin{};
    std::size_t end{};
};

struct ReflectionTypeId {
    Guid guid;
    std::uint32_t signature{};
};

std::size_t align_up(const std::size_t value, const std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

int nonnegative(BinaryReader& reader, const char* label) {
    const auto value = reader.i32();
    if (value < 0) throw std::runtime_error(std::string(label) + " count cannot be negative");
    return value;
}

void ensure_records(const BinaryReader& reader,
                    const std::size_t end,
                    const std::size_t count,
                    const std::size_t stride,
                    const char* label) {
    if (reader.position() > end || count > (end - reader.position()) / stride) {
        throw std::runtime_error(std::string(label) + " records exceed their chunk");
    }
}

std::vector<Chunk> read_chunks(BinaryReader& reader) {
    if (reader.u32() != riff) throw std::runtime_error("EBX is not a RIFF document");
    const auto riffEnd = static_cast<std::size_t>(reader.u32()) + 8;
    if (riffEnd != reader.size()) throw std::runtime_error("RIFF EBX size does not match its stream");
    const auto form = reader.u32(Endian::big);
    if (form != ebx && form != ebxs) throw std::runtime_error("RIFF document is not EBX or EBXS");
    std::vector<Chunk> chunks;
    while (reader.position() < riffEnd) {
        if (reader.remaining() < 8) throw std::runtime_error("truncated RIFF chunk header");
        Chunk chunk;
        chunk.id = reader.u32(Endian::big);
        const auto size = static_cast<std::size_t>(reader.u32());
        chunk.begin = reader.position();
        if (size > riffEnd - chunk.begin) throw std::runtime_error("RIFF chunk exceeds its document");
        chunk.end = chunk.begin + size;
        chunks.push_back(chunk);
        const auto next = chunk.end + (size & 1);
        if (next > riffEnd) throw std::runtime_error("RIFF chunk padding is truncated");
        reader.seek(next);
    }
    return chunks;
}

const Chunk& unique_chunk(const std::vector<Chunk>& chunks, const std::uint32_t id, const char* name) {
    const Chunk* result{};
    for (const auto& chunk : chunks) {
        if (chunk.id != id) continue;
        if (result) throw std::runtime_error(std::string("duplicate RIFF ") + name + " chunk");
        result = &chunk;
    }
    if (!result) throw std::runtime_error(std::string("missing RIFF ") + name + " chunk");
    return *result;
}

const Chunk* optional_chunk(const std::vector<Chunk>& chunks, const std::uint32_t id, const char* name) {
    const Chunk* result{};
    for (const auto& chunk : chunks) {
        if (chunk.id != id) continue;
        if (result) throw std::runtime_error(std::string("duplicate RIFF ") + name + " chunk");
        result = &chunk;
    }
    return result;
}

void validate_data_offset(const Document& document,
                          const std::uint32_t offset,
                          const std::size_t required,
                          const char* label) {
    const auto dataSize = document.dataEnd - document.dataStart;
    if (offset > dataSize || required > dataSize - offset) {
        throw std::runtime_error(std::string(label) + " offset exceeds EBXD");
    }
}

struct FixupTypes {
    std::vector<Guid> guids;
    std::vector<std::uint32_t> signatures;
};

FixupTypes read_fixup(BinaryReader& reader, const Chunk& chunk, Document& document) {
    reader.seek(chunk.begin);
    document.fileGuid = reader.guid();
    FixupTypes types;
    const auto typeCount = static_cast<std::size_t>(nonnegative(reader, "EFIX type"));
    ensure_records(reader, chunk.end, typeCount, 16, "EFIX type");
    types.guids.reserve(typeCount);
    for (std::size_t index = 0; index < typeCount; ++index) types.guids.push_back(reader.guid());
    const auto signatureCount = static_cast<std::size_t>(nonnegative(reader, "EFIX signature"));
    if (signatureCount > typeCount) throw std::runtime_error("EFIX has more signatures than types");
    ensure_records(reader, chunk.end, signatureCount, 4, "EFIX signature");
    for (std::size_t index = 0; index < signatureCount; ++index) types.signatures.push_back(reader.u32());
    document.fixupTypeGuids = types.guids;
    document.fixupTypeSignatures = types.signatures;

    const auto exportedCount = static_cast<std::size_t>(nonnegative(reader, "EFIX exported instance"));
    const auto instanceCount = static_cast<std::size_t>(nonnegative(reader, "EFIX instance"));
    if (exportedCount > instanceCount) throw std::runtime_error("EFIX exported instance count is invalid");
    ensure_records(reader, chunk.end, instanceCount, 4, "EFIX instance");
    document.instances.reserve(instanceCount);
    for (std::size_t index = 0; index < instanceCount; ++index) {
        const auto offset = reader.u32();
        validate_data_offset(document, offset, 2, "EFIX instance");
        const auto saved = reader.position();
        reader.seek(document.dataStart + offset);
        const auto classRef = reader.u16();
        reader.seek(saved);
        if (classRef >= types.guids.size()) throw std::runtime_error("EFIX instance type is invalid");
        document.instances.push_back({classRef, -1, offset, index < exportedCount});
    }

    auto readOffsets = [&](std::vector<std::uint32_t>& output, const char* label, const std::size_t width) {
        const auto count = static_cast<std::size_t>(nonnegative(reader, label));
        ensure_records(reader, chunk.end, count, 4, label);
        output.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = reader.u32();
            validate_data_offset(document, offset, width, label);
            output.push_back(offset);
        }
    };
    readOffsets(document.pointerOffsets, "EFIX pointer", 8);
    readOffsets(document.resourceRefOffsets, "EFIX resource reference", 8);

    const auto importCount = static_cast<std::size_t>(nonnegative(reader, "EFIX import"));
    ensure_records(reader, chunk.end, importCount, 32, "EFIX import");
    document.imports.reserve(importCount);
    for (std::size_t index = 0; index < importCount; ++index) {
        document.imports.push_back({reader.guid(), reader.guid()});
    }
    readOffsets(document.importOffsets, "EFIX import relocation", 8);
    readOffsets(document.typeInfoOffsets, "EFIX type-info reference", 8);
    if (chunk.end - reader.position() < 12) throw std::runtime_error("truncated EFIX section offsets");
    document.arraysOffset = reader.u32();
    document.boxedValuesOffset = reader.u32();
    document.stringsOffset = reader.u32();
    validate_data_offset(document, document.arraysOffset, 0, "EFIX array section");
    validate_data_offset(document, document.boxedValuesOffset, 0, "EFIX boxed-value section");
    validate_data_offset(document, document.stringsOffset, 0, "EFIX string section");
    if (reader.position() < chunk.end) {
        const auto count = static_cast<std::size_t>(nonnegative(reader, "EFIX boxed reference"));
        ensure_records(reader, chunk.end, count, 4, "EFIX boxed reference");
        document.boxedReferenceOffsets.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto offset = reader.u32();
            validate_data_offset(document, offset, 16, "EFIX boxed reference");
            document.boxedReferenceOffsets.push_back(offset);
        }
    }
    if (reader.position() != chunk.end) throw std::runtime_error("unexpected bytes at the end of EFIX");
    return types;
}

void read_extra(BinaryReader& reader, const Chunk& chunk, Document& document) {
    reader.seek(chunk.begin);
    const auto arrayCount = static_cast<std::size_t>(nonnegative(reader, "EBXX array"));
    const auto boxedCount = static_cast<std::size_t>(nonnegative(reader, "EBXX boxed value"));
    ensure_records(reader, chunk.end, arrayCount + boxedCount, 16, "EBXX");
    document.arrays.reserve(arrayCount);
    for (std::size_t index = 0; index < arrayCount; ++index) {
        ArrayRecord value;
        value.offset = reader.u32();
        const auto count = reader.i32();
        if (count < 0) throw std::runtime_error("EBXX array has a negative count");
        value.count = static_cast<std::uint32_t>(count);
        value.hash = reader.u32();
        value.encodedType = reader.u16();
        value.classRef = reader.u16();
        validate_data_offset(document, value.offset, 0, "EBXX array");
        document.arrays.push_back(value);
    }
    document.boxedValues.reserve(boxedCount);
    for (std::size_t index = 0; index < boxedCount; ++index) {
        BoxedValueRecord value;
        value.offset = reader.u32();
        const auto count = reader.i32();
        if (count < 0) throw std::runtime_error("EBXX boxed value has a negative count");
        value.count = static_cast<std::uint32_t>(count);
        value.hash = reader.u32();
        value.encodedType = reader.u16();
        value.classRef = reader.u16();
        validate_data_offset(document, value.offset, 0, "EBXX boxed value");
        document.boxedValues.push_back(value);
    }
    if (reader.position() != chunk.end) throw std::runtime_error("unexpected bytes at the end of EBXX");
}

std::string string_at(BinaryReader& reader, const std::size_t offset, const std::size_t end) {
    if (offset >= end) throw std::runtime_error("reflection string offset is invalid");
    const auto saved = reader.position();
    reader.seek(offset);
    std::string result;
    while (reader.position() < end) {
        const auto value = reader.u8();
        if (value == 0) {
            reader.seek(saved);
            return result;
        }
        result.push_back(static_cast<char>(value));
    }
    reader.seek(saved);
    throw std::runtime_error("reflection string is unterminated");
}

std::vector<ReflectionTypeId> read_reflection(BinaryReader& reader,
                                               const Chunk& chunk,
                                               const bool hashedNames,
                                               Document& document) {
    reader.seek(chunk.begin);
    const auto signatureCount = static_cast<std::size_t>(nonnegative(reader, "reflection signature"));
    ensure_records(reader, chunk.end, signatureCount, 20, "reflection signature");
    std::vector<ReflectionTypeId> ids(signatureCount);
    for (auto& id : ids) {
        id.guid = reader.guid();
        id.signature = reader.u32();
    }

    const auto typeCount = static_cast<std::size_t>(nonnegative(reader, "reflection type"));
    ensure_records(reader, chunk.end, typeCount, 16, "reflection type");
    std::vector<std::uint32_t> typeNames;
    typeNames.reserve(typeCount);
    document.types.reserve(typeCount);
    for (std::size_t index = 0; index < typeCount; ++index) {
        TypeDescriptor value;
        value.nameHash = reader.u32();
        value.fieldIndex = reader.i32();
        value.fieldCount = reader.u16();
        value.encodedType = static_cast<std::uint16_t>(reader.u16() >> 1);
        value.size = reader.u16();
        value.alignment = reader.u16();
        if (value.alignment > std::numeric_limits<std::uint8_t>::max()) {
            throw std::runtime_error("reflection type alignment is unsupported");
        }
        if (index < ids.size()) {
            value.guid = ids[index].guid;
            value.signature = ids[index].signature;
        }
        typeNames.push_back(value.nameHash);
        document.types.push_back(std::move(value));
    }

    const auto fieldCount = static_cast<std::size_t>(nonnegative(reader, "reflection field"));
    ensure_records(reader, chunk.end, fieldCount, 12, "reflection field");
    std::vector<std::uint32_t> fieldNames;
    fieldNames.reserve(fieldCount);
    document.fields.reserve(fieldCount);
    for (std::size_t index = 0; index < fieldCount; ++index) {
        FieldDescriptor value;
        value.nameHash = reader.u32();
        value.dataOffset = reader.u32();
        value.encodedType = static_cast<std::uint16_t>(reader.u16() >> 1);
        value.classRef = reader.u16();
        fieldNames.push_back(value.nameHash);
        document.fields.push_back(std::move(value));
    }
    for (const auto& type : document.types) {
        if (type.fieldIndex < 0 || static_cast<std::size_t>(type.fieldIndex) > document.fields.size() ||
            type.fieldCount > document.fields.size() - static_cast<std::size_t>(type.fieldIndex)) {
            throw std::runtime_error("reflection type field range is invalid");
        }
    }

    const auto groupCount = static_cast<std::size_t>(nonnegative(reader, "reflection group"));
    ensure_records(reader, chunk.end, groupCount, 12, "reflection group");
    document.reflectionGroups.reserve(groupCount);
    for (std::size_t index = 0; index < groupCount; ++index) {
        ReflectionGroup group;
        group.hash = reader.u32();
        group.count = reader.u32();
        group.first = reader.u32();
        document.reflectionGroups.push_back(group);
    }
    const auto mappingCount = static_cast<std::size_t>(nonnegative(reader, "reflection mapping"));
    ensure_records(reader, chunk.end, mappingCount, 8, "reflection mapping");
    reader.skip(static_cast<std::ptrdiff_t>(mappingCount * 8));

    if (hashedNames) {
        const auto nameCount = static_cast<std::size_t>(nonnegative(reader, "reflection name"));
        ensure_records(reader, chunk.end, nameCount, 8, "reflection name");
        std::unordered_map<std::uint32_t, std::uint32_t> nameOffsets;
        for (std::size_t index = 0; index < nameCount; ++index) {
            const auto hash = reader.u32();
            const auto offset = reader.u32();
            nameOffsets[hash] = offset;
        }
        const auto strings = reader.position();
        for (std::size_t index = 0; index < document.types.size(); ++index) {
            const auto found = nameOffsets.find(typeNames[index]);
            if (found == nameOffsets.end()) {
                throw std::runtime_error("RFL2 type name 0x" + std::to_string(typeNames[index]) +
                                         " is missing from " + std::to_string(nameOffsets.size()) + " names");
            }
            document.types[index].name = string_at(reader, strings + found->second, chunk.end);
        }
        for (std::size_t index = 0; index < document.fields.size(); ++index) {
            const auto found = nameOffsets.find(fieldNames[index]);
            if (found == nameOffsets.end()) throw std::runtime_error("RFL2 field name is missing");
            document.fields[index].name = string_at(reader, strings + found->second, chunk.end);
        }
    } else {
        const auto strings = reader.position();
        for (std::size_t index = 0; index < document.types.size(); ++index) {
            document.types[index].name = string_at(reader, strings + typeNames[index], chunk.end);
        }
        for (std::size_t index = 0; index < document.fields.size(); ++index) {
            document.fields[index].name = string_at(reader, strings + fieldNames[index], chunk.end);
        }
    }
    return ids;
}

void map_instances(Document& document,
                   const FixupTypes& fixup,
                   const std::vector<ReflectionTypeId>& reflection) {
    for (auto& instance : document.instances) {
        if (instance.fixupType >= fixup.guids.size() || instance.fixupType >= fixup.signatures.size()) continue;
        for (std::size_t index = 0; index < reflection.size(); ++index) {
            if (reflection[index].guid == fixup.guids[instance.fixupType] &&
                reflection[index].signature == fixup.signatures[instance.fixupType]) {
                instance.descriptor = static_cast<std::int32_t>(index);
                break;
            }
        }
    }
    if (!document.instances.empty()) {
        const auto descriptor = document.instances.front().descriptor;
        if (descriptor >= 0 && static_cast<std::size_t>(descriptor) < document.types.size()) {
            document.rootType = document.types[static_cast<std::size_t>(descriptor)].name;
        }
    }
}

class ValueReader final {
public:
    ValueReader(const std::span<const std::byte> bytes, Document& document)
        : bytes_(bytes), reader_(bytes), document_(document) {
        for (const auto& record : document.arrays) arrayHashes_.emplace(record.offset, record.hash);
    }

    void read_instances() {
        for (auto& instance : document_.instances) {
            if (instance.descriptor < 0 || static_cast<std::size_t>(instance.descriptor) >= document_.types.size()) {
                continue;
            }
            const auto classStart = document_.dataStart + instance.dataOffset;
            if (instance.exported) {
                if (classStart < document_.dataStart + 16) throw std::runtime_error("exported EBX instance has no GUID");
                reader_.seek(classStart - 16);
                instance.instanceGuid = reader_.guid();
            }
            const auto typeSize = document_.types[static_cast<std::size_t>(instance.descriptor)].size;
            if (classStart < document_.dataStart || typeSize > document_.dataEnd - classStart) {
                throw std::runtime_error("EBX object exceeds EBXD");
            }
            instance.rawImage.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(classStart),
                                     bytes_.begin() + static_cast<std::ptrdiff_t>(classStart + typeSize));
            instance.object = read_object(instance.descriptor, classStart);
        }
    }

private:
    // A type can name itself as its base or as a member: bound the nesting so a bad
    // file fails to parse instead of overflowing the stack.
    class Nesting final {
    public:
        explicit Nesting(unsigned& depth) : depth_(depth) {
            if (depth_ >= 128) throw std::runtime_error("EBX types nest too deeply");
            ++depth_;
        }
        ~Nesting() { --depth_; }
        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;
    private:
        unsigned& depth_;
    };

    [[nodiscard]] std::shared_ptr<Object> read_object(const std::int32_t descriptor,
                                                       const std::size_t start) {
        const Nesting nesting(depth_);
        if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document_.types.size()) {
            throw std::runtime_error("EBX object type reference is invalid");
        }
        const auto& type = document_.types[static_cast<std::size_t>(descriptor)];
        if (start < document_.dataStart || type.size > document_.dataEnd - start) {
            throw std::runtime_error("EBX object exceeds EBXD");
        }
        auto result = std::make_shared<Object>();
        result->descriptor = descriptor;
        read_fields(*result, type, start);
        return result;
    }

    void read_fields(Object& output, const TypeDescriptor& type, const std::size_t start) {
        for (std::size_t fieldIndex = 0; fieldIndex < type.fieldCount; ++fieldIndex) {
            const auto descriptorIndex = static_cast<std::size_t>(type.fieldIndex) + fieldIndex;
            const auto& field = document_.fields[descriptorIndex];
            if (field.type() == FieldType::inherited) {
                if (field.classRef >= document_.types.size()) throw std::runtime_error("EBX base type is invalid");
                const Nesting nesting(depth_);
                read_fields(output, document_.types[field.classRef], start);
                continue;
            }
            const auto position = start + field.dataOffset;
            if (position > document_.dataEnd) throw std::runtime_error("EBX field offset exceeds EBXD");
            auto value = read_field(field, position);
            if (field.category() == FieldCategory::array) {
                const auto* values = std::get_if<Value::Array>(&value.data);
                if (values && !values->empty()) {
                    std::int32_t displacement{};
                    std::memcpy(&displacement, bytes_.data() + position, sizeof(displacement));
                    const auto target = static_cast<std::int64_t>(position) + displacement -
                                        static_cast<std::int64_t>(document_.dataStart);
                    if (const auto found = arrayHashes_.find(target); found != arrayHashes_.end())
                        value.arrayHash = found->second;
                }
            }
            output.fields.push_back({descriptorIndex, field.name, std::move(value)});
        }
    }

    [[nodiscard]] Value read_field(const FieldDescriptor& field, const std::size_t position) {
        if (field.category() == FieldCategory::array) {
            reader_.seek(position);
            const auto displacement = reader_.i32();
            const auto targetSigned = static_cast<std::int64_t>(position) + displacement;
            if (targetSigned < static_cast<std::int64_t>(document_.dataStart + 4) ||
                targetSigned > static_cast<std::int64_t>(document_.dataEnd)) {
                throw std::runtime_error("EBX array displacement exceeds EBXD");
            }
            const auto target = static_cast<std::size_t>(targetSigned);
            reader_.seek(target - 4);
            const auto count = reader_.i32();
            if (count < 0) throw std::runtime_error("EBX array has a negative count");
            // Every element takes at least a byte.
            if (static_cast<std::size_t>(count) > document_.dataEnd - target) throw std::runtime_error("EBX array exceeds EBXD");
            reader_.seek(target);
            Value::Array values;
            values.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index) {
                const auto elementStart = reader_.position();
                values.push_back(read_value(field.type(), field.classRef, elementStart));
                if (field.type() == FieldType::structure) {
                    if (field.classRef >= document_.types.size()) throw std::runtime_error("EBX array struct type is invalid");
                    reader_.seek(elementStart + document_.types[field.classRef].size);
                } else if (field.type() == FieldType::pointer || field.type() == FieldType::cString) {
                    reader_.seek(align_up(reader_.position(), 8));
                }
            }
            return Value{std::move(values)};
        }
        reader_.seek(position);
        return read_value(field.type(), field.classRef, position);
    }

    [[nodiscard]] Value read_value(const FieldType type,
                                   const std::uint16_t classRef,
                                   const std::size_t position) {
        switch (type) {
        case FieldType::boolean: return Value{reader_.u8() != 0};
        case FieldType::int8: return Value{static_cast<std::int64_t>(reader_.i8())};
        case FieldType::uint8: return Value{static_cast<std::uint64_t>(reader_.u8())};
        case FieldType::int16: return Value{static_cast<std::int64_t>(reader_.i16())};
        case FieldType::uint16: return Value{static_cast<std::uint64_t>(reader_.u16())};
        case FieldType::int32:
        case FieldType::enumeration: return Value{static_cast<std::int64_t>(reader_.i32())};
        case FieldType::uint32: return Value{static_cast<std::uint64_t>(reader_.u32())};
        case FieldType::int64: return Value{reader_.i64()};
        case FieldType::uint64: return Value{reader_.u64()};
        case FieldType::float32: return Value{static_cast<double>(reader_.f32())};
        case FieldType::float64: return Value{reader_.f64()};
        case FieldType::guid: return Value{reader_.guid()};
        case FieldType::sha1: return Value{reader_.sha1()};
        case FieldType::resourceRef: return Value{ResourceReference{reader_.u64()}};
        case FieldType::string: return Value{reader_.fixed_string(32)};
        case FieldType::cString: {
            const auto displacement = reader_.u32();
            return Value{relative_string(position, displacement)};
        }
        case FieldType::fileRef: {
            const auto displacement = reader_.u32();
            reader_.skip(4);
            return Value{relative_string(position, displacement)};
        }
        case FieldType::pointer: {
            const auto encoded = reader_.i32();
            if (encoded == 0) return Value{PointerReference{PointerKind::null, -1}};
            if ((encoded & 1) != 0) {
                const auto index = encoded >> 1;
                if (index < 0 || static_cast<std::size_t>(index) >= document_.imports.size()) {
                    throw std::runtime_error("EBX pointer import is invalid");
                }
                return Value{PointerReference{PointerKind::external, index}};
            }
            const auto target = static_cast<std::int64_t>(position) + encoded -
                                static_cast<std::int64_t>(document_.dataStart);
            const auto found = std::ranges::find_if(document_.instances, [&](const InstanceRecord& instance) {
                return instance.dataOffset == target;
            });
            if (found == document_.instances.end()) throw std::runtime_error("EBX internal pointer target is invalid");
            return Value{PointerReference{PointerKind::internal,
                                          static_cast<std::int32_t>(found - document_.instances.begin())}};
        }
        case FieldType::typeRef:
        case FieldType::delegate: {
            auto encoded = reader_.u32();
            reader_.skip(4);
            if (encoded == 0) return Value{TypeReference{}};
            if ((encoded & 0x80000000U) != 0) {
                const auto raw = encoded;
                encoded &= ~0x80000000U;
                return Value{TypeReference{true, static_cast<FieldType>((encoded >> 5) & 0x1F), -1, raw}};
            }
            return Value{TypeReference{false, {}, static_cast<std::int32_t>(encoded >> 2), encoded}};
        }
        case FieldType::boxedValueRef: {
            const auto encoded = reader_.u32();
            reader_.skip(4);
            const auto displacement = reader_.i64();
            const auto target = static_cast<std::int64_t>(position + 8) + displacement -
                                static_cast<std::int64_t>(document_.dataStart);
            document_.boxedValueSlots.push_back(static_cast<std::uint32_t>(position - document_.dataStart));
            return Value{BoxedReference{encoded, target, displacement}};
        }
        case FieldType::structure:
            return Value{read_object(static_cast<std::int32_t>(classRef), position)};
        default:
            return Value{};
        }
    }

    [[nodiscard]] std::string relative_string(const std::size_t base, const std::uint32_t displacement) {
        if (displacement == std::numeric_limits<std::uint32_t>::max()) return {};
        const auto target = static_cast<std::uint64_t>(base) + displacement;
        if (target < document_.dataStart || target >= document_.dataEnd) {
            throw std::runtime_error("EBX string displacement exceeds EBXD");
        }
        const auto saved = reader_.position();
        reader_.seek(static_cast<std::size_t>(target));
        const auto value = reader_.c_string();
        if (reader_.position() > document_.dataEnd) throw std::runtime_error("EBX string exceeds EBXD");
        reader_.seek(saved);
        return value;
    }

    std::span<const std::byte> bytes_;
    std::unordered_map<std::int64_t, std::uint32_t> arrayHashes_;   // record offset -> its hash

    BinaryReader reader_;
    Document& document_;
    unsigned depth_{};
};

}

FieldType FieldDescriptor::type() const noexcept {
    return static_cast<FieldType>((encodedType >> 4) & 0x1F);
}

FieldCategory FieldDescriptor::category() const noexcept {
    return static_cast<FieldCategory>(encodedType & 0x0F);
}

FieldType TypeDescriptor::type() const noexcept {
    return static_cast<FieldType>((encodedType >> 4) & 0x1F);
}

FieldCategory TypeDescriptor::category() const noexcept {
    return static_cast<FieldCategory>(encodedType & 0x0F);
}

const FieldValue* Object::find(const std::string_view name) const {
    const auto found = std::ranges::find_if(fields, [&](const FieldValue& field) {
        return std::ranges::equal(field.name, name, [](const char left, const char right) {
            return std::tolower(static_cast<unsigned char>(left)) ==
                   std::tolower(static_cast<unsigned char>(right));
        });
    });
    return found == fields.end() ? nullptr : &*found;
}

const InstanceRecord* Document::root() const noexcept {
    return instances.empty() ? nullptr : &instances.front();
}

Document read_document(const std::span<const std::byte> bytes) {
    BinaryReader reader(bytes);
    const auto chunks = read_chunks(reader);
    const auto& data = unique_chunk(chunks, ebxd, "EBXD");
    const auto& fixupChunk = unique_chunk(chunks, efix, "EFIX");
    const auto& extra = unique_chunk(chunks, ebxx, "EBXX");
    const auto* reflection = optional_chunk(chunks, refl, "REFL");
    const auto* reflection2 = optional_chunk(chunks, rfl2, "RFL2");
    if ((reflection == nullptr) == (reflection2 == nullptr)) {
        throw std::runtime_error("RIFF EBX must contain exactly one REFL or RFL2 chunk");
    }

    Document result;
    reader.seek(8);
    result.serializedLayout = reader.u32(Endian::big) == ebxs;
    result.dataStart = align_up(data.begin, 16);
    result.dataEnd = data.end;
    if (result.dataStart > result.dataEnd) throw std::runtime_error("EBXD alignment exceeds the chunk");
    const auto fixup = read_fixup(reader, fixupChunk, result);
    read_extra(reader, extra, result);
    const auto reflectionIds = read_reflection(reader, reflection2 ? *reflection2 : *reflection,
                                                reflection2 != nullptr, result);
    const auto& reflectionChunk = reflection2 ? *reflection2 : *reflection;
    result.hashedReflectionNames = reflection2 != nullptr;
    result.reflectionData.assign(bytes.begin() + static_cast<std::ptrdiff_t>(reflectionChunk.begin),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(reflectionChunk.end));
    result.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(result.dataStart),
                          bytes.begin() + static_cast<std::ptrdiff_t>(result.dataEnd));
    map_instances(result, fixup, reflectionIds);
    ValueReader(bytes, result).read_instances();
    std::ranges::sort(result.boxedValues, {}, &BoxedValueRecord::offset);
    for (std::size_t index = 0; index < result.boxedValues.size(); ++index) {
        auto& value = result.boxedValues[index];
        const auto end = index + 1 < result.boxedValues.size()
            ? result.boxedValues[index + 1].offset
            : result.stringsOffset;
        if (end < value.offset) throw std::runtime_error("EBXX boxed values are not ordered");
        validate_data_offset(result, value.offset, end - value.offset, "EBXX boxed value bytes");
        value.rawBytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(result.dataStart + value.offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(result.dataStart + end));
    }
    return result;
}

Guid read_file_guid(const std::span<const std::byte> bytes) {
    BinaryReader reader(bytes);
    const auto chunks = read_chunks(reader);
    reader.seek(unique_chunk(chunks, efix, "EFIX").begin);
    return reader.guid();
}

}
