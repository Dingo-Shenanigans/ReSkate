if(WIN32)
    add_library(dingosdk_hooks STATIC
        Engine/Core/Hooks/hooks.cpp)
    target_link_libraries(dingosdk_hooks PRIVATE dingosdk_detours)
endif()

add_library(dingosdk_launcher_support STATIC
    Engine/Core/Platform/launcher_support.cpp
    Engine/Core/Platform/launcher_pe.cpp
    $<$<BOOL:${WIN32}>:Engine/Core/Platform/launcher_injection.cpp>)

if(WIN32)
    target_link_libraries(dingosdk_launcher_support PUBLIC bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(dingosdk_launcher_support PUBLIC OpenSSL::Crypto)
endif()

if(WIN32)
    # Checks that the running game is the supported Skate.exe build.
    add_library(dingosdk_supported_build STATIC Engine/Game/Build/image_identity.cpp)
    target_link_libraries(dingosdk_supported_build PUBLIC bcrypt)
endif()

add_library(dingosdk_console_core STATIC
    Engine/Core/Console/console_core.cpp
    Engine/Core/Console/command_registry.cpp
    Engine/Core/Console/console_activity.cpp)

add_library(dingosdk_json STATIC
    Engine/Core/Json/json.cpp)

# The bad-word filter (Engine/Core/Text/word_filter.h) with its list built in: editing
# bad_words.txt re-runs the configure step.
set(bad_words_path "${PROJECT_SOURCE_DIR}/Engine/Core/Text/bad_words.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bad_words_path}")
file(READ "${bad_words_path}" bad_words_source)
configure_file(cmake/templates/bad_words.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_bad_words.h" @ONLY)
add_library(dingosdk_word_filter STATIC Engine/Core/Text/word_filter.cpp)

target_include_directories(dingosdk_json SYSTEM PRIVATE External)


if(WIN32)
    add_library(dingosdk_logging STATIC
        Engine/Core/Log/logger.cpp
        Engine/Core/Log/banner.cpp
        Engine/Core/Log/events.cpp
        Engine/Core/Log/paths.cpp
        Engine/Core/Log/crash.cpp)
    target_link_libraries(dingosdk_logging PUBLIC dingosdk_console_core PRIVATE shell32 ole32)
    target_include_directories(dingosdk_logging SYSTEM PRIVATE External/spdlog/include External)
    target_compile_definitions(dingosdk_logging PRIVATE SPDLOG_USE_STD_FORMAT SPDLOG_WCHAR_FILENAMES)

    # The performance profiler: zones, frame timing, thread CPU use and the stack sampler
    # (Engine/Core/Profiling/profiler.h). Shared by the overlay and the runtime.
    add_library(dingosdk_profiler STATIC
        Engine/Core/Profiling/profiler.cpp
        Engine/Core/Profiling/sampler.cpp)
    target_link_libraries(dingosdk_profiler PUBLIC dingosdk_logging PRIVATE dbghelp)

    add_library(dingosdk_storage STATIC Engine/Core/Storage/save_database.cpp)
    target_link_libraries(dingosdk_storage PUBLIC dingosdk_json PRIVATE dingosdk_sqlite)
endif()

add_library(dingosdk_frostbite STATIC
    Engine/Resource/binary_io.cpp
    Engine/Resource/binary_bundle.cpp
    Engine/Resource/cas_codec.cpp
    Engine/Resource/ebx_document.cpp
    Engine/Resource/ebx_merge.cpp
    Engine/Resource/material_grid.cpp
    Engine/Resource/shader_lookup.cpp
    Engine/Resource/bundle_ref_table.cpp
    Engine/Resource/ebx_writer.cpp
    Engine/Resource/toc.cpp
    Engine/Resource/texture.cpp
    Engine/Resource/mesh_set.cpp
    Engine/Resource/skeleton_asset.cpp)
target_include_directories(dingosdk_frostbite SYSTEM PRIVATE External/bcdec)
target_link_libraries(dingosdk_frostbite PRIVATE dingosdk_lz4 dingosdk_zstd dingosdk_miniz)

add_library(dingosdk_native_db STATIC Engine/Vfs/native_db.cpp)

if(WIN32)
    add_library(dingosdk_initfs STATIC Engine/Vfs/initfs.cpp)
    target_link_libraries(dingosdk_initfs PUBLIC dingosdk_launcher_support dingosdk_json PRIVATE dingosdk_native_db bcrypt)

    # The Mods folder list and mods.json writer, without the merge: shared with
    # the launcher's mod manager.
    add_library(dingosdk_mod_list STATIC Engine/Vfs/mod_list.cpp)
    target_link_libraries(dingosdk_mod_list PUBLIC dingosdk_json)
endif()

# Read access to the installed game's cas archives.
add_library(dingosdk_game_archives STATIC Engine/Vfs/game_archives.cpp Engine/Vfs/game_bundles.cpp
    Engine/Vfs/item_thumbnails.cpp Engine/Vfs/game_textures.cpp)
target_link_libraries(dingosdk_game_archives PUBLIC dingosdk_native_db dingosdk_frostbite)

if(WIN32)
    add_library(dingosdk_mods STATIC
        Engine/Vfs/mod_catalog.cpp
        Engine/Vfs/mod_merge.cpp
        Engine/Vfs/mod_merge_files.cpp
        Engine/Vfs/mod_merge_stamp.cpp
        Engine/Vfs/mod_merge_cas_store.cpp
        Engine/Vfs/mod_merge_combine.cpp
        Engine/Vfs/mod_merge_overrides.cpp
        Engine/Vfs/mod_merge_grid.cpp
        Engine/Vfs/mod_merge_load_screens.cpp
        Engine/Vfs/mod_music.cpp
        Engine/Vfs/mod_scoring.cpp
        Engine/Vfs/mod_store_copies.cpp)
    target_link_libraries(dingosdk_mods PUBLIC dingosdk_json dingosdk_mod_list PRIVATE dingosdk_game_archives dingosdk_native_db dingosdk_frostbite dingosdk_content_cache shell32 bcrypt)

    add_library(dingosdk_custom_scripts STATIC Engine/Scripting/custom_scripts.cpp)
    target_link_libraries(dingosdk_custom_scripts PUBLIC dingosdk_json PRIVATE dingosdk_initfs)

    # HTTPS downloads shared by the park preview and content cache packs.
    add_library(dingosdk_https STATIC Engine/Vfs/https_download.cpp)
    target_link_libraries(dingosdk_https PUBLIC winhttp)
endif()

# The live game's content cache: the launcher installs it, the runtime reads
# catalogues from it (Engine/Vfs/content_cache.h).
add_library(dingosdk_content_cache STATIC Engine/Vfs/content_cache.cpp Engine/Vfs/content_catalogs.cpp)
target_link_libraries(dingosdk_content_cache PUBLIC dingosdk_json)
if(WIN32)
    add_library(dingosdk_content_cache_install STATIC Engine/Vfs/content_cache_install.cpp)
    target_link_libraries(dingosdk_content_cache_install PUBLIC dingosdk_content_cache
        PRIVATE dingosdk_launcher_support dingosdk_https dingosdk_miniz)
endif()
# World layers read from the installed level data, cached per game build.
add_library(dingosdk_world_layer_scan STATIC Engine/Vfs/world_layer_scan.cpp)
target_link_libraries(dingosdk_world_layer_scan PUBLIC dingosdk_json PRIVATE dingosdk_game_archives dingosdk_content_cache)
