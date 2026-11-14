// What a mod costs in video memory (Engine/Vfs/mod_vram.h), as the launcher's mod manager says it.
//
// Arguments: [<Skate folder> <mod folder>...]. Without them only the sums and the kept line are
// tested. With them each mod is read and its figures printed, to look at beside the mod's size on
// disk: a mod that reads with a problem, or a map that costs nothing, fails.
#include "Engine/Vfs/mod_vram.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace dingosdk;
using namespace dingosdk::mods;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

void sums() {
    // A 1024 x 1024 BC1 texture: 512 KB, then a quarter of that each mip down.
    frostbite::TextureHeader header;
    header.width = header.height = 1024;
    header.mip_count = 4;
    header.mip_sizes = {524288, 131072, 32768, 8192};
    auto bytes = texture_bytes(header);
    check(bytes.whole == 696320 && bytes.resident == 696320, "a texture that keeps every mip costs them all, always");
    header.first_mip = 2;   // the two sharpest stream
    bytes = texture_bytes(header);
    check(bytes.whole == 696320 && bytes.resident == 40960, "a streamed texture keeps only its tail loaded");
    header.mip_sizes[7] = 999;   // past its mip count: not its own
    check(texture_bytes(header).whole == 696320, "sizes past the mip count are not counted");
    header.first_mip = 9;        // a header gone wrong must not count more than there is
    check(texture_bytes(header).resident == 0, "a first mip past the last keeps nothing");
    header.mip_count = 200;
    header.first_mip = 0;
    check(texture_bytes(header).whole == 696320 + 999, "a mip count past the table reads the table only");

    check(on_demand_bundle("win32/characters/customization/cust_tops_bundlereftable"), "an item kind's bundle loads on demand");
    check(!on_demand_bundle("win32/cas_main_sharedbundle"), "the shared bundle is always loaded");
}

void lines() {
    ModVram vram;
    vram.level = 2'300'000'000ull;
    vram.level_resident = 410'000'000;
    vram.always = 12345;
    vram.on_demand = 6'000'000'000ull;
    vram.on_demand_largest = 90'000'000;
    vram.on_demand_bundles = 14;
    vram.textures = 4321;
    vram.meshes = 987;
    vram.unread = 2;
    ModVram back;
    check(parse_vram_line(vram_line(vram), back) && back == vram, "a mod's figures come back from the line kept for them");
    check(vram.most() == vram.level + vram.always + vram.on_demand, "the most a mod takes is its level and all its game data");
    ModVram untouched;
    untouched.always = 7;
    for (const auto* bad : {"", "v1", "v1 1 2 3", "v0 1 2 3 4 5 6 7 8 9", "v1 1 2 3 4 5 6 7 8 9 10", "v1 1 2 3 4 5 6 7 8 x",
                            "v1 1 2 3 4 5 99999999999 7 8 9", "v1  1 2 3 4 5 6 7 8 9"})
        check(!parse_vram_line(bad, untouched) && untouched.always == 7, std::string("a line that is not one is refused: \"") + bad + "\"");
}

std::string megabytes(std::uint64_t bytes) {
    return std::to_string((bytes + (1u << 19)) >> 20) + " MB";
}

void real_mod(const fs::path& game, const fs::path& folder) {
    const auto name = folder.filename().string();
    const auto start = std::chrono::steady_clock::now();
    const auto vram = read_mod_vram(game, folder);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    check(vram.problem.empty(), name + ": it is read (" + vram.problem + ")");
    check(vram.level_resident <= vram.level, name + ": what stays loaded of a level is not more than the level");
    check(vram.on_demand_largest <= vram.on_demand, name + ": the heaviest bundle is not more than all of them");
    std::error_code error;
    if (fs::is_regular_file(folder / L"reskate-levels.json", error))
        check(vram.level > 0, name + ": a map costs something");
    std::cout << "  " << name << ": level " << megabytes(vram.level) << " (" << megabytes(vram.level_resident) << " stays), always "
              << megabytes(vram.always) << ", on demand " << megabytes(vram.on_demand) << " in " << vram.on_demand_bundles
              << " bundle(s) (largest " << megabytes(vram.on_demand_largest) << "), " << vram.textures << " texture(s), " << vram.meshes
              << " mesh(es), " << vram.unread << " unread, in " << ms << " ms\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    sums();
    lines();
    if (argc > 2 && argv[1][0]) {
        std::cout << "real mods:\n";
        for (int at = 2; at < argc; ++at) real_mod(argv[1], argv[at]);
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mod vram: all checks passed\n";
    return 0;
}
