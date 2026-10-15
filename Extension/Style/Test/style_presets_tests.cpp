#include "Extension/Style/style_presets.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
std::string bytes_of(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
} // namespace

int style_presets_tests() {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("reskate-style-presets-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder);

    check(presets::after_delete(folder) == "default", "with no presets the next one is default");
    Style style;
    style.times[2] = {1.0f};
    style.rotations[{Target{true, 2, 0}, "Head"}] = {10, 0, 0};
    check(presets::write(folder, "street", style).empty() && presets::write(folder, "air", Style{}).empty(), "presets are written");
    std::ofstream(folder / "notes.txt") << "not a preset";
    check(presets::list(folder, "default") == std::vector<std::string>{"air", "default", "street"},
          "the list is sorted and skips other files");
    const auto back = presets::read(folder, "street");
    check(back.issue.empty() && back.style == style, "a written preset reads back the same");
    check(presets::read(folder, "missing").issue.empty() && presets::read(folder, "missing").style == Style{}, "a missing preset is empty");

    std::ofstream(folder / "broken.style.json") << "{ not json";
    const auto broken = presets::read(folder, "broken");
    check(!broken.issue.empty() && broken.copied && std::filesystem::exists(folder / "broken.style.json.unreadable"),
          "an unreadable preset is reported and kept aside");
    const std::string newer_text = R"({"format":2,"tricks":{"kickflip":[{"time":1,"lean":3}]}})";
    std::ofstream(folder / "later.style.json", std::ios::binary) << newer_text;
    const auto later = presets::read(folder, "later");
    check(later.newer && !later.copied && later.issue == "this preset is from a newer ReSkate" && later.style == Style{},
          "a preset from a newer ReSkate is not read and not copied aside");
    check(later.keep() && !broken.keep() && !back.keep(), "the session keeps a newer file, and writes over a copied or a good one");
    // The layer's save: a kept file is not written.
    if (!later.keep()) (void)presets::write(folder, "later", style);
    check(bytes_of(folder / "later.style.json") == newer_text && !std::filesystem::exists(folder / "later.style.json.unreadable"),
          "a newer preset is unchanged on disk after a save");

    using dingosdk::Json;
    check(presets::in_use_or_default(Json("street")) == "street", "the preset in use is the profile's value");
    check(presets::in_use_or_default(std::nullopt) == "default" && presets::in_use_or_default(Json(3)) == "default" &&
              presets::in_use_or_default(Json(true)) == "default" && presets::in_use_or_default(Json("CON")) == "default" &&
              presets::in_use_or_default(Json("../street")) == "default",
          "a missing value, one that is not text, or text that is not a preset name is default");
    check(presets::remove(folder, "street") && !presets::exists(folder, "street"), "a preset is deleted");
    check(presets::after_delete(folder) == "air", "after a delete the first saved preset is used");

    const auto blocked = folder / "blocked";
    std::ofstream(blocked) << "a file where the folder should be";
    check(!presets::write(blocked, "x", style).empty(), "a write that cannot happen says why");
    const auto clip = folder / "clips" / "kickflip.clip";
    const std::string clip_bytes{'R', 'S', 'C', 'L', '\x01', '\0', '\x7f'};
    auto clip_temporary = clip;
    clip_temporary += ".tmp";
    check(presets::write_atomically(clip, clip_bytes).empty() && bytes_of(clip) == clip_bytes &&
              !std::filesystem::exists(clip_temporary),
          "bytes are written whole into a new folder, and no temporary file stays");
    check(presets::write_atomically(clip, "again").empty() && bytes_of(clip) == "again", "a second write replaces the file");
    check(!presets::write_atomically(blocked / "kickflip.clip", clip_bytes).empty(), "a byte write that cannot happen says why");

    std::filesystem::remove_all(folder, ignored);
    if (!failures) std::cout << "style presets tests passed\n";
    return failures ? 1 : 0;
}
