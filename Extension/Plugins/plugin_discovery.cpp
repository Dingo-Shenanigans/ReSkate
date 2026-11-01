#include "plugin_discovery.h"
#include "plugin_approval.h"
#include "plugin_manifest.h"
#include "Engine/Vfs/mod_list.h"
#include <algorithm>
#include <array>
#include <fstream>

namespace dingosdk::plugins {
void FileChecks::forget_plugin(const Candidate& candidate) {
    forget_plugin(candidate.id, candidate.fingerprint);
}
void FileChecks::forget_plugin(std::string_view id, std::string_view mod_fingerprint) {
    plugins.erase(std::string(id) + "|" + std::string(mod_fingerprint));
}

std::vector<Candidate> discover(const std::filesystem::path& data_root, FileChecks& checks, std::vector<ModInfo>* mods) {
    std::vector<Candidate> result;
    const auto list = mods::scan_mods(data_root);
    // A mods.json the game cannot read loads no mod at all, plugins included.
    if (!list.present || !list.issue.empty()) return result;
    FileChecks seen;
    if (mods) {
        mods->clear();
        for (const auto& entry : list.entries) mods->push_back({entry.mod.name, entry.mod.title, entry.enabled, entry.mod.author});
    }
    for (const auto& entry : list.entries) {
        if (!entry.enabled) continue;
        const auto& directory = entry.mod.directory;
        std::error_code code;
        if (!std::filesystem::is_regular_file(directory / manifest_file, code)) continue;
        Candidate candidate{entry.mod.name, directory, mods::mod_fingerprint(directory), {}};
        const auto key = candidate.id + "|" + candidate.fingerprint;
        const auto known = checks.packages.find(key);
        auto package_refusal = known != checks.packages.end() ? known->second : check_package_files(directory);
        candidate.refusal = package_refusal;
        const auto cached = checks.plugins.find(key);
        auto snapshot = cached != checks.plugins.end() ? cached->second : plugin_snapshot(directory);
        candidate.plugin_fingerprint = snapshot.fingerprint;
        candidate.sources = snapshot.sources;
        if (!snapshot.error.empty()) {
            if (!candidate.refusal.empty()) candidate.refusal += "; ";
            candidate.refusal += "plugin source snapshot: " + snapshot.error;
        }
        seen.packages.emplace(key, std::move(package_refusal));
        seen.plugins.emplace(key, std::move(snapshot));
        result.push_back(std::move(candidate));
    }
    checks = std::move(seen);
    return result;
}
}
