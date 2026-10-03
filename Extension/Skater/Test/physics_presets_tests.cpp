// Physics presets: names, saving, loading and refusing what is not a preset.
//   dingosdk_physics_presets_tests
#include "Extension/Skater/physics_presets.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
using namespace dingosdk::physics_tuning::presets;
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    namespace fs = std::filesystem;
    check(valid_name("Floaty") && valid_name("Heavy 2-b_c") && valid_name("a"), "ordinary names are valid");
    check(!valid_name("") && !valid_name(" lead") && !valid_name("trail ") && !valid_name("a/b") && !valid_name("..") &&
          !valid_name("c:x") && !valid_name("dot.json") && !valid_name(std::string(41, 'a')), "bad names are refused");

    const auto directory = fs::temp_directory_path() /
        ("reskate-presets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(list(directory).empty(), "a missing folder lists nothing");
    check(!load(directory, "Nope"), "a missing preset does not load");

    const Values values{{"PhysicsPush.MaxPushableSpeed", 11.5f}, {"PhysicsJump.VerticalSpeed", 2.25f},
                        {"PhysicsWheels.UseWheelDrives", 1.0f}};
    check(save(directory, "Floaty", values), "a preset saves");
    check(save(directory, "Heavy", {}), "an empty preset saves");
    check(!save(directory, "bad/name", values), "an invalid name does not save");
    check(list(directory) == std::vector<std::string>({"Floaty", "Heavy"}), "presets are listed by name");
    const auto loaded = load(directory, "Floaty");
    check(loaded && *loaded == values, "a preset loads back exactly");
    check(save(directory, "Floaty", {{"PhysicsPush.MaxPushableSpeed", 3.0f}}), "a preset is replaced");
    const auto replaced = load(directory, "Floaty");
    check(replaced && replaced->size() == 1 && replaced->at("PhysicsPush.MaxPushableSpeed") == 3.0f, "the replacement loads");

    {
        std::ofstream(directory / "Junk.json") << "{ not json";
        std::ofstream(directory / "Other.json") << R"({"format":"something else","values":{"A.b":1}})";
        std::ofstream(directory / "Mixed.json") << R"({"values":{"A.ok":1.5,"A.text":"x","A.huge":1e30,"A.neg":-2}})";
    }
    check(!load(directory, "Junk"), "broken JSON is refused");
    check(!load(directory, "Other"), "another file format is refused");
    const auto mixed = load(directory, "Mixed");
    check(mixed && mixed->size() == 2 && mixed->at("A.ok") == 1.5f && mixed->at("A.neg") == -2.0f,
          "only finite, sane numbers are kept");

    check(remove(directory, "Floaty") && !remove(directory, "Floaty"), "a preset is deleted once");
    std::error_code error;
    fs::remove_all(directory, error);
    if (failures) return 1;
    std::cout << "physics presets: ok\n";
    return 0;
}
