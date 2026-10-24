#pragma once
#include "plugin_host.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Which mods are plugins: the mods mods.json leaves on (Engine/Vfs/mod_list.h) that have a
// reskate-plugin.json. Reads the disk; run it off the game thread.
namespace dingosdk::plugins {
// What check_package_files said about a package, by its fingerprint, so an unchanged package is
// not read again.
using FileChecks = std::map<std::string, std::string, std::less<>>;
// `mods`, when given, gets every mod in the Mods folder (for reskate.mods).
std::vector<Candidate> discover(const std::filesystem::path& data_root, FileChecks& checks, std::vector<ModInfo>* mods = nullptr);
}
