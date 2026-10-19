#include "cas_codec.h"
#include "Engine/Core/Platform/path_text.h"

#ifdef _WIN32
#include <Windows.h>
#endif
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

#include "lz4.h"
#include "miniz.h"
#include "zstd.h"

namespace dingosdk::frostbite {
namespace {

class Cursor final {
public:
    explicit Cursor(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
    [[nodiscard]] std::uint16_t u16_be() {
        const auto value = take(2);
        return static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(value[0]) << 8) |
                                          std::to_integer<std::uint8_t>(value[1]));
    }
    [[nodiscard]] std::uint16_t u16_le() {
        const auto value = take(2);
        return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(value[0]) |
                                          (std::to_integer<std::uint8_t>(value[1]) << 8));
    }
    [[nodiscard]] std::uint32_t u32_be() {
        std::uint32_t result{};
        for (const auto byte : take(4)) result = (result << 8) | std::to_integer<std::uint8_t>(byte);
        return result;
    }
    [[nodiscard]] std::span<const std::byte> take(std::size_t size) {
        if (size > remaining()) throw std::runtime_error("Truncated CAS stream");
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};

// Oodle is the game's own module, so it is resolved once and never unloaded.
#ifdef _WIN32
// What Oodle does when one of its checks fails on what it is given: its default stops the
// process at a breakpoint (the launcher's mod scan, "EXCEPTION_BREAKPOINT" in oo2core,
// 2026-10-15). Not stopping, the call fails and the block is reported as unreadable.
int __cdecl oodle_assertion(const char*, int, const char*, const char*) { return 0; }
HMODULE oodle(const std::filesystem::path& gameRoot) {
    static std::mutex mutex;
    static HMODULE handle{};
    std::scoped_lock lock(mutex);
    if (handle) return handle;
    if (auto* loaded = GetModuleHandleW(L"oo2core_9_win64.dll")) return handle = loaded;
    if (gameRoot.empty()) throw std::runtime_error("Oodle CAS data needs the game directory");
    std::vector<std::filesystem::path> matches;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(gameRoot, error)) {
        if (error || !entry.is_regular_file(error)) { error.clear(); continue; }
        auto name = entry.path().filename().wstring();
        std::ranges::transform(name, name.begin(), ::towlower);
        // Under Proton, a Unix filename containing '\\' can leave extra directories
        // in MSVC's iterator path. Keep the candidate anchored to the original root.
        if (name.starts_with(L"oo2core_") && name.ends_with(L"_win64.dll"))
            matches.push_back(gameRoot / entry.path().filename());
    }
    if (matches.empty()) throw std::runtime_error("The game's Oodle codec was not found");
    std::ranges::sort(matches);
    handle = LoadLibraryExW(matches.back().c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle) throw std::runtime_error("Cannot load " + path_utf8(matches.back()) +
                                          " (Windows error " + std::to_string(GetLastError()) + ")");
    // (Only for a copy loaded here: the game's own, found above, keeps the game's handler.)
    using Assertion = int(__cdecl*)(const char*, int, const char*, const char*);
#pragma warning(push)
#pragma warning(disable: 4191)
    if (const auto set = reinterpret_cast<Assertion (__cdecl*)(Assertion)>(GetProcAddress(handle, "OodleCore_Plugins_SetAssertion")))
#pragma warning(pop)
        set(&oodle_assertion);
    return handle;
}

template <typename Function> Function oodle_function(const std::filesystem::path& gameRoot, const char* name) {
#pragma warning(push)
#pragma warning(disable: 4191)
    const auto address = GetProcAddress(oodle(gameRoot), name);
#pragma warning(pop)
    if (!address) throw std::runtime_error(std::string("Oodle export is missing: ") + name);
    return reinterpret_cast<Function>(address);
}
#else
// Linux dedicated servers read pre-exported world-layers.json; Oodle blocks
// (game CAS) need the Windows game + oo2core DLL and are unsupported here.
[[maybe_unused]] void *oodle(const std::filesystem::path &) {
    throw std::runtime_error("Oodle CAS data is only supported on Windows (export world-layers.json there)");
}
template <typename Function> Function oodle_function(const std::filesystem::path&, const char* name) {
    (void)name;
    throw std::runtime_error("Oodle CAS data is only supported on Windows");
}
#endif

std::vector<std::byte> zlib_decode(std::span<const std::byte> input, std::size_t size) {
    // miniz is built without its zlib-compatible wrappers, so inflate directly.
    std::vector<std::byte> output(size);
    const auto written = tinfl_decompress_mem_to_mem(output.data(), output.size(), input.data(),
                                                     input.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || written != size)
        throw std::runtime_error("zlib CAS block failed to decode");
    return output;
}

std::vector<std::byte> lz4_decode(std::span<const std::byte> input, std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("LZ4 CAS block is too large");
    std::vector<std::byte> output(size);
    if (LZ4_decompress_safe(reinterpret_cast<const char*>(input.data()),
                            reinterpret_cast<char*>(output.data()),
                            static_cast<int>(input.size()),
                            static_cast<int>(size)) != static_cast<int>(size))
        throw std::runtime_error("LZ4 CAS block failed to decode");
    return output;
}

std::vector<std::byte> zstd_decode(std::span<const std::byte> input, std::size_t size) {
    std::vector<std::byte> output(size);
    const auto result = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(result) || result != size)
        throw std::runtime_error(std::string("Zstandard CAS block failed to decode: ") +
                                 (ZSTD_isError(result) ? ZSTD_getErrorName(result) : "short output"));
    return output;
}

std::vector<std::byte> oodle_decode(std::span<const std::byte> input, std::size_t size,
                                    const std::filesystem::path& gameRoot) {
#ifdef _WIN32
    using Function = std::int64_t(__cdecl*)(const void*, std::int64_t, void*, std::int64_t, int, int,
                                            int, void*, std::int64_t, void*, void*, void*, std::int64_t, int);
    using BufferSize = std::int64_t(__cdecl*)(int, std::int64_t, int);
    const auto decode = oodle_function<Function>(gameRoot, "OodleLZ_Decompress");
    // A block can be corrupt, or not a block at all (read_manifest tries a payload as one when
    // it does not read as a manifest), and its size is only what its header says. Oodle may
    // then write past that size: the buffer it says such data needs (for any compressor) is
    // given, and cut to the size afterwards. With exactly the size, it overwrote the heap
    // (the launcher's mod scan crashing in malloc after an Oodle block, 2026-10-15).
    constexpr int any_compressor = -1; // OodleLZ_Compressor_Invalid
    constexpr std::int64_t block = 0x40000; // OODLELZ_BLOCK_LEN
    const auto needed = oodle_function<BufferSize>(gameRoot, "OodleLZ_GetDecodeBufferSize")(
        any_compressor, static_cast<std::int64_t>(size), 1);
    const auto padded = std::max<std::int64_t>(needed, static_cast<std::int64_t>(size) + block);
    std::vector<std::byte> output(static_cast<std::size_t>(padded));
    if (decode(input.data(), static_cast<std::int64_t>(input.size()), output.data(),
               static_cast<std::int64_t>(size), 1, 0, 0, nullptr, 0, nullptr, nullptr,
               nullptr, 0, 3) != static_cast<std::int64_t>(size))
        throw std::runtime_error("Oodle CAS block failed to decode");
    output.resize(size);
    return output;
#else
    (void)input;
    (void)size;
    (void)gameRoot;
    throw std::runtime_error("Oodle CAS data is only supported on Windows (export world-layers.json there)");
#endif
}

std::vector<std::byte> oodle_encode(std::span<const std::byte> input, CasCompression compression,
                                    int level, const std::filesystem::path& gameRoot) {
#ifdef _WIN32
    using Bound = std::int64_t(__cdecl*)(int, std::int64_t);
    using Compress = std::int64_t(__cdecl*)(int, const void*, std::int64_t, void*, int, void*, void*,
                                            void*, void*, std::int64_t);
    int compressor{};
    switch (compression) {
    case CasCompression::kraken: compressor = 8; break;
    case CasCompression::selkie: compressor = 11; break;
    case CasCompression::leviathan: compressor = 13; break;
    case CasCompression::raw: return {input.begin(), input.end()};
    }
    const auto bound = oodle_function<Bound>(gameRoot, "OodleLZ_GetCompressedBufferSizeNeeded");
    const auto compress = oodle_function<Compress>(gameRoot, "OodleLZ_Compress");
    const auto capacity = bound(compressor, static_cast<std::int64_t>(input.size()));
    if (capacity <= 0) throw std::runtime_error("Oodle could not size its compression buffer");
    std::vector<std::byte> output(static_cast<std::size_t>(capacity));
    const auto length = compress(compressor, input.data(), static_cast<std::int64_t>(input.size()),
                                 output.data(), level, nullptr, nullptr, nullptr, nullptr, 0);
    if (length <= 0 || length > capacity) throw std::runtime_error("Oodle CAS compression failed");
    output.resize(static_cast<std::size_t>(length));
    return output;
#else
    (void)input;
    (void)compression;
    (void)level;
    (void)gameRoot;
    throw std::runtime_error("Oodle CAS data is only supported on Windows");
#endif
}

std::vector<std::byte> read_block(Cursor& cursor, const CasDecodeOptions& options) {
    if (cursor.remaining() < 8) throw std::runtime_error("Truncated CAS block header");
    auto decodedSize = cursor.u32_be();
    auto compression = cursor.u16_le();
    std::size_t encodedSize = cursor.u16_be();
    const auto flags = static_cast<std::uint8_t>(compression >> 8);
    encodedSize += static_cast<std::size_t>(flags & 0x0F) << 16;
    const auto usesDictionary = (decodedSize & 0xFF000000U) != 0;
    decodedSize &= 0x00FFFFFFU;
    compression &= 0x7F;
    if (decodedSize > options.maximumOutputSize) throw std::runtime_error("CAS block is too large");
    const auto input = cursor.take(encodedSize);
    switch (compression) {
    case 0x00:
        if (input.size() != decodedSize) throw std::runtime_error("Raw CAS block size does not match its header");
        return {input.begin(), input.end()};
    case 0x02: return zlib_decode(input, decodedSize);
    case 0x09: return lz4_decode(input, decodedSize);
    case 0x0F:
        if (usesDictionary) throw std::runtime_error("Dictionary Zstandard CAS blocks are not supported");
        return zstd_decode(input, decodedSize);
    case 0x11: case 0x15: case 0x19: return oodle_decode(input, decodedSize, options.gameRoot);
    default:
        throw std::runtime_error("Unsupported CAS compression type " +
                                 std::to_string(static_cast<unsigned>(compression)));
    }
}

} // namespace

std::vector<std::byte> decode_cas(const std::span<const std::byte> encoded,
                                  const CasDecodeOptions& options) {
    Cursor cursor(encoded);
    std::vector<std::byte> output;
    while (cursor.remaining() != 0) {
        const auto block = read_block(cursor, options);
        if (block.size() > options.maximumOutputSize - output.size())
            throw std::runtime_error("CAS output exceeds the safety limit");
        output.insert(output.end(), block.begin(), block.end());
    }
    return output;
}

std::vector<std::byte> encode_cas(const std::span<const std::byte> decoded,
                                  const CasEncodeOptions& options) {
    if (!options.blockSize || options.blockSize > 0x10000)
        throw std::invalid_argument("CAS block size must be between 1 and 65536 bytes");
    const auto count = std::max<std::size_t>(1, (decoded.size() + options.blockSize - 1) / options.blockSize);
    std::vector<std::byte> output;
    for (std::size_t index = 0; index < count; ++index) {
        const auto begin = std::min(decoded.size(), index * options.blockSize);
        const auto input = decoded.subspan(begin, std::min(options.blockSize, decoded.size() - begin));
        auto payload = options.compression == CasCompression::raw || input.empty()
            ? std::vector<std::byte>(input.begin(), input.end())
            : oodle_encode(input, options.compression, options.compressionLevel, options.gameRoot);
        auto compression = options.compression;
        // A block that did not shrink is stored raw, exactly as the game does.
        if (payload.size() >= input.size()) {
            payload.assign(input.begin(), input.end());
            compression = CasCompression::raw;
        }
        if (payload.size() > 0xFFFFF) throw std::overflow_error("Encoded CAS block is too large");
        const auto packed = (static_cast<std::uint64_t>(input.size() & 0xFFFFFFU) << 32U) |
                            (static_cast<std::uint64_t>(compression) << 24U) |
                            (0x7ULL << 20U) | static_cast<std::uint64_t>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8)
            output.push_back(static_cast<std::byte>((packed >> shift) & 0xFFU));
        output.insert(output.end(), payload.begin(), payload.end());
    }
    return output;
}

} // namespace dingosdk::frostbite
