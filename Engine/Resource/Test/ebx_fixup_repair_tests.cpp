// The EBX type-reference repair: an entry on another fixup's slot, or listed twice, comes out of
// the list, the chunk and RIFF sizes follow, and a document without one is left alone.
#include "Engine/Resource/ebx_fixup_repair.h"
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace fb = dingosdk::frostbite;

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}
void u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    const auto at = bytes.size();
    bytes.resize(at + 4);
    std::memcpy(bytes.data() + at, &value, 4);
}
std::uint32_t read(const std::vector<std::byte>& bytes, std::size_t at) {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
void chunk(std::vector<std::byte>& riff, std::string_view id, const std::vector<std::byte>& data) {
    for (const auto ch : id) riff.push_back(static_cast<std::byte>(ch));
    u32(riff, static_cast<std::uint32_t>(data.size()));
    riff.insert(riff.end(), data.begin(), data.end());
    if (data.size() & 1) riff.push_back(std::byte{});
}
std::vector<std::uint32_t> list_at(const std::vector<std::byte>& bytes, std::size_t at) {
    std::vector<std::uint32_t> out(read(bytes, at));
    for (std::size_t index = 0; index < out.size(); ++index) out[index] = read(bytes, at + 4 + 4 * index);
    return out;
}

// An EBX with one type, one instance at 0x10, a pointer at 0x40, one import relocated at 0x60,
// and the type references given. Returns where the type-reference count lies.
std::vector<std::byte> document(const std::vector<std::uint32_t>& types, std::size_t& typeList) {
    std::vector<std::byte> efix(16);   // the file guid
    u32(efix, 1);
    efix.resize(efix.size() + 16);     // its type
    u32(efix, 0);                      // no signatures
    u32(efix, 1);                      // exported
    u32(efix, 1);
    u32(efix, 0x10);                   // the instance
    u32(efix, 1);
    u32(efix, 0x40);                   // a pointer
    u32(efix, 0);                      // no resource references
    u32(efix, 1);
    efix.resize(efix.size() + 32);     // an import
    u32(efix, 1);
    u32(efix, 0x60);                   // its relocation
    const auto list = efix.size();
    u32(efix, static_cast<std::uint32_t>(types.size()));
    for (const auto type : types) u32(efix, type);
    u32(efix, 0x80);
    u32(efix, 0x80);
    u32(efix, 0x80);                   // the sections
    std::vector<std::byte> riff;
    for (const auto ch : std::string_view("RIFF")) riff.push_back(static_cast<std::byte>(ch));
    u32(riff, 0);
    for (const auto ch : std::string_view("EBX\0", 4)) riff.push_back(static_cast<std::byte>(ch));
    chunk(riff, "EBXD", std::vector<std::byte>(0x90));
    typeList = riff.size() + 8 + list;
    chunk(riff, "EFIX", efix);
    chunk(riff, "EBXX", std::vector<std::byte>(8));
    chunk(riff, "REFL", std::vector<std::byte>(5));
    const auto size = static_cast<std::uint32_t>(riff.size() - 8);
    std::memcpy(riff.data() + 4, &size, 4);
    return riff;
}
} // namespace

int main() {
    {
        std::size_t list{};
        const auto clean = document({0x20, 0x28}, list);
        check(!fb::ebx::repair_type_fixups(clean), "a document without collisions is left alone");
    }
    {
        // On the import's slot, inside the instance's header, and the same slot twice.
        std::size_t list{};
        const auto broken = document({0x20, 0x60, 0x18, 0x28, 0x20}, list);
        const auto repaired = fb::ebx::repair_type_fixups(broken);
        check(repaired.has_value(), "a document with collisions is repaired");
        if (repaired) {
            check(repaired->removed == 3, "three entries are removed");
            check(repaired->bytes.size() == broken.size() - 12, "the document shrinks by those entries");
            check(list_at(repaired->bytes, list) == std::vector<std::uint32_t>{0x20, 0x28}, "the clean entries stay, in order");
            check(read(repaired->bytes, 4) == read(broken, 4) - 12, "the RIFF size follows");
            constexpr std::size_t efixSize = 12 + 8 + 0x90 + 4;   // after the header and EBXD
            check(read(repaired->bytes, efixSize) == read(broken, efixSize) - 12, "the EFIX size follows");
            // EBXX (8 + 8 bytes) and REFL (8 + 5, padded) end the document.
            check(std::memcmp(repaired->bytes.data() + repaired->bytes.size() - 30, "EBXX", 4) == 0,
                  "the chunks after EFIX are carried along");
            check(!fb::ebx::repair_type_fixups(repaired->bytes), "the repaired document needs nothing more");
        }
    }
    check(!fb::ebx::repair_type_fixups(std::vector<std::byte>(4)), "something that is not an EBX is left alone");
    if (failures) return 1;
    std::cout << "ebx fixup repair tests passed\n";
    return 0;
}
