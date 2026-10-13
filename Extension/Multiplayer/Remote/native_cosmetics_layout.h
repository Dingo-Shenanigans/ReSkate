#pragma once
#include "cosmetics.h"
#include "native_pose_layout.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_cosmetics.h"
#include <algorithm>
#include <cstring>
#include <map>

namespace dingosdk::multiplayer {
struct NativeCosmeticItem {
    const char *asset{};
    const std::uint32_t *parameters{};
    std::uint32_t hash{}, slot{};
};
// Component+0x150 is the recipe value, without the resource pointer found in
// preset-manager records. The engine deep-copies this value (addr::native_cosmetics::recipe_copy).
struct NativeCosmeticRecipe {
    const std::uint32_t *scalars{};
    const NativeCosmeticItem *items{};
    const std::uint32_t *mask{};
    std::uint32_t word{};
    std::uint8_t flag{};
    std::array<std::byte, 3> padding{};
};
static_assert(sizeof(NativeCosmeticItem) == 0x18 && sizeof(NativeCosmeticRecipe) == 0x20);
static_assert(offsetof(NativeCosmeticRecipe, items) == 8 && offsetof(NativeCosmeticRecipe, flag) == 0x1c);

template <class T> struct CosmeticArray {
    std::vector<std::uint64_t> storage;
    explicit CosmeticArray(std::size_t count) : storage((8 + count * sizeof(T) + 7) / 8) {
        const auto n = static_cast<std::uint32_t>(count);
        std::memcpy(storage.data(), &n, 4);
        std::memcpy(reinterpret_cast<std::byte *>(storage.data()) + 4, &n, 4);
    }
    T *data() { return reinterpret_cast<T *>(reinterpret_cast<std::byte *>(storage.data()) + 8); }
};
struct BorrowedCosmeticRecipe {
    CosmeticArray<std::uint32_t> scalars, mask{0};
    CosmeticArray<NativeCosmeticItem> items;
    std::vector<CosmeticArray<std::uint32_t>> parameters;
    NativeCosmeticRecipe value;
    // Asset strings borrow the immutable packet only until the native copy returns.
    explicit BorrowedCosmeticRecipe(const CosmeticRecipe &r)
        : scalars(r.scalars.size()), items(r.items.size()) {
        std::copy(r.scalars.begin(), r.scalars.end(), scalars.data());
        parameters.reserve(r.items.size());
        for (std::size_t i = 0; i < r.items.size(); ++i) {
            const auto &item = r.items[i];
            parameters.emplace_back(item.parameters.size());
            std::copy(item.parameters.begin(), item.parameters.end(), parameters.back().data());
            items.data()[i] = {item.asset.c_str(), parameters.back().data(), cosmetic_asset_hash(item.asset),
                               item.slot};
        }
        value = {scalars.data(), items.data(), mask.data()};
    }
    BorrowedCosmeticRecipe(const BorrowedCosmeticRecipe &) = delete;
    BorrowedCosmeticRecipe &operator=(const BorrowedCosmeticRecipe &) = delete;
};

template <class Read> struct CosmeticMemory {
    Read read;
    std::uintptr_t base;
    template <class T> T get(std::uintptr_t p, std::size_t off = 0) const {
        return native_pose_detail::value<T>(read, native_pose_detail::add(p, off), "cosmetics");
    }
    std::uintptr_t ptr(std::uintptr_t p, std::size_t off = 0) const { return get<std::uintptr_t>(p, off); }
    static void check(bool ok, const char *reason) { native_pose_detail::require(ok, reason); }
    std::size_t count(std::uintptr_t p, std::size_t stride, std::size_t limit) const {
        check(p >= 0x10008, "Cosmetic array is unavailable.");
        const auto n = get<std::uint32_t>(p - 4) & 0x7fffffff;
        check(n <= limit && p <= 0x00007fffffffffffULL - n * stride, "Cosmetic array exceeds bounds.");
        return n;
    }
    template <class T> std::vector<T> array(std::uintptr_t p, std::size_t limit) const {
        std::vector<T> out(count(p, sizeof(T), limit));
        check(out.empty() || read(p, out.data(), out.size() * sizeof(T)), "Cosmetic array cannot be read.");
        return out;
    }
    std::string text(std::uintptr_t p) const {
        std::string out;
        // One read per chunk, not per character. A chunk that runs past readable
        // memory after a short name is read again byte by byte, up to its terminator.
        std::array<unsigned char, 64> chunk{};
        for (std::size_t at = 0; at <= max_cosmetic_asset; at += chunk.size()) {
            const auto size = std::min(chunk.size(), max_cosmetic_asset + 1 - at);
            const bool whole = read(native_pose_detail::add(p, static_cast<std::int64_t>(at)), chunk.data(), size);
            for (std::size_t i = 0; i < size; ++i) {
                const auto c = whole ? chunk[i] : get<unsigned char>(p, at + i);
                if (!c)
                    return out;
                check(c >= 32 && c != 127, "Cosmetic asset name is invalid.");
                out += static_cast<char>(c);
            }
        }
        throw std::runtime_error("Cosmetic asset name exceeds bounds.");
    }
    std::uintptr_t component(std::uintptr_t entity) const {
        return read_native_component(read, entity, base + addr::native_cosmetics::cosmetic_component_vtable);
    }
    std::uintptr_t resource(std::uintptr_t component) const {
        return ptr(ptr(component, 8), 0x90) & ~std::uintptr_t{4};
    }
    struct Item {
        std::string name;
        std::vector<std::uint32_t> categories;
    };
    // Catalog entries already found by this capture or apply: substitute_missing and
    // validate check the same assets. Only for the lifetime of this object.
    mutable std::map<std::uint32_t, Item> known{};
    Item installed(std::uint32_t hash) const {
        if (const auto found = known.find(hash); found != known.end())
            return found->second;
        auto item = lookup(hash);
        known.emplace(hash, item);
        return item;
    }
    Item lookup(std::uint32_t hash) const {
        const auto manager = ptr(base, addr::engine::cosmetics_manager);
        check(ptr(manager) == base + addr::engine::cosmetics_manager_vtable && get<std::uint8_t>(manager, 0xa8) == 1,
              "Cosmetic catalog is not ready.");
        const auto buckets = ptr(manager, 0x30);
        const auto size = get<std::uint32_t>(manager, 0x38), total = get<std::uint32_t>(manager, 0x3c);
        check(size && size <= 16384 && total && total <= 8192, "Cosmetic catalog exceeds bounds.");
        auto node = ptr(buckets, (hash % size) * 8ULL);
        for (std::uint32_t visited = 0; node && visited < total; ++visited, node = ptr(node, 0x10)) {
            if (get<std::uint32_t>(node) != hash)
                continue;
            const auto asset = ptr(node, 8) & ~std::uintptr_t{4};
            Item out{text(ptr(asset, 0x38)), {}};
            check(!out.name.empty() && get<std::uint32_t>(asset, 0x48) == hash &&
                      cosmetic_asset_hash(out.name) == hash,
                  "Cosmetic catalog asset differs.");
            for (auto category : array<std::uintptr_t>(ptr(asset, 0x20), 32))
                out.categories.push_back(get<std::uint32_t>(category & ~std::uintptr_t{4}, 0x38));
            return out;
        }
        throw std::runtime_error("Cosmetic asset is not installed.");
    }
    CosmeticRecipe capture(std::uintptr_t entity) const {
        const auto c = component(entity), res = resource(c);
        check(!get<std::uint8_t>(c, 0x10a) && !get<std::uint8_t>(c, 0x10d),
              "Waiting for the local cosmetic recipe to finish updating.");
        CosmeticRecipe out{get<std::uint32_t>(res, 0x38), get<std::uint32_t>(res, 0x3c)};
        out.scalars = array<std::uint32_t>(ptr(c, 0x150), max_cosmetic_scalars);
        for (const auto &item : array<NativeCosmeticItem>(ptr(c, 0x158), max_cosmetic_slots)) {
            CosmeticSlot slot{item.slot, text(reinterpret_cast<std::uintptr_t>(item.asset)),
                              array<std::uint32_t>(reinterpret_cast<std::uintptr_t>(item.parameters),
                                                   max_cosmetic_parameters)};
            // Active model recipes usually contain only hashes. Resolve those
            // against the local catalog; never transmit addresses or empty aliases.
            if (item.hash && slot.asset.empty())
                slot.asset = installed(item.hash).name;
            check(cosmetic_asset_hash(slot.asset) == item.hash, "Local cosmetic asset hash differs.");
            out.items.push_back(std::move(slot));
        }
        validate(res, out);
        return out;
    }
    // Whether this PC can show `item` in template slot `index`: its asset is in
    // the local catalog and fits the slot. A player's cosmetics mod can add
    // items other players do not have.
    bool item_installed(std::uintptr_t res, std::size_t index, const CosmeticSlot &item) const {
        if (item.asset.empty()) return true;
        try {
            const auto slots = ptr(res, 0x28);
            if (index >= count(slots, 24, max_cosmetic_slots)) return false;
            const auto slot = ptr(slots, index * 24 + 8) & ~std::uintptr_t{4};
            const auto info = installed(cosmetic_asset_hash(item.asset));
            const auto category = get<std::uint32_t>(ptr(slot, 0x30) & ~std::uintptr_t{4}, 0x38);
            return info.name == item.asset &&
                   std::find(info.categories.begin(), info.categories.end(), category) != info.categories.end();
        } catch (const std::exception &) {
            return false;
        }
    }
    // Swaps each item this PC does not have for the first of `fallbacks` that
    // fits its slot, or leaves the slot empty, so the rest of a peer's outfit
    // still applies. The slot keeps the sender's parameters: the slot, not the
    // item, decides their layout. Returns each swap as {missing, replacement}.
    template <class Fallbacks, class Blocked = bool (*)(const std::string &)>
    std::vector<std::pair<std::string, std::string>> substitute_missing(
        std::uintptr_t res, CosmeticRecipe &r, const Fallbacks &fallbacks,
        const Blocked &blocked = [](const std::string &) { return false; }) const {
        std::vector<std::pair<std::string, std::string>> swaps;
        for (std::size_t i = 0; i < r.items.size(); ++i) {
            auto &item = r.items[i];
            if (item_installed(res, i, item) && (item.asset.empty() || !blocked(item.asset))) continue;
            CosmeticSlot replacement{item.slot, {}, item.parameters};
            for (const auto &candidate : fallbacks) {
                CosmeticSlot fallback{item.slot, std::string(candidate), item.parameters};
                if (item_installed(res, i, fallback) && !blocked(fallback.asset)) {
                    replacement = std::move(fallback);
                    break;
                }
            }
            swaps.emplace_back(item.asset, replacement.asset);
            item = std::move(replacement);
        }
        return swaps;
    }
    // A mod can add slots to the skater or the board (stickers, say), and then two players'
    // templates hold different slots. Puts a peer's items in this game's slots by slot hash:
    // a slot the peer does not have stays empty, and an item in a slot this game does not
    // have is left out. `own(slot, count)` gives the parameters for a slot the peer sent none
    // for (or a different number of). Returns {slots left empty, items left out}; a recipe
    // that already has this game's slots in order is not touched.
    template <class Parameters>
    std::pair<std::size_t, std::size_t> fit_slots(std::uintptr_t res, CosmeticRecipe &r, const Parameters &own) const {
        const auto slots = ptr(res, 0x28);
        const auto wanted = count(slots, 24, max_cosmetic_slots);
        std::vector<std::uint32_t> ids(wanted);
        std::vector<std::size_t> sizes(wanted);
        bool same = r.items.size() == wanted;
        for (std::size_t i = 0; i < wanted; ++i) {
            const auto slot = ptr(slots, i * 24 + 8) & ~std::uintptr_t{4};
            ids[i] = get<std::uint32_t>(slot, 0x4c);
            sizes[i] = count(ptr(slot, 0x18), 4, max_cosmetic_parameters);
            same = same && r.items[i].slot == ids[i] && r.items[i].parameters.size() == sizes[i];
        }
        if (same) return {};
        std::vector<bool> used(r.items.size());
        std::vector<CosmeticSlot> fitted;
        fitted.reserve(wanted);
        std::size_t empty{};
        for (std::size_t i = 0; i < wanted; ++i) {
            std::size_t from = 0;
            while (from < r.items.size() && (used[from] || r.items[from].slot != ids[i])) ++from;
            if (from == r.items.size()) {
                ++empty;
                fitted.push_back({ids[i], {}, own(ids[i], sizes[i])});
                continue;
            }
            used[from] = true;
            fitted.push_back(std::move(r.items[from]));
            if (fitted.back().parameters.size() != sizes[i]) fitted.back().parameters = own(ids[i], sizes[i]);
        }
        const auto left_out = static_cast<std::size_t>(std::count(used.begin(), used.end(), false));
        r.items = std::move(fitted);
        return {empty, left_out};
    }
    void validate(std::uintptr_t res, const CosmeticRecipe &r) const {
        check(r.key == get<std::uint32_t>(res, 0x38) && r.version == get<std::uint32_t>(res, 0x3c),
              "Peer cosmetic template differs from this game.");
        const auto slots = ptr(res, 0x28);
        check(r.scalars.size() == count(ptr(res, 0x30), 16, max_cosmetic_scalars) &&
                  r.items.size() == count(slots, 24, max_cosmetic_slots),
              "Peer cosmetic recipe shape differs.");
        for (std::size_t i = 0; i < r.items.size(); ++i) {
            const auto slot = ptr(slots, i * 24 + 8) & ~std::uintptr_t{4};
            const auto &item = r.items[i];
            check(ptr(slot, 8) == base + addr::engine::cosmetic_slot_type &&
                      item.slot == get<std::uint32_t>(slot, 0x4c) &&
                      item.parameters.size() == count(ptr(slot, 0x18), 4, max_cosmetic_parameters),
                  "Peer cosmetic slot or parameter layout differs.");
            if (item.asset.empty())
                continue;
            const auto info = installed(cosmetic_asset_hash(item.asset));
            const auto category = get<std::uint32_t>(ptr(slot, 0x30) & ~std::uintptr_t{4}, 0x38);
            check(info.name == item.asset && std::find(info.categories.begin(), info.categories.end(),
                                                       category) != info.categories.end(),
                  "Peer cosmetic item does not match its slot category.");
        }
    }
};
} // namespace dingosdk::multiplayer
