// Contract tests for the playlist lookup crash guard. No game binary is needed:
// the patch site bytes, the jrcxz displacement and the branch target arithmetic
// are all fixed data of the supported build, so they are checked here.
#include "Engine/Game/Build/20260929/local_music.h"
#include <cstdint>
#include <cstdio>

namespace music = dingosdk::game::build::v20260929::local_music;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    };

    check(music::playlist_lookup_safety_contract.size() == 13, "the contract is 13 bytes");
    check(music::playlist_lookup_patch_bytes.size() == 13, "the patch is 13 bytes");
    check(music::playlist_lookup_safety_contract.size() == music::playlist_lookup_patch_bytes.size(),
          "the patch replaces the contract byte for byte");
    check(music::playlist_lookup_safety_rva == 0x14f80c7, "the site is the reviewed RVA");

    // The patch decodes as xor eax,eax; and rcx,-5; jrcxz; the preserved flag test; nop.
    const auto& patch = music::playlist_lookup_patch_bytes;
    check(patch[0] == 0x31 && patch[1] == 0xc0, "mov eax,0 becomes xor eax,eax");
    check(patch[2] == 0x48 && patch[3] == 0x83 && patch[4] == 0xe1 && patch[5] == 0xfb,
          "and rcx,-5 is preserved");
    check(patch[6] == 0xe3, "jrcxz is installed after the mask");
    check(patch[7] == music::playlist_lookup_jump_displacement, "the jrcxz displacement is declared");
    check(patch[8] == 0x66 && patch[9] == 0x85 && patch[10] == 0x71 && patch[11] == 0x14,
          "the [rcx+0x14] flag test is preserved");
    check(patch[12] == 0x90, "the trailing byte pads the 5-byte mov");

    // The 2-byte jrcxz sits at +6, so its relative displacement is measured from +8.
    check(music::playlist_lookup_jump_next == 8, "jrcxz ends 8 bytes into the site");
    check(music::playlist_lookup_jump_target == 0x14f80f8,
          "the guard jumps to the loop increment (inc r9d; add r8,4)");

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
