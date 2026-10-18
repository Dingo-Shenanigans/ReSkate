// Runs the mod merge on a game folder outside the game, the way the launcher does at launch,
// and prints every step with the time it took and the merge's notes. For finding where a
// long merge spends its time: a full merge of a few hundred mods takes over a minute, and
// the game's log keeps only the step totals.
//
//   ReSkateMergeBench <game folder> [--notes <file>]
//
// The patch goes to <game folder>/Mods/.reskate as at launch. The stamp names a ReSkate.dll
// beside this program as the SDK that merged, so the launcher merges again on its next
// launch unless the game's ReSkate.dll is copied beside it.
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
std::string seconds(Clock::duration elapsed) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    char text[32];
    std::snprintf(text, sizeof text, "%lld.%03llds", static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000));
    return text;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ReSkateMergeBench <game folder> [--notes <file>]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    std::filesystem::path notesFile;
    for (int at = 2; at + 1 < argc; ++at)
        if (std::string(argv[at]) == "--notes") notesFile = argv[at + 1];

    const auto start = Clock::now();
    auto last = start;
    std::string lastStep;
    // Each step is reported when it begins; the time printed is the previous step's.
    const dingosdk::mods::MergeObserver observe = [&](const dingosdk::mods::MergeProgress& progress) {
        const auto now = Clock::now();
        if (!lastStep.empty())
            std::printf("%8s  %s\n", seconds(now - last).c_str(), lastStep.c_str());
        std::fflush(stdout);
        last = now;
        lastStep = std::to_string(progress.done) + "/" + std::to_string(progress.total) + "  " + progress.step;
    };

    auto catalog = dingosdk::mods::load_catalog(root, observe, false);
    if (!catalog.issue.empty()) {
        std::fprintf(stderr, "catalog: %s\n", catalog.issue.c_str());
        return 1;
    }
    std::printf("%zu enabled mod(s), %zu disabled with a layout, %s\n", catalog.mods.size(), catalog.inactive.size(),
                seconds(Clock::now() - start).c_str());
    const auto mergeStart = Clock::now();
    const auto report = dingosdk::mods::merge_mods(catalog, observe);
    const auto end = Clock::now();
    if (!lastStep.empty()) std::printf("%8s  %s\n", seconds(end - last).c_str(), lastStep.c_str());
    std::printf("\n%s for the merge: built=%d reused=%d superbundles=%zu mergedBundles=%zu mergedAssets=%zu archives=%zu\n",
                seconds(end - mergeStart).c_str(), report.built, report.reused, report.superbundles,
                report.mergedBundles, report.mergedAssets, report.archives);
    if (!report.issue.empty()) std::printf("issue: %s\n", report.issue.c_str());
    for (const auto& [mod, problems] : report.problems)
        for (const auto& problem : problems) std::printf("problem %s: %s\n", mod.c_str(), problem.c_str());
    for (const auto& note : report.notes)
        if (note.starts_with("Merge times") || note.find(": times:") != std::string::npos) std::printf("%s\n", note.c_str());
    if (!notesFile.empty()) {
        std::ofstream out(notesFile);
        for (const auto& note : report.notes) out << note << '\n';
        std::printf("%zu note(s) written to %s\n", report.notes.size(), notesFile.string().c_str());
    }
    return report.built ? 0 : 1;
}
