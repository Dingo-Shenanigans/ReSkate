#include "plugin_discovery.h"
#include "plugin_manifest.h"
#include "Engine/Vfs/mod_list.h"
#include <algorithm>
#include <array>
#include <fstream>

namespace dingosdk::plugins {
std::vector<Candidate> discover(const std::filesystem::path& data_root, FileChecks& checks, std::vector<ModInfo>* mods) {
    std::vector<Candidate> result;
    const auto list = mods::scan_mods(data_root);
    // A mods.json the game cannot read loads no mod at all, plugins included.
    if (!list.present || !list.issue.empty()) return result;
    FileChecks seen;
    if (mods) {
        mods->clear();
        for (const auto& entry : list.entries) mods->push_back({entry.mod.name, entry.mod.title, entry.enabled});
    }
    for (const auto& entry : list.entries) {
        if (!entry.enabled) continue;
        const auto& directory = entry.mod.directory;
        std::error_code code;
        if (!std::filesystem::is_regular_file(directory / manifest_file, code)) continue;
        Candidate candidate{entry.mod.name, directory, mods::mod_fingerprint(directory), {}};
        const auto key = candidate.id + "|" + candidate.fingerprint;
        const auto known = checks.find(key);
        candidate.refusal = known != checks.end() ? known->second : check_package_files(directory);
        seen.emplace(key, candidate.refusal);
        result.push_back(std::move(candidate));
    }
    checks = std::move(seen);
    return result;
}
}
