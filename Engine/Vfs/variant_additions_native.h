#pragma once
#include "mod_list.h"
#include <span>

namespace dingosdk::mods::detail {
// Throws on unreadable or unproved input. The caller keeps strict displacement
// in that case. No source files or archive payloads are changed here.
std::map<std::string, std::set<std::string>, std::less<>> admit_variant_additions(
    const std::filesystem::path& game, const Mod& owner, const Mod& selected,
    const Mod& donor, std::span<const ModEntry> others);
}
