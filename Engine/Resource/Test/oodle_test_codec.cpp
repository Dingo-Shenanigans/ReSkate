// Test-only DLL: never package this as the game's actual codec.
#include <cstdint>

extern "C" __declspec(dllexport) std::int64_t __cdecl OodleLZ_Decompress(
    const void* input, std::int64_t inputSize, void* output, std::int64_t outputSize,
    int, int, int, void*, std::int64_t, void*, void*, void*, std::int64_t, int) {
    if (!input || !output || inputSize != 1 || outputSize != 1) return -1;
    *static_cast<unsigned char*>(output) = *static_cast<const unsigned char*>(input);
    return 1;
}

// The buffer a decode into `rawSize` bytes needs: as the real codec, a little more than asked.
extern "C" __declspec(dllexport) std::int64_t __cdecl OodleLZ_GetDecodeBufferSize(int, std::int64_t rawSize, int) {
    return rawSize + 16;
}
