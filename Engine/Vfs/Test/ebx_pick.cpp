// A tool, not a test: one mod's edit of an asset list cut down to some of the records it adds.
//   dingosdk_ebx_pick <base.ebx> <edit.ebx> <out.ebx> <record>...
// The base is the game's own asset, the edit a mod's copy of it (both decoded EBX). The result
// is the edit with its root list cut back to the base's entries and the named records it
// appended (by DebugName, whole or after its last /). Used to carry the Throwdowns logo
// grid's records without the modes' logos that ReSkate_Throwdown adds beside them
// (tools/builtin/build_throwdown_menu_pack.py).
//
// The edit's bytes are changed in place, nothing else: the kept entries move up to follow the
// base's, and the list's two counts shrink. Rewriting the document instead would lay out its
// boxed values again, which the records' defaults all are (ebx_merge.cpp carries none either).
// The entries left out stay where they were, past the list's end, with their records unlisted:
// nothing finds an asset list's record but through its list.
#include "Engine/Resource/ebx_document.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
namespace ebx = dingosdk::frostbite::ebx;
std::vector<std::byte> load(const char* path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (raw.empty()) throw std::runtime_error(std::string("cannot read ") + path);
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}
std::string debug_name(const ebx::Document& document, const ebx::Value& entry) {
    const auto* pointer = std::get_if<ebx::PointerReference>(&entry.data);
    if (!pointer || pointer->kind != ebx::PointerKind::internal || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.instances.size()) return {};
    const auto& object = document.instances[static_cast<std::size_t>(pointer->index)].object;
    const auto* field = object ? object->find("DebugName") : nullptr;
    const auto* text = field ? std::get_if<std::string>(&field->value.data) : nullptr;
    return text ? *text : std::string{};
}
template<class T> T get(const std::vector<std::byte>& bytes, std::size_t at) {
    if (at + sizeof(T) > bytes.size()) throw std::runtime_error("read past the document's end");
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}
template<class T> void put(std::vector<std::byte>& bytes, std::size_t at, T value) { std::memcpy(bytes.data() + at, &value, sizeof(T)); }
} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: dingosdk_ebx_pick <base.ebx> <edit.ebx> <out.ebx> <record>...\n";
        return 2;
    }
    try {
        const auto base = ebx::read_document(load(argv[1]));
        auto bytes = load(argv[2]);
        const auto edit = ebx::read_document(bytes);
        const std::set<std::string> wanted(argv + 4, argv + argc);
        const auto* base_root = base.root();
        const auto* edit_root = edit.root();
        if (!base_root || !base_root->object || !edit_root || !edit_root->object) throw std::runtime_error("no root object");
        std::set<std::string> found;
        unsigned lists{};
        for (const auto& field : edit_root->object->fields) {
            const auto* entries = std::get_if<ebx::Value::Array>(&field.value.data);
            const auto* before = base_root->object->find(field.name);
            const auto* base_entries = before ? std::get_if<ebx::Value::Array>(&before->value.data) : nullptr;
            if (!entries || !base_entries || entries->size() <= base_entries->size()) continue;
            // The list's storage: the one array record of its length whose 8-byte slots decode to
            // exactly its entries.
            const auto slot_kind = [&](std::size_t at) { return get<std::int32_t>(bytes, at); };
            std::optional<std::size_t> record;
            for (std::size_t r = 0; r < edit.arrays.size(); ++r) {
                const auto& array = edit.arrays[r];
                if (array.count != entries->size()) continue;
                const auto start = edit.dataStart + array.offset;
                bool same = true;
                for (std::size_t i = 0; i < entries->size() && same; ++i) {
                    const auto* pointer = std::get_if<ebx::PointerReference>(&(*entries)[i].data);
                    const auto encoded = slot_kind(start + i * 8);
                    if (!pointer) same = false;
                    else if (pointer->kind == ebx::PointerKind::null) same = encoded == 0;
                    else if (pointer->kind == ebx::PointerKind::external) same = encoded == ((pointer->index << 1) | 1);
                    else same = (encoded & 1) == 0 && encoded != 0 &&
                                static_cast<std::int64_t>(start + i * 8) + encoded - static_cast<std::int64_t>(edit.dataStart) ==
                                    edit.instances[static_cast<std::size_t>(pointer->index)].dataOffset;
                }
                if (!same) continue;
                if (record) throw std::runtime_error("two arrays hold " + field.name);
                record = r;
            }
            if (!record) throw std::runtime_error("cannot find the storage of " + field.name);
            const auto& array = edit.arrays[*record];
            const auto start = edit.dataStart + array.offset;
            if (get<std::uint32_t>(bytes, start - 4) != array.count) throw std::runtime_error(field.name + "'s stored count differs");
            std::vector<std::size_t> order;
            for (std::size_t i = 0; i < base_entries->size(); ++i) order.push_back(i);
            for (auto at = base_entries->size(); at < entries->size(); ++at) {
                const auto name = debug_name(edit, (*entries)[at]);
                const auto leaf = name.substr(name.find_last_of('/') + 1);
                if (wanted.contains(name) || wanted.contains(leaf)) {
                    order.push_back(at);
                    found.insert(leaf);
                } else if (!name.empty()) std::cout << "left out " << name << '\n';
            }
            const std::vector<std::byte> before_bytes(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                                      bytes.begin() + static_cast<std::ptrdiff_t>(start + array.count * 8));
            const std::set<std::uint32_t> internal(edit.pointerOffsets.begin(), edit.pointerOffsets.end());
            const std::set<std::uint32_t> external(edit.importOffsets.begin(), edit.importOffsets.end());
            for (std::size_t slot = 0; slot < order.size(); ++slot) {
                const auto from = order[slot];
                if (from == slot) continue;
                std::memcpy(bytes.data() + start + slot * 8, before_bytes.data() + from * 8, 8);
                const auto encoded = get<std::int32_t>(before_bytes, from * 8);
                const auto offset = static_cast<std::uint32_t>(array.offset + slot * 8);
                if (encoded == 0) continue;
                if (encoded & 1) {
                    if (!external.contains(offset)) throw std::runtime_error("an import moved to a slot with no import fixup");
                    continue;
                }
                // An internal pointer is relative to its own slot.
                if (!internal.contains(offset)) throw std::runtime_error("a pointer moved to a slot with no pointer fixup");
                const auto target = static_cast<std::int64_t>(start + from * 8) + encoded;
                put<std::int32_t>(bytes, start + slot * 8, static_cast<std::int32_t>(target - static_cast<std::int64_t>(start + slot * 8)));
            }
            // Both counts: before the list's storage, and its EBXX array record.
            put<std::uint32_t>(bytes, start - 4, static_cast<std::uint32_t>(order.size()));
            std::size_t extra{};
            for (std::size_t at = 0; at + 16 <= bytes.size(); ++at)
                if (std::memcmp(bytes.data() + at, "EBXX", 4) == 0 && get<std::uint32_t>(bytes, at + 8) == edit.arrays.size() &&
                    get<std::uint32_t>(bytes, at + 12) == edit.boxedValues.size()) { extra = at + 8; break; }
            if (!extra) throw std::runtime_error("cannot find the EBXX array table");
            const auto entry = extra + 8 + *record * 16;
            if (get<std::uint32_t>(bytes, entry) != array.offset || get<std::uint32_t>(bytes, entry + 4) != array.count)
                throw std::runtime_error("the EBXX array record differs");
            put<std::uint32_t>(bytes, entry + 4, static_cast<std::uint32_t>(order.size()));
            ++lists;
        }
        if (!lists) throw std::runtime_error("the edit appends to none of the base's lists");
        for (const auto& name : wanted)
            if (!found.contains(name.substr(name.find_last_of('/') + 1))) throw std::runtime_error("the edit adds no record " + name);
        // It must read back as the same document, with the lists it was meant to have.
        const auto check = ebx::read_document(bytes);
        if (check.instances.size() != edit.instances.size() || check.imports.size() != edit.imports.size())
            throw std::runtime_error("the edited copy does not read back");
        std::ofstream(argv[3], std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()),
                                                                          static_cast<std::streamsize>(bytes.size()));
        std::cout << "kept " << found.size() << " record(s) the edit adds, in place; " << bytes.size() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dingosdk_ebx_pick: " << error.what() << '\n';
        return 1;
    }
}
