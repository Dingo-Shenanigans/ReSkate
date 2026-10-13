#!/bin/bash
# ReSkateLauncher.exe cross-build from macOS: brew install mingw-w64.
# Run from the repo root: contrib/build-launcher-mingw.sh [outdir]  (default ~/Games/reskate/mingw-build)
# CMake path is MSVC-only (CMakeLists.txt:3-5), so this drives mingw g++ directly.
# Out: $B/ReSkateLauncher.exe (+ stripped copy). Source list mirrors cmake/Apps.cmake + Core.cmake.
set -u
B="${1:-$HOME/Games/reskate/mingw-build}"; CXX=x86_64-w64-mingw32-g++; CC=x86_64-w64-mingw32-gcc
mkdir -p "$B/gen/generated" "$B/gen/src" "$B/obj"; rm -f "$B"/obj/*
P="$PWD"
cat > "$B/gen/generated/launcher_update_config.h" <<H
#pragma once
namespace dingosdk::launcher_update {
inline constexpr bool launcher_binary_updates = false;
inline constexpr wchar_t launcher_release_repo[] = L"Dingo-Shenanigans/ReSkate";
}
H
cat > "$B/gen/generated/crash_report_config.h" <<H
#pragma once
namespace dingosdk::crash_report {
inline constexpr wchar_t configured_url[] = L"";
inline constexpr char build_version[] = "$(git describe --always --dirty --abbrev=12)";
}
H
# mingw-w64 wincodec.h lacks this enumerator (MSVC SDK value 4)
cat > "$B/gen/mingw_compat.h" <<H
#pragma once
#include <windows.h>
#include <wincodec.h>
#ifndef WICBitmapInterpolationModeHighQualityCubic
#define WICBitmapInterpolationModeHighQualityCubic ((WICBitmapInterpolationMode)4)
#endif
H
# 3 MSVC-isms patched in COPIES (repo sources untouched): wstring->ofstream ctor, _MSC_VER
sed 's/std::ofstream metadata(dump.wstring() + L".json"/std::ofstream metadata(std::filesystem::path(dump.wstring() + L".json")/' Launcher/crash_reporter.cpp > "$B/gen/src/crash_reporter.cpp"
sed 's/std::ifstream input(metadata,/std::ifstream input(std::filesystem::path(metadata),/' Engine/Core/Debug/upload.cpp > "$B/gen/src/upload.cpp"
sed 's/_MSC_VER/1944/' Engine/Core/Log/banner.cpp > "$B/gen/src/banner.cpp"
FL="-O1 -w -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -DSPDLOG_USE_STD_FORMAT -DSPDLOG_WCHAR_FILENAMES -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -I. -I$B/gen/generated -IExternal -IExternal/imgui -IExternal/miniz -IExternal/spdlog/include -IExternal/bcdec -IExternal/lz4 -IExternal/zstd/lib -IExternal/sqlite"
SRC="$(sed -n '/Launcher\/main.cpp/,/problem.h/p' cmake/Apps.cmake | grep -o '[A-Za-z_/]*\.cpp' | sort -u | grep -v '^Launcher/updater_x$')
Engine/Core/Log/logger.cpp Engine/Core/Log/events.cpp Engine/Core/Log/paths.cpp Engine/Core/Log/crash.cpp
Engine/Core/Console/console_core.cpp Engine/Core/Console/command_registry.cpp Engine/Core/Console/console_activity.cpp
Engine/Core/Platform/launcher_support.cpp Engine/Core/Platform/launcher_pe.cpp Engine/Core/Platform/launcher_injection.cpp Engine/Core/Json/json.cpp
Engine/Core/Debug/client.cpp Engine/Core/Debug/trace.cpp Engine/Core/Debug/native_dump.cpp Engine/Core/Debug/multipart.cpp
Engine/Vfs/initfs.cpp Engine/Vfs/mod_list.cpp Engine/Vfs/native_db.cpp Engine/Vfs/https_download.cpp Engine/Vfs/content_cache.cpp Engine/Vfs/content_catalogs.cpp Engine/Vfs/content_cache_install.cpp Engine/Vfs/world_layer_scan.cpp
Engine/Vfs/game_archives.cpp Engine/Vfs/game_bundles.cpp Engine/Vfs/item_thumbnails.cpp Engine/Vfs/game_textures.cpp
Engine/Vfs/mod_catalog.cpp Engine/Vfs/mod_merge.cpp Engine/Vfs/mod_merge_files.cpp Engine/Vfs/mod_merge_stamp.cpp Engine/Vfs/mod_merge_cas_store.cpp Engine/Vfs/mod_merge_combine.cpp Engine/Vfs/mod_merge_overrides.cpp Engine/Vfs/mod_merge_grid.cpp Engine/Vfs/mod_merge_load_screens.cpp Engine/Vfs/mod_music.cpp Engine/Vfs/mod_scoring.cpp Engine/Vfs/mod_store_copies.cpp
Engine/Resource/binary_io.cpp Engine/Resource/binary_bundle.cpp Engine/Resource/cas_codec.cpp Engine/Resource/ebx_document.cpp Engine/Resource/ebx_merge.cpp Engine/Resource/material_grid.cpp Engine/Resource/shader_lookup.cpp Engine/Resource/bundle_ref_table.cpp Engine/Resource/ebx_writer.cpp Engine/Resource/toc.cpp Engine/Resource/texture.cpp Engine/Resource/mesh_set.cpp Engine/Resource/skeleton_asset.cpp
Extension/UI/Overlay/playstation_input.cpp
External/imgui/imgui.cpp External/imgui/imgui_draw.cpp External/imgui/imgui_tables.cpp External/imgui/imgui_widgets.cpp External/imgui/backends/imgui_impl_dx12.cpp External/imgui/backends/imgui_impl_win32.cpp"
CSRC="External/miniz/miniz.c External/lz4/lz4.c $(ls External/zstd/lib/common/*.c External/zstd/lib/compress/*.c External/zstd/lib/decompress/*.c)"
obj() { echo "$B/obj/$(echo "$1" | tr / _).o"; }
fail=0
for f in $SRC Launcher/crash_reporter.cpp Engine/Core/Log/banner.cpp Engine/Core/Debug/upload.cpp; do
  s=$f; case $f in Launcher/crash_reporter.cpp) s="$B/gen/src/crash_reporter.cpp";; Engine/Core/Log/banner.cpp) s="$B/gen/src/banner.cpp";; Engine/Core/Debug/upload.cpp) s="$B/gen/src/upload.cpp";; esac
  ( $CXX -std=c++20 -include "$B/gen/mingw_compat.h" -I"$(dirname "$f")" -c $FL "$s" -o "$(obj $f)" 2> "$(obj $f).err" || echo "FAIL $f" ) &
  while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.2; done
done
for f in $CSRC; do ( $CC -c $FL "$f" -o "$(obj $f)" 2> "$(obj $f).err" || echo "FAIL $f" ) & while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.2; done; done
wait
cat > "$B/gen/launcher.rc" <<H
1 ICON "$P/assets/launcher/icon.ico"
FONT_HEADING RCDATA "$P/External/fonts/Montserrat-ExtraBold.ttf"
FONT_BODY RCDATA "$P/External/fonts/Montserrat-SemiBold.ttf"
FONT_BRUSH RCDATA "$P/External/fonts/PermanentMarker-Regular.ttf"
LAUNCHER_BACKGROUND RCDATA "$P/assets/launcher/background.jpg"
LAUNCHER_ICON_MODS RCDATA "$P/assets/launcher/icon_mods.png"
LAUNCHER_ICON_SETTINGS RCDATA "$P/assets/launcher/icon_settings.png"
LAUNCHER_ICON_THUNDERSTORE RCDATA "$P/assets/launcher/icon_thunderstore.png"
H
x86_64-w64-mingw32-windres "$B/gen/launcher.rc" -o "$B/obj/launcher_rc.res.o" || exit 1
$CXX -mwindows -municode -static -o "$B/ReSkateLauncher.exe" "$B"/obj/*.o -ld3d12 -ldxgi -ld3dcompiler -ldwmapi -lwinhttp -lshell32 -lwindowscodecs -lole32 -lbcrypt -ldbghelp -lhid -lcfgmgr32 -luuid -lgdi32 -lcomdlg32 -loleaut32 -lsetupapi -lshlwapi -limm32 -ldwrite -lxinput -ladvapi32 -lwinmm || exit 1
x86_64-w64-mingw32-strip -o "$B/ReSkateLauncher.stripped.exe" "$B/ReSkateLauncher.exe"
shasum -a 256 "$B/ReSkateLauncher.stripped.exe"
