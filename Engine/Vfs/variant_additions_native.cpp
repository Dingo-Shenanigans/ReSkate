#include "variant_additions_native.h"
#include "variant_additions_plan.h"
#include "mod_merge_internal.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/cas_codec.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
std::vector<std::byte> bounded_toc(const fs::path& path) {
    if (fs::file_size(path) > (64u << 20)) throw std::runtime_error("variant TOC exceeds 64 MiB");
    return read_file(path);
}
std::string hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto value : bytes) {
        const auto b = std::to_integer<unsigned>(value);
        result += digits[b >> 4]; result += digits[b & 15];
    }
    return result;
}
std::string key(const fb::BundleAsset& asset) {
    return std::to_string(static_cast<int>(asset.kind)) + ':' +
        (asset.kind == fb::AssetKind::chunk ? asset.guid.string() : lower(asset.name));
}
std::vector<VariantAsset> inventory(const fs::path& game, const fs::path& root, const fb::TocBundle& bundle) {
    const auto layout = vfs::read_layout(root / "layout.toc");
    CasStore store(game / "Data", {}, layout.root);
    const auto listing = list_bundle(store, root, game / "Data", bundle, game);
    if (!listing) throw std::runtime_error("unreadable variant bundle manifest");
    const auto& manifest = listing->manifest;
    const auto count = manifest.ebx.size() + manifest.resources.size() + manifest.chunks.size();
    if (count > 100000 || listing->first > listing->files.size() || count != listing->files.size() - listing->first)
        throw std::runtime_error("variant asset count is invalid");
    std::vector<VariantAsset> result;
    std::size_t at{}, decoded{};
    for (const auto* list : {&manifest.ebx, &manifest.resources, &manifest.chunks}) for (const auto& asset : *list) {
        const auto& file = listing->files.at(listing->first + at);
        const auto size = store.archive_size(file.location.patch ? root : game / "Data", file.location);
        if (!size || std::uint64_t{file.offset} + file.size > *size)
            throw std::runtime_error("variant asset lies outside its archive");
        VariantAsset row;
        row.name = key(asset);
        const auto kind = std::to_string(static_cast<int>(asset.kind)) + ':';
        row.identity = kind + (asset.kind == fb::AssetKind::resource ? std::to_string(asset.resourceId) : asset.guid.string());
        row.fingerprint = hex(asset.sha1.bytes) + ':' + std::to_string(asset.resourceType) + ':' + hex(asset.resourceMeta) +
            ':' + std::to_string(asset.originalSize) + ':' + std::to_string(asset.logicalOffset) + ':' + std::to_string(asset.logicalSize);
        if (asset.kind == fb::AssetKind::ebx) {
            if (asset.originalSize > (64u << 20) || file.size > (64u << 20) || decoded + asset.originalSize > (256u << 20))
                throw std::runtime_error("variant EBX decoding exceeds its bound");
            const auto bytes = fb::decode_cas(store.read(file.location.patch ? root : game / "Data", file.location,
                                                        file.offset, file.size),
                {.gameRoot = game, .maximumOutputSize = std::min<std::size_t>(64u << 20, (256u << 20) - decoded)});
            decoded += bytes.size();
            if (bytes.size() != asset.originalSize) throw std::runtime_error("variant EBX size differs from its manifest");
            const auto document = fb::ebx::read_document(bytes);
            row.identity = kind + document.fileGuid.string();
            for (const auto& reference : document.imports) row.dependencies.push_back("0:" + reference.fileGuid.string());
            for (const auto offset : document.resourceRefOffsets) {
                if (offset > document.payload.size() || document.payload.size() - offset < 8)
                    throw std::runtime_error("invalid native resource reference");
                std::uint64_t id{}; std::memcpy(&id, document.payload.data() + offset, 8);
                if (id) row.dependencies.push_back("1:" + std::to_string(id));
            }
        }
        result.push_back(std::move(row)); ++at;
    }
    return result;
}
const fb::TocBundle& named(const fb::TocDocument& toc, const std::string& name) {
    const auto at = std::ranges::find_if(toc.bundles, [&](const auto& b) { return lower(b.name) == name; });
    if (at == toc.bundles.end()) throw std::runtime_error("selected variant or game does not deliver the requested bundle");
    return *at;
}
}

std::map<std::string, std::set<std::string>, std::less<>> admit_variant_additions(
    const fs::path& game, const Mod& owner, const Mod& selected, const Mod& donor, std::span<const ModEntry> others) {
    if (!owner.variants || !donor.outdated.empty() || !selected.outdated.empty())
        throw std::runtime_error("variant has no current native delivery contract");
    std::map<std::string, std::set<std::string>, std::less<>> result;
    for (const auto& file : fs::directory_iterator(donor.directory / "Win32")) {
        if (!file.is_regular_file() || file.path().extension() != ".toc") continue;
        const auto toc = fb::read_toc(bounded_toc(file.path()));
        for (const auto& bundle : toc.bundles) {
            const auto name = lower(bundle.name);
            if (std::ranges::none_of(owner.variants->displaces, [&](const auto& part) { return name.find(part) != std::string::npos; })) continue;
            if (std::ranges::find(owner.variant_additions.accept, name) == owner.variant_additions.accept.end() ||
                std::ranges::find(donor.variant_additions.contribute, name) == donor.variant_additions.contribute.end())
                throw std::runtime_error("both authors must permit this exact bundle");
            const auto relative = fs::path("Win32") / file.path().filename();
            const auto base = fb::read_toc(bounded_toc(game / "Data" / relative));
            const auto choice = fb::read_toc(bounded_toc(selected.directory / relative));
            const auto stock = inventory(game, game / "Data", named(base, name));
            const auto retained = inventory(game, selected.directory, named(choice, name));
            const auto incoming = inventory(game, donor.directory, bundle);
            const auto plan = plan_variant_additions(true, true, stock, retained, incoming, {});
            if (!plan) throw std::runtime_error(plan.refusal);
            if (plan.additions.empty()) throw std::runtime_error("bundle has no private additions");
            // Reserve additions of every other prospective contributor too: two
            // packs must not become compatible merely by swapping load order.
            for (const auto& other : others) {
                if (!other.enabled || other.mod.directory == donor.directory || other.mod.directory == selected.directory ||
                    other.mod.directory == owner.directory || variant_owner(other.mod.name) == owner.name) continue;
                const auto path = other.mod.directory / relative;
                if (!fs::exists(path)) continue;
                std::vector<VariantAsset> reserved;
                try {
                    const auto other_toc = fb::read_toc(bounded_toc(path));
                    const auto found = std::ranges::find_if(other_toc.bundles, [&](const auto& b) { return lower(b.name) == name; });
                    if (found == other_toc.bundles.end()) continue;
                    reserved = inventory(game, other.mod.directory, *found);
                } catch (const std::exception& failure) {
                    throw std::runtime_error("enabled pack " + other.mod.name + " blocks admission: " + failure.what());
                }
                std::set<std::string> names, identities;
                for (const auto& asset : reserved) { names.insert(asset.name); identities.insert(asset.identity); }
                for (const auto index : plan.additions)
                    if (names.contains(incoming[index].name) || identities.contains(incoming[index].identity))
                        throw std::runtime_error("private addition collides with another enabled pack");
            }
            auto& keys = result[name];
            for (const auto index : plan.additions) keys.insert(incoming[index].name);
        }
    }
    if (result.empty()) throw std::runtime_error("no displaced native bundle was proved additive");
    return result;
}
}
