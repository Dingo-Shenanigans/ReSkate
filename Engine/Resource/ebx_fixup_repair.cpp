#include "ebx_fixup_repair.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>

namespace dingosdk::frostbite::ebx {
namespace {

std::uint32_t u32_at(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) throw std::out_of_range("EBX read past its end");
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
void put_u32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }

// The fixup lists of an EBX, read from its EFIX chunk alone: this runs on every EBX a mod
// ships, and the rest of the document has nothing to say about where the fixups go.
struct Fixups {
    std::size_t efix{}, efixSize{};   // the chunk's data and its size
    std::size_t typeList{};           // where the type-reference count is
    std::vector<std::uint32_t> instances, pointers, resources, imports, types, boxed;
};

std::optional<Fixups> read_fixups(std::span<const std::byte> ebx) {
    if (ebx.size() < 12 || std::memcmp(ebx.data(), "RIFF", 4) != 0) return std::nullopt;
    Fixups out;
    const auto riffEnd = std::size_t{u32_at(ebx, 4)} + 8;
    if (riffEnd > ebx.size()) return std::nullopt;
    for (std::size_t at = 12; at + 8 <= riffEnd;) {
        const auto size = std::size_t{u32_at(ebx, at + 4)};
        if (std::memcmp(ebx.data() + at, "EFIX", 4) == 0) {
            out.efix = at + 8;
            out.efixSize = size;
            break;
        }
        at += 8 + size + (size & 1);
    }
    const auto end = out.efix + out.efixSize;
    if (!out.efix || end > riffEnd) return std::nullopt;

    std::size_t at = out.efix + 16;   // the file guid
    const auto count = [&](std::size_t width) {
        const auto value = std::size_t{u32_at(ebx, at)};
        at += 4;
        if (value > (end - at) / width) throw std::out_of_range("EFIX list past its chunk");
        return value;
    };
    const auto list = [&](std::vector<std::uint32_t>& into) {
        into.resize(count(4));
        for (auto& value : into) { value = u32_at(ebx, at); at += 4; }
    };
    at += 16 * count(16);   // types
    at += 4 * count(4);     // signatures
    at += 4;                // exported instances
    list(out.instances);
    list(out.pointers);
    list(out.resources);
    at += 32 * count(32);   // imports
    list(out.imports);
    out.typeList = at;
    list(out.types);
    if (end - at < 12) return std::nullopt;
    at += 12;               // the array, boxed-value and string sections
    if (at < end) list(out.boxed);
    if (at != end) return std::nullopt;
    return out;
}

// Which type-reference entries collide with another fixup's slot, or repeat.
std::set<std::size_t> colliding(const Fixups& fixups) {
    struct Range { std::uint64_t begin, end; };
    std::vector<Range> others;
    const auto add = [&](std::uint64_t begin, std::uint64_t size) { others.push_back({begin, begin + size}); };
    for (const auto offset : fixups.pointers) add(offset, 8);
    for (const auto offset : fixups.resources) add(offset, 8);
    for (const auto offset : fixups.imports) add(offset, 8);
    for (const auto offset : fixups.boxed) add(std::uint64_t{offset} + 8, 8);
    for (const auto offset : fixups.instances) add(offset, 16);
    std::set<std::size_t> bad;
    std::set<std::uint32_t> seen;
    for (std::size_t index = 0; index < fixups.types.size(); ++index) {
        const std::uint64_t offset = fixups.types[index];
        if (!seen.insert(fixups.types[index]).second) {
            bad.insert(index);
            continue;
        }
        for (const auto& other : others)
            if (offset < other.end && other.begin < offset + 8) {
                bad.insert(index);
                break;
            }
    }
    return bad;
}

} // namespace

std::optional<FixupRepair> repair_type_fixups(std::span<const std::byte> ebx) {
    std::optional<Fixups> fixups;
    try {
        fixups = read_fixups(ebx);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!fixups) return std::nullopt;
    const auto bad = colliding(*fixups);
    if (bad.empty()) return std::nullopt;

    // The type-reference list is the only thing that changes length. EBXD comes ahead of EFIX,
    // so the data's alignment and every offset into it stay as they are.
    FixupRepair out;
    out.removed = bad.size();
    const auto list = fixups->typeList;
    out.bytes.assign(ebx.begin(), ebx.begin() + static_cast<std::ptrdiff_t>(list + 4));
    for (std::size_t index = 0; index < fixups->types.size(); ++index) {
        if (bad.contains(index)) continue;
        const auto at = out.bytes.size();
        out.bytes.resize(at + 4);
        put_u32(out.bytes, at, fixups->types[index]);
    }
    const auto rest = list + 4 + 4 * fixups->types.size();
    out.bytes.insert(out.bytes.end(), ebx.begin() + static_cast<std::ptrdiff_t>(rest), ebx.end());
    const auto shrink = static_cast<std::uint32_t>(4 * bad.size());
    put_u32(out.bytes, list, static_cast<std::uint32_t>(fixups->types.size() - bad.size()));
    put_u32(out.bytes, fixups->efix - 4, static_cast<std::uint32_t>(fixups->efixSize) - shrink); // the chunk's size (its padding is unchanged)
    put_u32(out.bytes, 4, u32_at(ebx, 4) - shrink);                                             // the RIFF's
    // What was written reads back, without the collisions.
    const auto again = read_fixups(out.bytes);
    if (!again || !colliding(*again).empty() || again->types.size() != fixups->types.size() - bad.size())
        throw std::runtime_error("EBX type-reference repair did not read back");
    return out;
}

} // namespace dingosdk::frostbite::ebx
