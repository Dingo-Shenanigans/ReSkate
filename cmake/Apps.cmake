if(WIN32)
    add_executable(dingosdk_launcher WIN32 Launcher/main.cpp Launcher/launch.cpp
        Launcher/gui.cpp Launcher/gui_launcher.cpp Launcher/gui_renderer.cpp Launcher/gui_home.cpp
        Launcher/gui_settings.cpp Launcher/gui_sign_in.cpp Launcher/gui_mods.cpp Launcher/gui_mods_browse.cpp
        Launcher/gui_gamepad.cpp Launcher/gamepad_input.cpp Launcher/gui_changelog.cpp Launcher/game_settings.cpp
        Launcher/updater.cpp Launcher/mod_manager.cpp Launcher/thunderstore.cpp Launcher/problem.h)
    target_link_libraries(dingosdk_launcher PRIVATE dingosdk_logging dingosdk_content_cache_install dingosdk_world_layer_scan dingosdk_launcher_support dingosdk_initfs
        dingosdk_mod_list dingosdk_mods dingosdk_json dingosdk_miniz dingosdk_imgui dingosdk_playstation_input winhttp shell32 dwmapi windowscodecs ole32)
    set_target_properties(dingosdk_launcher PROPERTIES OUTPUT_NAME "ReSkateLauncher")
    dingosdk_version_info(dingosdk_launcher "ReSkate launcher" "ReSkateLauncher.exe" VFT_APP)
endif()

option(DINGOSDK_BUILD_LAUNCHER_TESTS "Build launcher mod manager regression tests" OFF)
if(DINGOSDK_BUILD_LAUNCHER_TESTS AND WIN32)
    enable_testing()
    add_executable(dingosdk_mod_manager_tests Launcher/Test/mod_manager_tests.cpp Launcher/mod_manager.cpp)
    target_link_libraries(dingosdk_mod_manager_tests PRIVATE dingosdk_mod_list dingosdk_launcher_support dingosdk_miniz shell32)
    add_test(NAME launcher_mod_manager COMMAND dingosdk_mod_manager_tests)
    add_executable(dingosdk_thunderstore_tests Launcher/Test/thunderstore_tests.cpp Launcher/thunderstore.cpp)
    target_link_libraries(dingosdk_thunderstore_tests PRIVATE dingosdk_json dingosdk_miniz)
    target_include_directories(dingosdk_thunderstore_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME launcher_thunderstore COMMAND dingosdk_thunderstore_tests)
    add_executable(dingosdk_thunderstore_package_tests Engine/Vfs/Test/thunderstore_package_tests.cpp Engine/Vfs/thunderstore_package.cpp)
    target_link_libraries(dingosdk_thunderstore_package_tests PRIVATE dingosdk_json)
    target_include_directories(dingosdk_thunderstore_package_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME thunderstore_package COMMAND dingosdk_thunderstore_package_tests)
    add_executable(dingosdk_game_settings_tests Launcher/Test/game_settings_tests.cpp Launcher/game_settings.cpp)
    target_include_directories(dingosdk_game_settings_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME launcher_game_settings COMMAND dingosdk_game_settings_tests)
    add_executable(dingosdk_gamepad_input_tests Launcher/Test/gamepad_input_tests.cpp Launcher/gamepad_input.cpp)
    target_link_libraries(dingosdk_gamepad_input_tests PRIVATE dingosdk_imgui)
    target_include_directories(dingosdk_gamepad_input_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME launcher_gamepad_input COMMAND dingosdk_gamepad_input_tests)
    add_executable(dingosdk_depot_output_tests Launcher/Test/depot_output_tests.cpp)
    target_include_directories(dingosdk_depot_output_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME launcher_depot_output COMMAND dingosdk_depot_output_tests)
    add_executable(dingosdk_content_catalogs_tests Engine/Vfs/Test/content_catalogs_tests.cpp)
    target_link_libraries(dingosdk_content_catalogs_tests PRIVATE dingosdk_content_cache)
    add_test(NAME content_catalogs COMMAND dingosdk_content_catalogs_tests)
    add_executable(dingosdk_content_cache_install_tests Engine/Vfs/Test/content_cache_install_tests.cpp)
    target_link_libraries(dingosdk_content_cache_install_tests PRIVATE dingosdk_content_cache_install)
    add_test(NAME content_cache_install COMMAND dingosdk_content_cache_install_tests)
    add_executable(dingosdk_item_thumbnails_tests Engine/Vfs/Test/item_thumbnails_tests.cpp)
    target_link_libraries(dingosdk_item_thumbnails_tests PRIVATE dingosdk_game_archives)
    add_test(NAME item_thumbnails COMMAND dingosdk_item_thumbnails_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_world_layer_scan_tests Engine/Vfs/Test/world_layer_scan_tests.cpp)
    target_link_libraries(dingosdk_world_layer_scan_tests PRIVATE dingosdk_world_layer_scan)
    add_test(NAME world_layer_scan COMMAND dingosdk_world_layer_scan_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_mod_merge_grid_tests Engine/Vfs/Test/mod_merge_grid_tests.cpp)
    target_link_libraries(dingosdk_mod_merge_grid_tests PRIVATE dingosdk_mods)
    add_test(NAME mod_merge_grid COMMAND dingosdk_mod_merge_grid_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_mod_build_stamp_tests Engine/Vfs/Test/mod_build_stamp_tests.cpp)
    target_link_libraries(dingosdk_mod_build_stamp_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_mod_build_stamp_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME mod_build_stamp COMMAND dingosdk_mod_build_stamp_tests)
    add_executable(dingosdk_bundle_ref_table_tests Engine/Vfs/Test/bundle_ref_table_tests.cpp)
    target_link_libraries(dingosdk_bundle_ref_table_tests PRIVATE dingosdk_mods)
    add_test(NAME bundle_ref_table COMMAND dingosdk_bundle_ref_table_tests)
    add_executable(dingosdk_mod_scoring_tests Engine/Vfs/Test/mod_scoring_tests.cpp)
    target_link_libraries(dingosdk_mod_scoring_tests PRIVATE dingosdk_mods)
    add_test(NAME mod_scoring COMMAND dingosdk_mod_scoring_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_mod_music_tests Engine/Vfs/Test/mod_music_tests.cpp)
    target_link_libraries(dingosdk_mod_music_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_mod_music_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME mod_music COMMAND dingosdk_mod_music_tests)
    add_executable(dingosdk_music_safety_tests Extension/Music/Test/local_music_safety_tests.cpp)
    target_include_directories(dingosdk_music_safety_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    dingosdk_configure_target(dingosdk_music_safety_tests)
    add_test(NAME music_safety COMMAND dingosdk_music_safety_tests)
    add_executable(dingosdk_music_playback_policy_tests Extension/Music/Test/local_music_playback_policy_tests.cpp)
    target_include_directories(dingosdk_music_playback_policy_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    dingosdk_configure_target(dingosdk_music_playback_policy_tests)
    add_test(NAME music_playback_policy COMMAND dingosdk_music_playback_policy_tests)
    add_executable(dingosdk_music_shelf_lifetime_tests Extension/Music/Test/local_music_shelf_lifetime_tests.cpp)
    target_include_directories(dingosdk_music_shelf_lifetime_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    dingosdk_configure_target(dingosdk_music_shelf_lifetime_tests)
    add_test(NAME music_shelf_lifetime COMMAND dingosdk_music_shelf_lifetime_tests)
    add_executable(dingosdk_music_artwork_tests Extension/Music/Test/music_artwork_tests.cpp Extension/Music/music_artwork.cpp)
    target_link_libraries(dingosdk_music_artwork_tests PRIVATE dingosdk_mods ws2_32 winhttp)
    dingosdk_configure_target(dingosdk_music_artwork_tests)
    add_test(NAME music_artwork COMMAND dingosdk_music_artwork_tests)
    add_executable(dingosdk_mod_merge_unshift_tests Engine/Vfs/Test/mod_merge_unshift_tests.cpp)
    target_link_libraries(dingosdk_mod_merge_unshift_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_mod_merge_unshift_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME mod_merge_unshift COMMAND dingosdk_mod_merge_unshift_tests)
    add_executable(dingosdk_chunk_metadata_merge_tests Engine/Vfs/Test/chunk_metadata_merge_tests.cpp)
    target_link_libraries(dingosdk_chunk_metadata_merge_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_chunk_metadata_merge_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME chunk_metadata_merge COMMAND dingosdk_chunk_metadata_merge_tests)
    add_executable(dingosdk_mod_merge_added_assets_tests Engine/Vfs/Test/mod_merge_added_assets_tests.cpp)
    target_link_libraries(dingosdk_mod_merge_added_assets_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_mod_merge_added_assets_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME mod_merge_added_assets COMMAND dingosdk_mod_merge_added_assets_tests)
    add_executable(dingosdk_item_list_merge_tests Engine/Vfs/Test/item_list_merge_tests.cpp)
    target_link_libraries(dingosdk_item_list_merge_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_item_list_merge_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME item_list_merge COMMAND dingosdk_item_list_merge_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_ebx_merge_tests Engine/Vfs/Test/ebx_merge_tests.cpp)
    target_link_libraries(dingosdk_ebx_merge_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_ebx_merge_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME ebx_merge COMMAND dingosdk_ebx_merge_tests)
    add_executable(dingosdk_mod_merge_audit_tests Engine/Vfs/Test/mod_merge_audit_tests.cpp)
    target_link_libraries(dingosdk_mod_merge_audit_tests PRIVATE dingosdk_mods)
    target_include_directories(dingosdk_mod_merge_audit_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME mod_merge_audit COMMAND dingosdk_mod_merge_audit_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_store_copies_tests Engine/Vfs/Test/store_copies_tests.cpp)
    target_link_libraries(dingosdk_store_copies_tests PRIVATE dingosdk_mods dingosdk_content_cache)
    target_include_directories(dingosdk_store_copies_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME store_copies COMMAND dingosdk_store_copies_tests "${DINGOSDK_TEST_GAME_ROOT}")
endif()

if(WIN32)
    # Launcher fonts (OFL / Apache, licences beside them) and the background photo,
    # embedded when present. A ReSkateLauncher.background.jpg beside the exe
    # overrides the photo at runtime.
    set(launcher_fonts "${PROJECT_SOURCE_DIR}/External/fonts")
    set(launcher_resources
        "FONT_HEADING RCDATA \"${launcher_fonts}/Montserrat-ExtraBold.ttf\"\n"
        "FONT_BODY RCDATA \"${launcher_fonts}/Montserrat-SemiBold.ttf\"\n"
        "FONT_BRUSH RCDATA \"${launcher_fonts}/PermanentMarker-Regular.ttf\"\n")
    set(launcher_background "${PROJECT_SOURCE_DIR}/assets/launcher/background.jpg")
    if(EXISTS "${launcher_background}")
        list(APPEND launcher_resources "LAUNCHER_BACKGROUND RCDATA \"${launcher_background}\"\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${launcher_background}")
    endif()
    # Tile icons: the skate tool on MODS, the wheel on SETTINGS; and Thunderstore's mark for the
    # mod manager's Thunderstore button.
    foreach(tile_icon mods settings thunderstore)
        set(tile_icon_path "${PROJECT_SOURCE_DIR}/assets/launcher/icon_${tile_icon}.png")
        if(EXISTS "${tile_icon_path}")
            string(TOUPPER "${tile_icon}" tile_icon_name)
            list(APPEND launcher_resources "LAUNCHER_ICON_${tile_icon_name} RCDATA \"${tile_icon_path}\"\n")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tile_icon_path}")
        endif()
    endforeach()
    # Icon resource 1: the exe's icon in Explorer and the window's icon.
    set(launcher_icon "${PROJECT_SOURCE_DIR}/assets/launcher/icon.ico")
    if(EXISTS "${launcher_icon}")
        list(PREPEND launcher_resources "1 ICON \"${launcher_icon}\"\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${launcher_icon}")
    endif()
    string(CONCAT launcher_resources ${launcher_resources})
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_resources.rc" "${launcher_resources}")
    target_sources(dingosdk_launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_resources.rc")
endif()

# Off for local builds so a deployed development DLL is never replaced by a release.
option(DINGOSDK_LAUNCHER_AUTO_UPDATE "Let the launcher replace itself and ReSkate.dll from the launcher config" OFF)

# Updates come from the public GitHub releases of DINGOSDK_RELEASE_REPO: the launcher and
# server read launcher.json (game depot/manifest and the pinned downloads) from the latest
# release. No credentials are built in. Build folders configured before releases moved
# still cache the old private repo, which no public build can read.
if(DINGOSDK_RELEASE_REPO STREQUAL "Dingo-Shenanigans/DingoSDK")
    unset(DINGOSDK_RELEASE_REPO CACHE)
endif()
set(DINGOSDK_RELEASE_REPO "Dingo-Shenanigans/ReSkate" CACHE STRING "Public GitHub owner/repo whose releases update the launcher")
set(launcher_release_repo "${DINGOSDK_RELEASE_REPO}")
if(NOT launcher_release_repo MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "DINGOSDK_RELEASE_REPO must be owner/repo")
endif()
if(DINGOSDK_LAUNCHER_AUTO_UPDATE)
    set(launcher_binary_updates true)
else()
    set(launcher_binary_updates false)
endif()
configure_file(cmake/templates/launcher_update_config.h.in generated/launcher_update_config.h @ONLY)

if(WIN32)
    # ReSkateEmotePacker: a folder of pictures and GIFs -> the chat emote pack (emotes.json +
    # emotes.png) that ReSkate.dll bakes in from assets/emotes.
    add_executable(dingosdk_emote_packer EmotePacker/main.cpp)
    target_link_libraries(dingosdk_emote_packer PRIVATE dingosdk_json windowscodecs ole32)
    set_target_properties(dingosdk_emote_packer PROPERTIES OUTPUT_NAME "ReSkateEmotePacker")
endif()

# ReSkate dedicated server: a headless session host. It runs from its own folder
# next to steam_api64.dll and the Steam client files; no game install needed.
# On Linux next to libsteam_api.so; it updates itself there too, with the machine's curl and tar.
add_executable(dingosdk_server Server/main.cpp Server/server_host.cpp Server/server_party.cpp
    Extension/Multiplayer/Session/party_book.cpp Server/server_config.cpp Server/steam_server.cpp
    Server/server_update.cpp Server/server_release.cpp $<$<BOOL:${WIN32}>:Launcher/updater.cpp>
    Server/global_bans.cpp Extension/Multiplayer/developer_identity.cpp Extension/Multiplayer/word_lists.cpp
    Extension/Multiplayer/Steam/steam_transport.cpp Extension/Multiplayer/Net/protocol.cpp
    Extension/Multiplayer/Net/delta_codec.cpp Extension/Multiplayer/Net/wire_codec.cpp
    Extension/Multiplayer/Remote/playback_buffers.cpp Extension/Multiplayer/Session/password.cpp
    Server/server_activity.cpp Server/server_votes.cpp Server/server_commands.cpp Extension/Throwdowns/throwdown_wire.cpp)
target_include_directories(dingosdk_server SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/External/steam_networking")
if(WIN32)
    target_link_libraries(dingosdk_server PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
        dingosdk_lz4 dingosdk_zstd dingosdk_logging dingosdk_miniz dingosdk_word_filter dingosdk_https winhttp bcrypt winmm)
    set_target_properties(dingosdk_server PROPERTIES OUTPUT_NAME "ReSkateServer")
    dingosdk_version_info(dingosdk_server "ReSkate dedicated server" "ReSkateServer.exe" VFT_APP)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(dingosdk_server PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
        dingosdk_lz4 dingosdk_zstd dingosdk_miniz dingosdk_word_filter OpenSSL::Crypto dl pthread)
    set_target_properties(dingosdk_server PROPERTIES OUTPUT_NAME "ReSkateServer")
endif()
