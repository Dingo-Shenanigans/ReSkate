// Builds a built-in content pack (Engine/Vfs/builtin_packs.h) from its zip in assets/builtin:
//   ReSkateBuiltinPacker <pack.zip> <pack.rskpack>
// The zip's files go into one plain archive (builtin_pack_format.h) with files of the same name
// next to each other (a Board Break deck pack carries much the same files once for every deck),
// which is then compressed whole with zstd and long-distance matching. Zip compresses each file
// on its own and finds none of that; this way each repeat costs next to nothing.
#include "Engine/Vfs/builtin_pack_format.h"
#include <miniz.h>
#include <zstd.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct File {
    std::string name;
    std::vector<unsigned char> bytes;
};
std::string base_name(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
template<class T> void put(std::vector<unsigned char>& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
int fail(const std::string& message) {
    std::cerr << "ReSkateBuiltinPacker: " << message << '\n';
    return 1;
}
}

int main(int argc, char** argv) {
    if (argc != 3) return fail("usage: ReSkateBuiltinPacker <pack.zip> <pack.rskpack>");
    std::ifstream input(argv[1], std::ios::binary);
    const std::vector<unsigned char> zip_bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (zip_bytes.empty()) return fail(std::string("cannot read ") + argv[1]);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zip_bytes.data(), zip_bytes.size(), 0)) return fail("not a zip");
    std::vector<File> files;
    std::uint64_t total{};
    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index) {
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&zip, index, &info)) return fail("an unreadable entry");
        if (info.m_is_directory) continue;
        File file;
        file.name = info.m_filename;
        if (!dingosdk::mods::builtin_pack::safe_name(file.name)) return fail("an entry the game may not have: " + file.name);
        file.bytes.resize(static_cast<std::size_t>(info.m_uncomp_size));
        if (!file.bytes.empty() && !mz_zip_reader_extract_to_mem(&zip, index, file.bytes.data(), file.bytes.size(), 0))
            return fail("cannot unpack " + file.name);
        total += file.bytes.size();
        files.push_back(std::move(file));
    }
    mz_zip_reader_end(&zip);
    if (total > dingosdk::mods::builtin_pack::max_bytes) return fail("the pack is larger than a built-in pack may be");
    // Same names together (a deck pack's decks/<deck>/<file> for every deck), then by path.
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) {
        const auto ka = base_name(a.name), kb = base_name(b.name);
        return ka != kb ? ka < kb : a.name < b.name;
    });

    std::vector<unsigned char> plain;
    plain.insert(plain.end(), dingosdk::mods::builtin_pack::magic.begin(), dingosdk::mods::builtin_pack::magic.end());
    put<std::uint32_t>(plain, static_cast<std::uint32_t>(files.size()));
    for (const auto& file : files) {
        put<std::uint16_t>(plain, static_cast<std::uint16_t>(file.name.size()));
        plain.insert(plain.end(), file.name.begin(), file.name.end());
        put<std::uint64_t>(plain, file.bytes.size());
    }
    for (const auto& file : files) plain.insert(plain.end(), file.bytes.begin(), file.bytes.end());

    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (!context) return fail("out of memory");
    // A window to cover the whole pack (within what any decoder takes by default, 2^27), so a
    // file repeated anywhere in it is found.
    int window = 10;
    while (window < dingosdk::mods::builtin_pack::max_window_log && (std::size_t{1} << window) < plain.size()) ++window;
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(context, ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, window);
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    std::vector<unsigned char> packed(ZSTD_compressBound(plain.size()));
    const auto size = ZSTD_compress2(context, packed.data(), packed.size(), plain.data(), plain.size());
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(size)) return fail(std::string("zstd: ") + ZSTD_getErrorName(size));
    packed.resize(size);
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
    if (!output) return fail(std::string("cannot write ") + argv[2]);
    std::cout << argv[1] << ": " << files.size() << " files, " << total << " bytes -> " << packed.size() << " bytes ("
              << zip_bytes.size() << " as a zip)\n";
    return 0;
}
