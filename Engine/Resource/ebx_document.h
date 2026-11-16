#pragma once

#include "binary_io.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dingosdk::frostbite::ebx {

enum class FieldType : std::uint8_t {
    inherited = 0x00,
    dbObject = 0x01,
    structure = 0x02,
    pointer = 0x03,
    array = 0x04,
    fixedArray = 0x05,
    string = 0x06,
    cString = 0x07,
    enumeration = 0x08,
    fileRef = 0x09,
    boolean = 0x0A,
    int8 = 0x0B,
    uint8 = 0x0C,
    int16 = 0x0D,
    uint16 = 0x0E,
    int32 = 0x0F,
    uint32 = 0x10,
    uint64 = 0x11,
    int64 = 0x12,
    float32 = 0x13,
    float64 = 0x14,
    guid = 0x15,
    sha1 = 0x16,
    resourceRef = 0x17,
    function = 0x18,
    typeRef = 0x19,
    boxedValueRef = 0x1A,
    interfaceType = 0x1B,
    delegate = 0x1C
};

enum class FieldCategory : std::uint8_t {
    none,
    pointer,
    structure,
    primitive,
    array,
    enumeration,
    function,
    interfaceType,
    delegate
};

struct FieldDescriptor {
    std::string name;
    std::uint32_t nameHash{};
    std::uint16_t encodedType{};
    std::uint16_t classRef{};
    std::uint32_t dataOffset{};

    [[nodiscard]] FieldType type() const noexcept;
    [[nodiscard]] FieldCategory category() const noexcept;
};

struct TypeDescriptor {
    std::string name;
    std::uint32_t nameHash{};
    std::int32_t fieldIndex{};
    std::uint16_t fieldCount{};
    std::uint16_t encodedType{};
    std::uint16_t size{};
    std::uint16_t alignment{};
    Guid guid;
    std::uint32_t signature{};

    [[nodiscard]] FieldType type() const noexcept;
    [[nodiscard]] FieldCategory category() const noexcept;
};

struct ImportReference {
    Guid fileGuid;
    Guid classGuid;
};

struct ArrayRecord {
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t hash{};
    std::uint16_t encodedType{};
    std::uint16_t classRef{};
};

// A 12-byte record of the RFL2 group table: one per array type the document uses, sorted by
// hash (first[i + 1] = first[i] + count[i]). Every array record's hash is one of these keys.
struct ReflectionGroup {
    std::uint32_t hash{};
    std::uint32_t count{};
    std::uint32_t first{};
};

struct BoxedValueRecord {
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t hash{};
    std::uint16_t encodedType{};
    std::uint16_t classRef{};
    std::vector<std::byte> rawBytes;
};

struct InstanceRecord {
    std::uint16_t fixupType{};
    std::int32_t descriptor{-1};
    std::uint32_t dataOffset{};
    bool exported{};
    Guid instanceGuid;
    std::shared_ptr<struct Object> object;
    std::vector<std::byte> rawImage;
};

enum class PointerKind {
    null,
    internal,
    external
};

struct PointerReference {
    PointerKind kind{};
    std::int32_t index{-1};
};

struct TypeReference {
    bool primitive{};
    FieldType primitiveType{};
    std::int32_t descriptor{-1};
    std::uint32_t encoded{};
};

struct ResourceReference {
    std::uint64_t id{};
};

struct BoxedValue;

// A boxed field: its type code (an index into the document's EFIX type list, times four, or a
// primitive with bit 31 set) and where its value lies in the boxed section.
struct BoxedReference {
    std::uint32_t encodedType{};
    std::int64_t dataOffset{-1};
    std::int64_t relativeOffset{};
    // What the box holds, read through its type when the document lays that type out
    // (read_document). Null for a box whose type it does not, which then only travels as bytes.
    std::shared_ptr<BoxedValue> content;
    // Set when `content` was changed or carried in from another document: the writer then lays
    // the content down afresh instead of carrying the bytes the box was read from.
    bool rewritten{};
};

struct Value {
    using Array = std::vector<Value>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                                 std::string, Guid, Sha1, ResourceReference,
                                 PointerReference, TypeReference, BoxedReference,
                                 std::shared_ptr<struct Object>, Array>;
    Storage data;
    // An array's own record hash, kept when the document is written again; a new array has none
    // and borrows one (ebx_writer.cpp, array_hash).
    std::optional<std::uint32_t> arrayHash;
};

struct FieldValue {
    std::size_t descriptor{};
    std::string name;
    Value value;
};

struct Object {
    std::int32_t descriptor{-1};
    std::vector<FieldValue> fields;

    [[nodiscard]] const FieldValue* find(std::string_view name) const;
};

// A boxed value read through its type. The type is named by its guid and signature, which is
// what two documents agree on (each numbers its own EFIX type list). A list type's guid is its
// element type's with the last byte one higher: that is how a boxed list is told apart, and its
// elements are structs the document describes or references to objects.
struct BoxedValue {
    enum class Kind { primitive, primitiveList, structure, list, reference };
    Kind kind{};
    FieldType primitiveType{};      // primitive and primitiveList: what each value is
    Guid type;                      // structure, list and reference: the boxed type
    std::uint32_t signature{};
    std::int32_t descriptor{-1};    // structure: its struct; list: its element struct, or -1 for references
    Value value;                    // an Object, an Array, a PointerReference or a scalar
    // The EBXX entries the box and its list were read with (count, hash, kind, type slot).
    BoxedValueRecord record;
    ArrayRecord listRecord;
};

struct Document {
    Guid fileGuid;
    std::string rootType;
    std::size_t dataStart{};
    std::size_t dataEnd{};
    std::uint32_t arraysOffset{};
    std::uint32_t boxedValuesOffset{};
    std::uint32_t stringsOffset{};
    bool serializedLayout{};
    bool hashedReflectionNames{};
    std::vector<std::byte> reflectionData;
    std::vector<Guid> fixupTypeGuids;
    std::vector<std::uint32_t> fixupTypeSignatures;
    std::vector<std::uint32_t> boxedReferenceOffsets;
    std::vector<TypeDescriptor> types;
    std::vector<FieldDescriptor> fields;
    std::vector<InstanceRecord> instances;
    std::vector<ImportReference> imports;
    std::vector<ArrayRecord> arrays;
    std::vector<BoxedValueRecord> boxedValues;
    std::vector<std::uint32_t> pointerOffsets;
    std::vector<std::uint32_t> resourceRefOffsets;
    std::vector<std::uint32_t> importOffsets;
    std::vector<std::uint32_t> typeInfoOffsets;
    std::vector<std::uint32_t> boxedValueSlots;
    // The EBXD bytes the document was read from (offsets as in the fixup tables): what boxed
    // values point at (arrays, strings) is carried from here when the document is written.
    std::vector<std::byte> payload;
    std::vector<ReflectionGroup> reflectionGroups;

    [[nodiscard]] const InstanceRecord* root() const noexcept;
};

[[nodiscard]] Document read_document(std::span<const std::byte> bytes);
// The document's own guid, which is what another document's import names it by,
// without reading the rest.
[[nodiscard]] Guid read_file_guid(std::span<const std::byte> bytes);

}
