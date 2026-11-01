#pragma once
#include "plugin_host.h"
#include "plugin_approval.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Which mods are plugins: the mods mods.json leaves on (Engine/Vfs/mod_list.h) that have a
// reskate-plugin.json. Reads the disk; run it off the game thread.
namespace dingosdk::plugins {
// The package refusal and approved source snapshot by the mod folder's fingerprint, so an unchanged
// package is not read again. Reload can invalidate only the source snapshot without rescanning the mod.
struct FileChecks {
    std::map<std::string, std::string, std::less<>> packages;
    std::map<std::string, PluginSnapshot, std::less<>> plugins;
    void forget_plugin(const Candidate& candidate);
    void forget_plugin(std::string_view id, std::string_view mod_fingerprint);
};
// `mods`, when given, gets every mod in the Mods folder (for reskate.mods).
std::vector<Candidate> discover(const std::filesystem::path& data_root, FileChecks& checks, std::vector<ModInfo>* mods = nullptr);
}
