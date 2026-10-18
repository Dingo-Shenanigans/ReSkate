// What superbundles write waits in a store of their own and is settled into the patch's archive in
// turn (CasStore::settle). Bytes the archive already holds are pointed at, not written again: every
// map carries the same changes other mods make, and writing them once per map filled the archive.
//
// Arguments: <data root> (a folder holding the game's Data/); skipped when none is given. Only the
// game's layout is read; the archive is written to a folder of its own under the temp directory.
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Vfs/game_archives.h"
#include <fstream>
#include <iostream>

using namespace dingosdk;
using namespace dingosdk::mods::detail;

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}
std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::byte>(text[i]);
    return out;
}
std::string read_back(const fs::path& file, std::uint32_t offset, std::uint32_t size) {
    std::ifstream in(file, std::ios::binary);
    in.seekg(offset);
    std::string text(size, '\0');
    in.read(text.data(), size);
    return text;
}
// A placement made final: the archive it names and the offset in it.
fb::BundleFileInfo settle_one(const CasStore& waiting, fb::BundleFileInfo file) {
    waiting.settled(file.location, file.offset);
    return file;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !fs::exists(fs::path(argv[1]) / "Data" / "layout.toc")) {
        std::cout << "No game given; skipped.\n";
        return 0;
    }
    const fs::path data = fs::path(argv[1]) / "Data";
    const auto output = fs::temp_directory_path() / L"reskate-cas-sharing-test";
    std::error_code error;
    fs::remove_all(output, error);
    const auto layout = vfs::read_layout(data / L"layout.toc");
    CasStore main(data, output, layout.root);
    const std::string package = "configurations/layout/defaultinstallpackage";
    const auto chunks = main.chunks_in(package);
    if (chunks.empty()) {
        std::cerr << "The layout has no install chunk in " << package << "\n";
        return 1;
    }
    // Another install chunk of the same package when the layout has one: its placements name it.
    const auto chunk = chunks[0], other = chunks.size() > 1 ? chunks[1] : chunks[0];
    const auto archive = output / L"Win32" / fs::path(package) / L"cas_01.cas";
    const auto x = bytes_of("first payload, shared"), y = bytes_of("second payload"), z = bytes_of("third, only once");

    // Two superbundles, each writing in a waiting store of its own.
    auto one = main.waiting();
    const auto p1 = one.write(chunk, 1, x);
    const auto p2 = one.write(chunk, 1, y);
    auto two = main.waiting();
    const auto q1 = two.write(other, 1, y);   // the same bytes, from another install chunk of the package
    const auto q2 = two.write(chunk, 1, z);
    const auto q3 = two.write(chunk, 1, x);
    const auto q4 = two.write(chunk, 1, x);   // twice in one superbundle

    main.settle(one);
    const auto s1 = settle_one(one, p1), s2 = settle_one(one, p2);
    main.settle(two);
    const auto t1 = settle_one(two, q1), t2 = settle_one(two, q2), t3 = settle_one(two, q3), t4 = settle_one(two, q4);

    check(fs::file_size(archive, error) == x.size() + y.size() + z.size(), "each payload is in the archive once");
    check(s1.location.archive == 1 && t3.location.archive == 1, "settled placements name archive 1");
    check(t3.offset == s1.offset && t4.offset == s1.offset, "a payload the archive holds is pointed at, not written again");
    check(t1.offset == s2.offset, "so is one written again, from any install chunk of the same package");
    check(t1.location.installChunk == other, "the placement keeps its own install chunk");
    check(read_back(archive, s1.offset, s1.size) == "first payload, shared" &&
              read_back(archive, s2.offset, s2.size) == "second payload" &&
              read_back(archive, t2.offset, t2.size) == "third, only once",
          "every placement reads its own bytes");

    // A bundle's writes taken into its superbundle's (combine), then settled.
    auto job = main.waiting();
    auto part = job.waiting();
    const auto r1 = part.write(chunk, 1, z);
    const auto fresh = bytes_of("new in the third superbundle");
    const auto r2 = part.write(chunk, 1, fresh);
    job.take(part);
    auto u1 = r1, u2 = r2;
    part.taken(u1.location, u1.offset);
    part.taken(u2.location, u2.offset);
    main.settle(job);
    const auto v1 = settle_one(job, u1), v2 = settle_one(job, u2);
    check(v1.offset == t2.offset, "a taken payload the archive holds is pointed at");
    check(read_back(archive, v2.offset, v2.size) == "new in the third superbundle", "a new one is appended and found");
    check(fs::file_size(archive, error) == x.size() + y.size() + z.size() + fresh.size(), "and only the new one is added");

    fs::remove_all(output, error);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "cas store sharing: ok\n";
    return 0;
}
