// The REALISTIC list's presets (trainer_presets.h): that Hardcore goes at least as far as
// Realistic on everything Realistic changes and shows on that list, and that each of their rules
// naming one of the game's data-defined classes names a field it has. A pattern that matches
// nothing is skipped without a word in game, so a misspelt one would otherwise go unnoticed.
// Only these two presets are checked: in the others a pattern such as "wipeout.x" may be meant
// for the physics tuning asset ("physicswipeout.x"), whose names need the game's data.
// And that a preset of the player's own never keeps a built-in preset's name: apply and remove
// look at the player's presets first, so it would hide the built-in.
#include "Extension/Trainer/trainer_classes.h"
#include "Extension/Trainer/trainer_presets.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace dingosdk::trainer;
int failures = 0;
void check(bool condition, const std::string &message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
std::string lower(std::string_view text) {
    std::string out(text);
    for (auto &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
const BuiltinPreset *find_preset(std::string_view name) {
    const auto &presets = builtin_presets();
    const auto found = std::ranges::find(presets, name, &BuiltinPreset::name);
    return found == presets.end() ? nullptr : &*found;
}
// The class whose key the pattern starts with ("push." for "push.maxpushspeedlight"), if any.
const ClassSpec *class_of(std::string_view pattern) {
    for (const auto &spec : class_specs)
        if (const auto key = lower(spec.key); pattern.size() > key.size() && pattern.starts_with(key) && pattern[key.size()] == '.')
            return &spec;
    return nullptr;
}
}

int main() {
    const auto *realistic = find_preset("Realistic");
    const auto *hardcore = find_preset("Hardcore");
    check(realistic != nullptr, "Realistic is a built-in preset");
    check(hardcore != nullptr, "Hardcore is a built-in preset");
    if (realistic && hardcore) {
        check((preset_dial("Hardcore").modes & mode_realistic) != 0, "Hardcore shows on the REALISTIC list");
        // Every value Realistic turns down, Hardcore turns down at least as far, so switching
        // from one to the other never makes the game easier.
        for (const auto &rule : realistic->rules) {
            const auto same = std::ranges::find(hardcore->rules, rule.pattern, &PresetRule::pattern);
            check(same != hardcore->rules.end(), "Hardcore also sets \"" + std::string(rule.pattern) + "\"");
            if (same == hardcore->rules.end()) continue;
            check(same->multiply == rule.multiply && same->curves == rule.curves && same->amount <= rule.amount,
                  "Hardcore goes at least as far as Realistic on \"" + std::string(rule.pattern) + "\"");
        }
    }
    for (const auto *preset : {realistic, hardcore}) {
        if (!preset) continue;
        for (const auto &rule : preset->rules) {
            if (rule.multiply) check(rule.amount > 0, std::string(preset->name) + ": a multiplier above 0 for \"" + std::string(rule.pattern) + "\"");
            // One word that starts with a class's key: it is meant for that class's fields.
            if (rule.curves || rule.pattern.find(' ') != std::string_view::npos) continue;
            const auto *spec = class_of(rule.pattern);
            if (!spec) continue;
            bool found = false;
            for (std::size_t i = 0; i < spec->count && !found; ++i)
                found = lower(std::string(spec->key) + "." + class_fields[spec->first + i].name).find(rule.pattern) != std::string::npos;
            check(found, std::string(preset->name) + ": \"" + std::string(rule.pattern) + "\" names a field of " + spec->key);
        }
    }

    // Names a player's preset may not have, in any case: the built-ins', Stock and Map.
    for (const auto *name : {"Hardcore", "hardcore", "HARDCORE", "Realistic", "Stock", "map"})
        check(reserved_preset_name(name), std::string("\"") + name + "\" is reserved");
    for (const auto *name : {"Hardcore (yours)", "My Hardcore", "Hardcor", ""})
        check(!reserved_preset_name(name), std::string("\"") + name + "\" is free");

    // A preset saved before a built-in took its name keeps its values under a free one.
    UserPresets user{{"Hardcore", {{"physicsmode.jumpmaxheight", 0.5}}},
                     {"hardcore (yours)", {{"push.maxpushspeedlight", 3.0}}},
                     {"STOCK", {{"jump.basejumpheight", 0.5}}},
                     {"Mine", {{"physicsmode.grindlockdist", 0.4}}}};
    const auto renamed = rename_reserved_presets(user);
    check(renamed.size() == 2, "two presets are renamed");
    const auto renamed_to = [&](std::string_view from) {
        const auto found = std::ranges::find(renamed, from, [](const auto &pair) { return std::string_view(pair.first); });
        return found == renamed.end() ? std::string{} : found->second;
    };
    check(renamed_to("Hardcore") == "Hardcore (yours 2)", "a taken new name, in any case, gets a number");
    check(renamed_to("STOCK") == "STOCK (yours)", "Stock is renamed like a built-in's name");
    check(user.size() == 4 && user.contains("Mine") && user.contains("hardcore (yours)"), "the other presets stay as they were");
    check(user.contains("Hardcore (yours 2)") && user.at("Hardcore (yours 2)").at("physicsmode.jumpmaxheight") == 0.5,
          "a renamed preset keeps its values");
    for (const auto &entry : user) check(!reserved_preset_name(entry.first), "no reserved name is left: \"" + entry.first + "\"");
    check(rename_reserved_presets(user).empty(), "renaming again changes nothing");
    // Every reserved name still fits once renamed, or the preset would be dropped on the next load.
    for (const auto &preset : builtin_presets()) {
        UserPresets one{{std::string(preset.name), {}}};
        const auto result = rename_reserved_presets(one);
        check(result.size() == 1 && result[0].second.size() <= max_preset_name, std::string(preset.name) + " fits once renamed");
    }

    if (failures) {
        std::cerr << failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "Trainer presets: all checks passed.\n";
    return 0;
}
