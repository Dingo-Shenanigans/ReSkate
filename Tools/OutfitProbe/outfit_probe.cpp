// outfit-probe: what Skate Synchros would send for one worn item, for every item the enabled mods
// provide. It runs ReSkate's own item cutter (Engine/Vfs/synchros_closure.h) over the mods in
// mods.json's order, so an item that uses another mod's files (a deck with a shader library's
// shaders) counts them, and reports per mod. Reads only; writes a report.
//
//   outfit-probe.exe [game folder] [report file]
//
// The game folder (with Skate.exe, Data and Mods) defaults to the one above this program.
#include "Engine/Vfs/synchros_closure.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <chrono>
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
} // namespace

int main(int argc, char** argv) {
    const fs::path self = fs::absolute(argv[0]).parent_path();
    const fs::path game = argc > 1 ? fs::path(argv[1]) : self.parent_path();
    const fs::path out = argc > 2 ? fs::path(argv[2]) : self / "outfit-probe-report.txt";
    report.open(out);
    say("outfit-probe 4: game " + game.string());
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

    struct Row { std::string item; std::uint64_t bytes{}, borrowed{}; std::size_t files{}; std::set<std::string> lenders; };
    std::map<std::size_t, std::vector<Row>> by_mod;
    for (const auto& item : items) {
        const auto cut = index->cut(item);
        if (!cut || cut->pieces.empty()) continue;
        const auto home = cut->pieces.front().source;
        Row row{item, cut->bytes, 0, cut->pieces.size(), {}};
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
            say("     " + mb(rows[k].bytes) + "  " + std::to_string(rows[k].files) + " files  " + rows[k].item +
                (lenders.empty() ? "" : "  (+" + mb(rows[k].borrowed) + " from " + lenders + ")"));
        }
        if (rows.size() > 6) say("     ... and " + std::to_string(rows.size() - 6) + " more");
    }
    say("done. Report: " + out.string());
    return 0;
}
