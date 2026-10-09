# Third-party dependencies

| Library | Pinned revision | License | Upstream |
| --- | --- | --- | --- |
| Dear ImGui (v1.91.9b) | `f5befd2d29e66809cd1110a152e375a7f1981f06` | MIT | [ocornut/imgui](https://github.com/ocornut/imgui/tree/f5befd2d29e66809cd1110a152e375a7f1981f06) |
| Microsoft Detours | `adb07604aa56508448b95bf037c2a6d0d3b6831a` | MIT | [microsoft/Detours](https://github.com/microsoft/Detours/tree/adb07604aa56508448b95bf037c2a6d0d3b6831a) |
| Tencent RapidJSON | `24b5e7a8b27f42fa16b96fc70aade9106cf7102f` | MIT, with bundled BSD notices | [Tencent/rapidjson](https://github.com/Tencent/rapidjson/tree/24b5e7a8b27f42fa16b96fc70aade9106cf7102f) |
| spdlog (v1.17.0) | `79524ddd08a4ec981b7fea76afd08ee05f83755d` | MIT | [gabime/spdlog](https://github.com/gabime/spdlog/tree/79524ddd08a4ec981b7fea76afd08ee05f83755d) |
| SQLite | `3.53.4` (`3530400`) | Public domain | [sqlite.org amalgamation](https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip) |
| Valve networking ABI headers | `a424b7db649438acafb60c99cae6667587c42732` | BSD-3-Clause | [ValveSoftware/GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets/tree/a424b7db649438acafb60c99cae6667587c42732) |
| miniz | `3.1.2` | MIT | [richgel999/miniz](https://github.com/richgel999/miniz/releases/tag/3.1.2) |
| bcdec | `80859ed3b7afb1c527a2a99d70c61457bea72d0c` | MIT or Unlicense | [iOrange/bcdec](https://github.com/iOrange/bcdec/tree/80859ed3b7afb1c527a2a99d70c61457bea72d0c) |
| LZ4 | `1.10.0` | BSD-2-Clause | [lz4/lz4](https://github.com/lz4/lz4/releases/tag/v1.10.0) |
| Zstandard | `1.5.7` | BSD-3-Clause (dual-licensed with GPLv2; used under BSD) | [facebook/zstd](https://github.com/facebook/zstd/releases/tag/v1.5.7) |
| Lua | `5.4.9` | MIT | [lua.org](https://www.lua.org/ftp/lua-5.4.9.tar.gz) |
| Montserrat font | `fonts/Montserrat-*.ttf` | SIL Open Font License 1.1 | [JulietaUla/Montserrat](https://github.com/JulietaUla/Montserrat) |
| Permanent Marker font | `fonts/PermanentMarker-Regular.ttf` | Apache 2.0 | [Google Fonts](https://fonts.google.com/specimen/Permanent+Marker) |

`manifest.json` records the upstream SHA-256 for every vendored file, with declared
local patch hashes where applicable. Detours and RapidJSON came from pinned GitHub
source archives; ImGui came from pinned upstream source URLs. Builds require no
package manager or network access. Keep each library's license file (`LICENSE` for spdlog, `LICENSE.txt` for the others) in source
distributions and alongside binaries that use it.

Detours owns instruction relocation and hook transactions. The project adapter is
in `Engine/Core/Hooks/hooks.cpp`. RapidJSON supplies the SAX reader and JSON writers;
DingoSDK's owning value model is in `Engine/Core/Json/json.h` and
`Engine/Core/Json/json.cpp`.

The ImGui local vendor patch is `imgui/backends/imgui_impl_dx12.cpp`: font upload
checks resource/command failures, bounds its GPU wait to five seconds, propagates
failure, and retains submitted resources on timeout to avoid premature release.
Detours has a local patch in `detours/src/detours.cpp`: transaction thread and
operation records use `VirtualAlloc`/`VirtualFree` instead of the shared heap.
Enlistment and cleanup can run while another thread owns the heap lock and is
suspended, so ordinary `new`/`delete` can deadlock startup. Both record types use
the same allocation path for successful commits, failed enlistment, and aborts.
The patch also preserves function-entry and jump-back boundaries when the
compact x64 alignment table truncates an offset of eight or more. Thread context
updates distinguish relocated code from the x64 replacement-entry stub and stop
after one remap. The instruction decoder remains upstream. Related upstream
discussions: [allocator deadlock](https://github.com/microsoft/Detours/pull/261)
and [thread context updates](https://github.com/microsoft/Detours/pull/368).

Current patch hashes are recorded under `local_patches`. RapidJSON is unmodified.

spdlog is used header-only with the VS2022 C++20 `std::format` backend;
bundled fmt is not distributed. `Engine/Core/Log` owns shared formatting,
categories, severity filtering, repeat suppression, console output, and a Win32
append sink. The launcher resets the single file before the runtime appends to
it, so two processes never use competing buffered file offsets. This layout
takes inspiration from [R5SDK's logger](https://github.com/R5Reloaded/r5sdk/blob/p4sync/src/core/logger.cpp).

SQLite is the unmodified official C amalgamation, compiled into the runtime. `manifest.json` includes the archive SHA3-256 and individual source
SHA-256 hashes. Extension loading and double-quoted SQL string literals are
disabled; thread safety is enabled. ReSkate uses prepared statements, strict domain
tables, WAL with full synchronous commits, and SQLite's online backup API. The
public-domain notice is retained in `sqlite/LICENSE.txt`.

`steam_networking` contains unmodified public headers for the v012 Steam
Networking Sockets ABI. ReSkate resolves flat exports from the game's verified
original `steam_api64.dll`. It does not link the standalone networking library, and the
client package carries no Steam DLL; the dedicated server package carries the Steam files
it signs in with (see `Server/README.txt`). Keep `steam_networking/LICENSE` with distributions.

LZ4 1.10.0 (`lz4/`) supplies the unmodified BSD-2-Clause block codec for bounded, lossless multiplayer packet compression. Source hashes and upstream tag are recorded in `manifest.json`; packages include `licenses/lz4-LICENSE.txt`.

Zstandard 1.5.7 (`zstd/`) is the library's common, compress and decompress sources, built without legacy format
support or assembly, for multiplayer packet and game archive compression. It is not
yet hashed in `manifest.json`. Packages include `licenses/zstd-LICENSE.txt` (the
BSD license; `COPYING` is the alternative GPLv2 grant).

miniz supplies the ZIP reader for the launcher's downloads (DepotDownloader, the
content cache, Thunderstore mods) and the dedicated server's updates.
The official 3.1.2 amalgamation is unmodified; compression, archive writing,
zlib compatibility, stdio and timestamp APIs are disabled. Extraction writes only
the expected file to a fixed temporary path, bounds its size, and checks the
uncompressed SHA-256 before replacing the cache. Keep `miniz/LICENSE.txt` with
binary packages. Builds do not fetch the library.
