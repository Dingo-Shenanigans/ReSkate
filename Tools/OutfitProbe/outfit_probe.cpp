// outfit-probe: what Skate Synchros would send for one worn item, for every item the enabled mods
// provide. It runs ReSkate's own item cutter (Engine/Vfs/synchros_closure.h) over the mods in
// mods.json's order, so an item that uses another mod's files (a deck with a shader library's
// shaders) counts them, and reports per mod. Reads only; writes a report.
//
//   outfit-probe.exe [game folder] [report file]
//
// The game folder (with Skate.exe, Data and Mods) defaults to the one above this program.
//
// Then a round trip for one item of each mod, as a viewer would get it: the item's files as sent,
// written into an outfit mod (Engine/Vfs/synchros_outfit_mod.h) in a folder beside this program,
// and that mod read back the way the merge reads a mod.
#include "Engine/Vfs/synchros_closure.h"
#include "Engine/Vfs/synchros_outfit_mod.h"
#include "Engine/Vfs/game_archives.h"
#include "Engine/Resource/bundle_ref_table.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/toc.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace syn = dingosdk::synchros;

namespace {
std::ofstream report;
void say(const std::string& line) {
    report << line << '\n';
    report.flush();
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}
std::string mb(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

// The enabled mods, highest priority first: mods.json's rows, then folders it does not list.
std::vector<syn::Source> enabled_mods(const fs::path& mods) {
    std::vector<syn::Source> out;
    std::set<std::string> listed;
    std::ifstream in(mods / "mods.json");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    rapidjson::Document json;
    if (!text.empty() && !json.Parse(text.c_str()).HasParseError() && json.IsObject() && json.HasMember("mods") && json["mods"].IsArray())
        for (const auto& row : json["mods"].GetArray()) {
            if (!row.IsObject() || !row.HasMember("name") || !row["name"].IsString()) continue;
            const std::string name = row["name"].GetString();
            listed.insert(name);
            const bool on = !row.HasMember("enabled") || !row["enabled"].IsBool() || row["enabled"].GetBool();
            if (on && fs::is_directory(mods / name)) out.push_back({mods / name, name});
        }
    std::vector<std::string> rest;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(mods, error)) {
        const auto name = entry.path().filename().string();
        if (entry.is_directory() && !name.empty() && name.front() != '.' && !listed.contains(name)) rest.push_back(name);
    }
    std::sort(rest.begin(), rest.end());
    for (const auto& name : rest) out.push_back({mods / name, name});
    return out;
}
std::vector<std::byte> file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::byte> out(text.size());
    if (!text.empty()) std::memcpy(out.data(), text.data(), text.size());
    return out;
}

// One item as a viewer receives it, written into an outfit mod and read back.
std::string round_trip(syn::Index& index, const std::string& item, const fs::path& game, const fs::path& folder) {
    namespace fb = dingosdk::frostbite;
    const auto cut = index.cut(item);
    if (!cut) return "no cut";
    std::map<std::array<std::uint8_t, 20>, std::vector<std::uint8_t>> cache;
    syn::OutfitItem received{item, {}, {}};
    for (const auto& piece : cut->pieces) {
        const auto stored = index.read(piece);
        syn::OutfitFile f;
        f.id = syn::sha1(stored);
        f.size = static_cast<std::uint32_t>(stored.size());
        f.kind = static_cast<syn::OutfitFile::Kind>(piece.kind);
        f.name = piece.name;
        f.bundle = piece.bundle;
        f.superbundle = piece.superbundle;
        f.original_size = piece.asset.originalSize;
        f.resource_type = piece.asset.resourceType;
        f.resource_id = piece.asset.resourceId;
        f.resource_meta.resize(piece.asset.resourceMeta.size());
        if (!f.resource_meta.empty()) std::memcpy(f.resource_meta.data(), piece.asset.resourceMeta.data(), f.resource_meta.size());
        std::memcpy(f.chunk_guid.data(), piece.asset.guid.bytes.data(), 16);
        f.logical_offset = piece.asset.logicalOffset;
        f.logical_size = piece.asset.logicalSize;
        std::memcpy(f.sha1.data(), piece.asset.sha1.bytes.data(), 20);
        f.chunk_record.resize(piece.chunk_record.size());
        if (!f.chunk_record.empty()) std::memcpy(f.chunk_record.data(), piece.chunk_record.data(), f.chunk_record.size());
        std::vector<std::uint8_t> bytes(stored.size());
        if (!bytes.empty()) std::memcpy(bytes.data(), stored.data(), bytes.size());
        cache.emplace(f.id, std::move(bytes));
        received.files.push_back(std::move(f));
    }
    for (const auto& row : cut->rows) received.rows.push_back({row.table, row.path, row.bundle, row.table_bundle, row.table_superbundle});
    const auto written = syn::write_outfit_mod(folder, 76561198000000001ULL, game, {received}, [&](const std::array<std::uint8_t, 20>& id)
        -> std::optional<std::vector<std::uint8_t>> {
        const auto f = cache.find(id);
        if (f == cache.end()) return std::nullopt;
        return f->second;
    });
    if (!written.written) return "not written: " + written.problem;

    // Read back as the merge reads a mod: each TOC, each bundle's manifest, each received payload.
    const auto data = game / "Data";
    const auto layout = dingosdk::vfs::read_layout(data / "layout.toc");
    dingosdk::vfs::GameArchives archives(data, layout.root);
    std::size_t bundles = 0, checked = 0, presets = 0;
    std::set<std::string> want_presets;
    for (const auto& row : cut->rows) want_presets.insert(row.path);
    for (const auto* toc_name : {"items.toc", "globals.toc"}) {
        const auto path = folder / "Win32" / toc_name;
        if (!fs::exists(path)) continue;
        const auto toc = fb::read_toc(file_bytes(path));
        for (const auto& bundle : toc.bundles) {
            ++bundles;
            const auto region = fb::read_bundle_region(bundle.region);
            if (region.files.empty()) return bundle.name + ": empty region";
            const auto& first = region.files.front();
            if (!first.location.patch) return bundle.name + ": manifest not in the mod";
            const auto manifest = archives.read_manifest(folder, first.location, first.offset, first.size, game);
            const auto assets = manifest.ebx.size() + manifest.resources.size() + manifest.chunks.size();
            if (assets + 1 != region.files.size()) return bundle.name + ": manifest lists " + std::to_string(assets) + " assets, region has " +
                                                         std::to_string(region.files.size() - 1);
            std::size_t at = 1;
            for (const auto* list : {&manifest.ebx, &manifest.resources, &manifest.chunks})
                for (const auto& asset : *list) {
                    const auto& file = region.files[at++];
                    if (!file.location.patch) continue;
                    const auto raw = archives.read(folder, file.location, file.offset, file.size);
                    const auto decoded = fb::decode_cas(raw, {game});
                    if (list != &manifest.chunks && decoded.size() != asset.originalSize)
                        return asset.name + ": decodes to " + std::to_string(decoded.size()) + " bytes, manifest says " + std::to_string(asset.originalSize);
                    if (list == &manifest.resources && fb::bundle_ref::is_table(asset.name)) {
                        const auto held = fb::bundle_ref::presets({decoded, asset.resourceMeta});
                        for (const auto& p : want_presets) if (held.contains(p)) ++presets;
                    }
                    ++checked;
                }
        }
    }
    if (presets < want_presets.size())
        return "only " + std::to_string(presets) + " of " + std::to_string(want_presets.size()) + " presets are in the table copy";
    return "ok: " + std::to_string(bundles) + " bundles, " + std::to_string(checked) + " payloads read back, " +
           std::to_string(presets) + " presets in the table, " + mb(written.bytes) + " written";
}
} // namespace

int main(int argc, char** argv) {
    const fs::path self = fs::absolute(argv[0]).parent_path();
    const fs::path game = argc > 1 ? fs::path(argv[1]) : self.parent_path();
    const fs::path out = argc > 2 ? fs::path(argv[2]) : self / "outfit-probe-report.txt";
    report.open(out);
    say("outfit-probe 7: game " + game.string());
    const auto sources = enabled_mods(game / "Mods");
    say(std::to_string(sources.size()) + " enabled mods; reading their cosmetics...");
    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<syn::Index> index;
    try {
        index = std::make_unique<syn::Index>(sources, game);
    } catch (const std::exception& e) {
        say(std::string("Could not read the mods: ") + e.what());
        return 1;
    }
    const auto items = index->items();
    say(std::to_string(items.size()) + " items in " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) +
        " ms" + (index->unreadable() ? ", " + std::to_string(index->unreadable()) + " files unreadable" : ""));

    const auto [table_copies, table_presets] = index->tables();
    say(std::to_string(table_copies) + " bundle-reference table copies, " + std::to_string(table_presets) + " presets in them");
    struct Row { std::string item; std::uint64_t bytes{}, borrowed{}; std::size_t files{}, rows{}; std::set<std::string> lenders; };
    std::map<std::size_t, std::vector<Row>> by_mod;
    for (const auto& item : items) {
        const auto cut = index->cut(item);
        if (!cut || cut->pieces.empty()) continue;
        const auto home = cut->pieces.front().source;
        Row row{item, cut->bytes, 0, cut->pieces.size(), cut->rows.size(), {}};
        for (const auto& piece : cut->pieces)
            if (piece.source != home) {
                row.borrowed += piece.file.size;
                row.lenders.insert(index->sources()[piece.source].name);
            }
        by_mod[home].push_back(std::move(row));
    }
    for (auto& [source, rows] : by_mod) {
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
        std::uint64_t total = 0, borrowed = 0;
        for (const auto& r : rows) { total += r.bytes; borrowed += r.borrowed; }
        say("== " + index->sources()[source].name + ": " + std::to_string(rows.size()) + " items");
        say("   per worn item: largest " + mb(rows.front().bytes) + ", median " + mb(rows[rows.size() / 2].bytes) + ", average " +
            mb(total / rows.size()) + (borrowed ? ", of which from other mods " + mb(borrowed / rows.size()) + " on average" : ""));
        for (std::size_t k = 0; k < rows.size() && k < 6; ++k) {
            std::string lenders;
            for (const auto& l : rows[k].lenders) lenders += (lenders.empty() ? "" : ", ") + l;
            say("     " + mb(rows[k].bytes) + "  " + std::to_string(rows[k].files) + " files, " + std::to_string(rows[k].rows) + " preset rows  " + rows[k].item +
                (lenders.empty() ? "" : "  (+" + mb(rows[k].borrowed) + " from " + lenders + ")"));
        }
        if (rows.size() > 6) say("     ... and " + std::to_string(rows.size() - 6) + " more");
    }
    say("== round trip: one item of each mod, as a viewer gets it");
    const auto folder = self / "round-trip-mod";
    std::size_t ok = 0, tried = 0;
    for (auto& [source, rows] : by_mod) {
        const auto& item = rows.back().item;   // the smallest, to keep it quick
        std::string verdict;
        try {
            verdict = round_trip(*index, item, game, folder);
        } catch (const std::exception& e) {
            verdict = std::string("failed: ") + e.what();
        }
        ++tried;
        if (verdict.starts_with("ok")) ++ok;
        say("   " + index->sources()[source].name + ": " + item + "  " + verdict);
    }
    std::error_code gone;
    fs::remove_all(folder, gone);
    say(std::to_string(ok) + " of " + std::to_string(tried) + " round trips ok");
    say("done. Report: " + out.string());
    return 0;
}
