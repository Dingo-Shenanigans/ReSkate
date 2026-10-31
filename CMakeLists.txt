cmake_minimum_required(VERSION 3.24)
project(DingoSDK LANGUAGES CXX C)
if(WIN32)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "DingoSDK on Windows requires MSVC and the x64 platform.")
    endif()
    # ReSkate uses no vcpkg packages. A user-wide "vcpkg integrate install" still hooks every
    # project and runs its DLL copy after each link, with PowerShell 7 ("'pwsh.exe' is not
    # recognized" where it is not installed); the preset's VCLibPackagePath only covers configure.
    set(CMAKE_VS_GLOBALS "VCPkgLocalAppDataDisabled=true")
else()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "DingoSDK on Linux requires a 64-bit platform.")
    endif()
    message(STATUS "Linux build: dedicated server only (launcher/runtime stay Windows-only).")
endif()

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/CustomScriptData.cmake)
include(cmake/Core.cmake)
if(WIN32)
    include(cmake/Features.cmake)
    include(cmake/UI.cmake)
    include(cmake/Runtime.cmake)
endif()
include(cmake/Apps.cmake)
if(WIN32)
    include(cmake/CrashReport.cmake)
endif()

option(DINGOSDK_BUILD_TESTS "Build logging regression tests" OFF)
option(DINGOSDK_BUILD_PARK_EDITOR_TESTS "Build park editor regression tests" OFF)
option(DINGOSDK_BUILD_TRAINER_TESTS "Build the trainer's tools and regression tests" OFF)
if(DINGOSDK_BUILD_PARK_EDITOR_TESTS)
    enable_testing()
    add_executable(dingosdk_park_randomization_tests Engine/Game/World/Test/park_randomization_tests.cpp)
    add_test(NAME park_randomization COMMAND dingosdk_park_randomization_tests)
endif()
if(DINGOSDK_BUILD_PARK_EDITOR_TESTS AND WIN32)
    enable_testing()
    add_executable(dingosdk_park_editor_tests Extension/Objects/ParkEditor/Test/park_editor_tests.cpp)
    target_link_libraries(dingosdk_park_editor_tests PRIVATE dingosdk_local_profile)
    add_test(NAME park_editor COMMAND dingosdk_park_editor_tests "${CMAKE_CURRENT_BINARY_DIR}/editor-fixtures")
    add_executable(dingosdk_park_editor_ui_tests Extension/UI/Overlay/Test/park_editor_tests.cpp
        Extension/UI/Overlay/park_editor.cpp Extension/UI/Overlay/park_editor_actions.cpp
        Extension/UI/Overlay/park_editor_panels.cpp Extension/UI/Overlay/park_editor_viewport.cpp Extension/UI/Overlay/park_previews.cpp)
    target_link_libraries(dingosdk_park_editor_ui_tests PRIVATE dingosdk_imgui dingosdk_json dingosdk_game_archives dingosdk_content_cache dingosdk_logging)
    add_test(NAME park_editor_ui COMMAND dingosdk_park_editor_ui_tests "${CMAKE_CURRENT_BINARY_DIR}/editor-preview" "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_park_editor_transaction_tests Extension/Objects/ParkEditor/Test/park_editor_transaction_tests.cpp)
    target_link_libraries(dingosdk_park_editor_transaction_tests PRIVATE dingosdk_local_profile dingosdk_logging dingosdk_hooks)
    add_test(NAME park_editor_transactions COMMAND dingosdk_park_editor_transaction_tests)
    add_executable(dingosdk_profile_migration_tests Extension/Profile/Test/profile_migration_tests.cpp)
    target_link_libraries(dingosdk_profile_migration_tests PRIVATE dingosdk_local_profile)
    add_test(NAME profile_migration COMMAND dingosdk_profile_migration_tests)
    add_executable(dingosdk_profile_change_tests Extension/Profile/Test/profile_change_tests.cpp)
    target_link_libraries(dingosdk_profile_change_tests PRIVATE dingosdk_local_profile)
    add_test(NAME profile_changes COMMAND dingosdk_profile_change_tests)
    add_executable(dingosdk_music_favorites_tests Extension/Music/Test/local_music_favorites_tests.cpp)
    target_link_libraries(dingosdk_music_favorites_tests PRIVATE dingosdk_local_profile)
    add_test(NAME music_favorites COMMAND dingosdk_music_favorites_tests)
    add_executable(dingosdk_object_categories_tests Extension/Objects/Test/object_categories_tests.cpp)
    target_link_libraries(dingosdk_object_categories_tests PRIVATE dingosdk_local_profile)
    add_test(NAME object_categories COMMAND dingosdk_object_categories_tests)
    # The park editor's native ray (Extension/Objects/ParkEditor/park_editor_surface.h) over a
    # constructed result: the editor's own rules and exclusions, and the counted cast behind
    # reskate.world.probe and the ray batches (clear, hit and malformed results, presets and
    # measured contact normals).
    add_executable(dingosdk_surface_probe_tests Extension/Objects/ParkEditor/Test/surface_probe_tests.cpp)
    add_test(NAME surface_probe COMMAND dingosdk_surface_probe_tests)
endif()
option(DINGOSDK_BUILD_MULTIPLAYER_TESTS "Build multiplayer protocol regression tests" OFF)
if(DINGOSDK_BUILD_MULTIPLAYER_TESTS)
    enable_testing()
    add_executable(dingosdk_map_fetch_policy_tests Extension/Multiplayer/Test/map_fetch_policy_tests.cpp)
    target_include_directories(dingosdk_map_fetch_policy_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME map_fetch_policy COMMAND dingosdk_map_fetch_policy_tests)
    if(WIN32)
        add_executable(dingosdk_https_download_timeout_tests Engine/Vfs/Test/https_download_timeout_tests.cpp)
        target_include_directories(dingosdk_https_download_timeout_tests PRIVATE "${PROJECT_SOURCE_DIR}")
        target_link_libraries(dingosdk_https_download_timeout_tests PRIVATE winhttp)
        add_test(NAME https_download_timeout COMMAND dingosdk_https_download_timeout_tests
            "${CMAKE_CURRENT_BINARY_DIR}/https-download-fixtures")
        add_executable(dingosdk_map_download_timeout_tests Extension/Assets/Test/map_download_timeout_tests.cpp
            Engine/Vfs/thunderstore_package.cpp)
        target_include_directories(dingosdk_map_download_timeout_tests PRIVATE "${PROJECT_SOURCE_DIR}")
        target_link_libraries(dingosdk_map_download_timeout_tests PRIVATE dingosdk_json)
        add_test(NAME map_download_timeout COMMAND dingosdk_map_download_timeout_tests
            "${CMAKE_CURRENT_BINARY_DIR}/map-download-fixtures")
        add_executable(dingosdk_voice_tests Extension/Multiplayer/Test/voice_chat_tests.cpp Extension/Multiplayer/Net/protocol.cpp
            Extension/Multiplayer/Net/delta_codec.cpp Extension/Multiplayer/Net/wire_codec.cpp)
        target_link_libraries(dingosdk_voice_tests PRIVATE dingosdk_lz4 dingosdk_zstd)
        add_test(NAME multiplayer_voice COMMAND dingosdk_voice_tests)
    endif()
    add_executable(dingosdk_party_tests Extension/Multiplayer/Test/party_tests.cpp Extension/Multiplayer/Session/party_book.cpp)
    target_include_directories(dingosdk_party_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME multiplayer_parties COMMAND dingosdk_party_tests)
    add_executable(dingosdk_server_activity_tests Server/Test/server_activity_tests.cpp Server/server_activity.cpp
        Extension/Throwdowns/throwdown_wire.cpp)
    target_include_directories(dingosdk_server_activity_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME server_activity COMMAND dingosdk_server_activity_tests)
    add_executable(dingosdk_server_text_tests Server/Test/server_text_tests.cpp)
    target_include_directories(dingosdk_server_text_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME server_text COMMAND dingosdk_server_text_tests)
    add_executable(dingosdk_speed_check_tests Server/Test/speed_check_tests.cpp)
    target_include_directories(dingosdk_speed_check_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME server_speed_check COMMAND dingosdk_speed_check_tests)
    add_executable(dingosdk_board_check_tests Server/Test/board_check_tests.cpp)
    target_include_directories(dingosdk_board_check_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME server_board_check COMMAND dingosdk_board_check_tests)
    add_executable(dingosdk_server_config_tests Server/Test/server_config_tests.cpp Server/server_config.cpp
        Extension/Multiplayer/Net/protocol.cpp)
    target_include_directories(dingosdk_server_config_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(dingosdk_server_config_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_logging)
    else()
        target_link_libraries(dingosdk_server_config_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd)
    endif()
    add_test(NAME server_config COMMAND dingosdk_server_config_tests)
    # Park mods spawned by the server's host, over a transport that is not Steam's.
    add_executable(dingosdk_server_park_tests Server/Test/server_park_tests.cpp Server/server_host.cpp Server/server_party.cpp
        Extension/Multiplayer/Session/party_book.cpp Server/server_config.cpp Server/server_update.cpp Server/server_release.cpp
        $<$<BOOL:${WIN32}>:Launcher/updater.cpp> Server/global_bans.cpp Server/server_words.cpp Extension/Multiplayer/developer_identity.cpp
        Extension/Multiplayer/word_lists.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Net/delta_codec.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Session/password.cpp Server/server_activity.cpp Server/server_votes.cpp Server/server_commands.cpp
        Server/server_parks.cpp Extension/Throwdowns/throwdown_wire.cpp Server/server_plugins.cpp)
    target_include_directories(dingosdk_server_park_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_include_directories(dingosdk_server_park_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/External/steam_networking")
    if(WIN32)
        target_link_libraries(dingosdk_server_park_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_logging dingosdk_miniz dingosdk_word_filter dingosdk_https winhttp bcrypt winmm
            dingosdk_plugins)
    else()
        target_link_libraries(dingosdk_server_park_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_miniz dingosdk_word_filter OpenSSL::Crypto dingosdk_plugins dl pthread)
    endif()
    add_test(NAME server_park_mods COMMAND dingosdk_server_park_tests "${PROJECT_SOURCE_DIR}/Server/Test/fixtures/park_mods")
    # Tools locked for single players (lock, throwdown_locks), as each player's game reads its roster.
    add_executable(dingosdk_server_lock_tests Server/Test/server_lock_tests.cpp Server/server_host.cpp Server/server_party.cpp
        Extension/Multiplayer/Session/party_book.cpp Server/server_config.cpp Server/server_update.cpp Server/server_release.cpp
        $<$<BOOL:${WIN32}>:Launcher/updater.cpp> Server/global_bans.cpp Server/server_words.cpp Extension/Multiplayer/developer_identity.cpp
        Extension/Multiplayer/word_lists.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Net/delta_codec.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Session/password.cpp Server/server_activity.cpp Server/server_votes.cpp Server/server_commands.cpp
        Server/server_parks.cpp Extension/Throwdowns/throwdown_wire.cpp Server/server_plugins.cpp)
    target_include_directories(dingosdk_server_lock_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_include_directories(dingosdk_server_lock_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/External/steam_networking")
    if(WIN32)
        target_link_libraries(dingosdk_server_lock_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_logging dingosdk_miniz dingosdk_word_filter dingosdk_https winhttp bcrypt winmm
            dingosdk_plugins)
    else()
        target_link_libraries(dingosdk_server_lock_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_miniz dingosdk_word_filter OpenSSL::Crypto dingosdk_plugins dl pthread)
    endif()
    add_test(NAME server_locks COMMAND dingosdk_server_lock_tests)
    # What the server's plugins see of the server and do there: its players' skaters, its map, its park mods.
    add_executable(dingosdk_server_plugin_tests Server/Test/server_plugin_tests.cpp Server/server_host.cpp Server/server_party.cpp
        Extension/Multiplayer/Session/party_book.cpp Server/server_config.cpp Server/server_update.cpp Server/server_release.cpp
        $<$<BOOL:${WIN32}>:Launcher/updater.cpp> Server/global_bans.cpp Server/server_words.cpp Extension/Multiplayer/developer_identity.cpp
        Extension/Multiplayer/word_lists.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Net/delta_codec.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Session/password.cpp Server/server_activity.cpp Server/server_votes.cpp Server/server_commands.cpp
        Server/server_parks.cpp Extension/Throwdowns/throwdown_wire.cpp Server/server_plugins.cpp)
    target_include_directories(dingosdk_server_plugin_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_include_directories(dingosdk_server_plugin_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/External/steam_networking")
    if(WIN32)
        target_link_libraries(dingosdk_server_plugin_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_logging dingosdk_miniz dingosdk_word_filter dingosdk_https winhttp bcrypt winmm
            dingosdk_plugins)
    else()
        target_link_libraries(dingosdk_server_plugin_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
            dingosdk_lz4 dingosdk_zstd dingosdk_miniz dingosdk_word_filter OpenSSL::Crypto dingosdk_plugins dl pthread)
    endif()
    add_test(NAME server_plugins COMMAND dingosdk_server_plugin_tests "${PROJECT_SOURCE_DIR}/Server/Test/fixtures/park_mods")
    add_executable(dingosdk_server_release_tests Server/Test/server_release_tests.cpp Server/server_release.cpp)
    target_include_directories(dingosdk_server_release_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_server_release_tests PRIVATE dingosdk_json)
    add_test(NAME server_release COMMAND dingosdk_server_release_tests)
    add_executable(dingosdk_global_bans_tests Server/Test/global_bans_tests.cpp Server/global_bans.cpp
        Extension/Multiplayer/developer_identity.cpp Extension/Multiplayer/word_lists.cpp)
    target_include_directories(dingosdk_global_bans_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_global_bans_tests PRIVATE dingosdk_json dingosdk_word_filter $<$<BOOL:${WIN32}>:dingosdk_https>)
    add_test(NAME server_global_bans COMMAND dingosdk_global_bans_tests)
    add_executable(dingosdk_server_words_tests Server/Test/server_words_tests.cpp Server/server_words.cpp)
    target_include_directories(dingosdk_server_words_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_server_words_tests PRIVATE dingosdk_word_filter)
    add_test(NAME server_words COMMAND dingosdk_server_words_tests)
    add_executable(dingosdk_server_map_update_tests Server/Test/server_map_update_tests.cpp Server/server_map_update.cpp
        Engine/Vfs/thunderstore_package.cpp)
    target_include_directories(dingosdk_server_map_update_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_server_map_update_tests PRIVATE dingosdk_json)
    if(WIN32)
        target_link_libraries(dingosdk_server_map_update_tests PRIVATE dingosdk_https)
    endif()
    add_test(NAME server_map_update COMMAND dingosdk_server_map_update_tests)
    if(WIN32)
        # By hand: what the music player outside the game is playing (Extension/Music/external_player.h).
        add_executable(dingosdk_external_player_probe Extension/Music/Test/external_player_probe.cpp Extension/Music/external_player.cpp
            Extension/Music/Test/external_player_probe_stubs.cpp)
        target_include_directories(dingosdk_external_player_probe PRIVATE "${PROJECT_SOURCE_DIR}")
        add_executable(dingosdk_app_audio_probe Extension/Music/Test/app_audio_probe.cpp Extension/Music/app_audio_capture.cpp)
        target_include_directories(dingosdk_app_audio_probe PRIVATE "${PROJECT_SOURCE_DIR}")
    endif()
    add_executable(dingosdk_word_filter_tests Engine/Core/Text/Test/word_filter_tests.cpp)
    target_link_libraries(dingosdk_word_filter_tests PRIVATE dingosdk_word_filter)
    add_test(NAME word_filter COMMAND dingosdk_word_filter_tests)
    add_executable(dingosdk_multiplayer_bandwidth_tests
        Extension/Multiplayer/Test/bandwidth_tests.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Net/delta_codec.cpp)
    target_link_libraries(dingosdk_multiplayer_bandwidth_tests PRIVATE dingosdk_lz4 dingosdk_zstd)
    add_test(NAME multiplayer_bandwidth COMMAND dingosdk_multiplayer_bandwidth_tests)
    # With a pose-dump recording as its argument it prints what each encoding costs on real play.
    add_executable(dingosdk_pose_codec_tests
        Extension/Multiplayer/Test/pose_codec_tests.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Net/delta_codec.cpp)
    target_link_libraries(dingosdk_pose_codec_tests PRIVATE dingosdk_lz4 dingosdk_zstd)
    add_test(NAME multiplayer_pose_codec COMMAND dingosdk_pose_codec_tests)
    add_executable(dingosdk_multiplayer_lane_tests Extension/Multiplayer/Test/steam_lanes_tests.cpp)
    target_include_directories(dingosdk_multiplayer_lane_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/External/steam_networking")
    add_test(NAME multiplayer_lanes COMMAND dingosdk_multiplayer_lane_tests)
    add_executable(dingosdk_multiplayer_prediction_tests
        Extension/Multiplayer/Test/prediction_tests.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Remote/playback_buffers.cpp)
    add_test(NAME multiplayer_prediction COMMAND dingosdk_multiplayer_prediction_tests)
endif()
if(DINGOSDK_BUILD_MULTIPLAYER_TESTS AND WIN32)
    add_executable(dingosdk_velocity_boost_tests Engine/Game/Skater/Test/velocity_boost_tests.cpp)
    add_test(NAME velocity_boost COMMAND dingosdk_velocity_boost_tests)
    if(EXISTS "${PROJECT_SOURCE_DIR}/tests/first_person_spring_tests.cpp")
        add_executable(dingosdk_first_person_spring_tests tests/first_person_spring_tests.cpp)
        target_include_directories(dingosdk_first_person_spring_tests PRIVATE "${PROJECT_SOURCE_DIR}")
        add_test(NAME first_person_spring COMMAND dingosdk_first_person_spring_tests)
    endif()
    add_executable(dingosdk_first_person_stabilize_tests Engine/Game/Skater/Test/first_person_stabilize_tests.cpp)
    target_include_directories(dingosdk_first_person_stabilize_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME first_person_stabilize COMMAND dingosdk_first_person_stabilize_tests)
    add_executable(dingosdk_first_person_cycle_tests Engine/Game/Skater/Test/first_person_cycle_tests.cpp)
    target_include_directories(dingosdk_first_person_cycle_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME first_person_cycle COMMAND dingosdk_first_person_cycle_tests)
    add_executable(dingosdk_playstation_report_tests Engine/Game/Input/Test/playstation_report_tests.cpp)
    target_include_directories(dingosdk_playstation_report_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME playstation_report COMMAND dingosdk_playstation_report_tests)
    add_executable(dingosdk_scripted_pad_tests Extension/UI/Overlay/Test/scripted_pad_tests.cpp
        Extension/UI/Overlay/scripted_pad.cpp)
    target_include_directories(dingosdk_scripted_pad_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME scripted_pad COMMAND dingosdk_scripted_pad_tests)
    add_executable(dingosdk_physics_tuning_tests Extension/Skater/Test/physics_tuning_tests.cpp
        Extension/Skater/physics_tuning_model.cpp)
    target_include_directories(dingosdk_physics_tuning_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_physics_tuning_tests PRIVATE dingosdk_game_archives)
    add_test(NAME physics_tuning COMMAND dingosdk_physics_tuning_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_camera_observer_tests Extension/Skater/Test/camera_observer_tests.cpp)
    add_test(NAME camera_observer_recovery COMMAND dingosdk_camera_observer_tests)
    add_executable(dingosdk_linear_transform_tests Engine/Game/Abi/Test/linear_transform_tests.cpp)
    target_include_directories(dingosdk_linear_transform_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME linear_transform COMMAND dingosdk_linear_transform_tests)
    add_executable(dingosdk_draw_packet_tests Engine/Game/Rendering/Test/draw_packet_tests.cpp)
    target_include_directories(dingosdk_draw_packet_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME draw_packet COMMAND dingosdk_draw_packet_tests)
    add_executable(dingosdk_skinned_mesh_tests Engine/Resource/Test/skinned_mesh_tests.cpp)
    target_include_directories(dingosdk_skinned_mesh_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_skinned_mesh_tests PRIVATE dingosdk_frostbite)
    add_test(NAME skinned_mesh COMMAND dingosdk_skinned_mesh_tests)
    add_executable(dingosdk_game_textures_tests Engine/Vfs/Test/game_textures_tests.cpp)
    target_include_directories(dingosdk_game_textures_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_game_textures_tests PRIVATE dingosdk_game_archives)
    add_test(NAME game_textures COMMAND dingosdk_game_textures_tests "${DINGOSDK_TEST_GAME_ROOT}")
    add_executable(dingosdk_native_menu_view_tests Extension/UI/NativeMenu/Test/native_menu_view_tests.cpp)
    add_executable(dingosdk_loading_screen_tests Extension/World/Test/loading_screen_tests.cpp)
    target_include_directories(dingosdk_loading_screen_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME loading_screen_selection COMMAND dingosdk_loading_screen_tests)
    add_test(NAME native_menu_browser COMMAND dingosdk_native_menu_view_tests)
    add_executable(dingosdk_native_tools_view_tests Extension/UI/NativeMenu/Test/native_tools_view_tests.cpp)
    target_include_directories(dingosdk_native_tools_view_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME native_menu_tools COMMAND dingosdk_native_tools_view_tests)
    add_executable(dingosdk_native_menu_lifetime_tests Extension/UI/NativeMenu/Test/native_menu_lifetime_tests.cpp)
    add_test(NAME native_menu_lifetime COMMAND dingosdk_native_menu_lifetime_tests)
    add_executable(dingosdk_native_callback_tests Engine/Game/Abi/Test/native_callback_queue_tests.cpp)
    target_include_directories(dingosdk_native_callback_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME native_callback_lifetime COMMAND dingosdk_native_callback_tests)
    add_executable(dingosdk_steam_friend_join_tests Extension/Multiplayer/Test/steam_friend_join_tests.cpp)
    add_test(NAME steam_friend_join COMMAND dingosdk_steam_friend_join_tests)
    add_executable(dingosdk_multiplayer_tests
        Extension/Multiplayer/Test/protocol_tests.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Steam/steam_server_browser.cpp Server/steam_server.cpp
        Extension/Throwdowns/throwdown_wire.cpp Extension/Multiplayer/developer_identity.cpp)
    target_link_libraries(dingosdk_multiplayer_tests PRIVATE dingosdk_lz4 dingosdk_zstd dingosdk_logging dingosdk_word_filter dingosdk_json)
    add_test(NAME multiplayer_protocol COMMAND dingosdk_multiplayer_tests)
    add_executable(dingosdk_throwdown_relay_tests Extension/Throwdowns/Test/throwdown_relay_tests.cpp
        Extension/Throwdowns/throwdown_relay.cpp Extension/Throwdowns/throwdown_wire.cpp Extension/Throwdowns/custom_modes.cpp)
    target_include_directories(dingosdk_throwdown_relay_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_throwdown_relay_tests PRIVATE dingosdk_logging)
    add_test(NAME throwdown_relay COMMAND dingosdk_throwdown_relay_tests)
    add_executable(dingosdk_custom_throwdown_tests Extension/Throwdowns/Test/custom_throwdown_tests.cpp
        Extension/Throwdowns/custom_modes.cpp)
    target_include_directories(dingosdk_custom_throwdown_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME custom_throwdowns COMMAND dingosdk_custom_throwdown_tests)
    add_executable(dingosdk_multiplayer_native_tests
        Extension/Multiplayer/Test/native_layout_tests.cpp Extension/Multiplayer/Net/protocol.cpp)
    add_test(NAME multiplayer_native_layout COMMAND dingosdk_multiplayer_native_tests)
    add_executable(dingosdk_multiplayer_lobby_tests
        Extension/Multiplayer/Test/lobby_tests.cpp Extension/Multiplayer/Steam/steam_lobbies.cpp
        Extension/Multiplayer/Net/protocol.cpp)
    target_link_libraries(dingosdk_multiplayer_lobby_tests PRIVATE dingosdk_word_filter)
    add_test(NAME multiplayer_lobbies COMMAND dingosdk_multiplayer_lobby_tests)
    add_executable(dingosdk_multiplayer_mesh_tests
        Extension/Multiplayer/Test/mesh_tests.cpp Extension/Multiplayer/Net/protocol.cpp Extension/Multiplayer/Remote/playback_buffers.cpp
        Extension/Multiplayer/Net/wire_codec.cpp Extension/Multiplayer/Net/delta_codec.cpp
        Extension/Multiplayer/Session/password.cpp Extension/Multiplayer/Session/party_book.cpp
        Extension/Multiplayer/Steam/steam_lobbies.cpp Extension/Multiplayer/Steam/steam_server_browser.cpp
        Extension/Multiplayer/word_lists.cpp Extension/AnimationStudio/anim_core.cpp)
    target_link_libraries(dingosdk_multiplayer_mesh_tests PRIVATE dingosdk_logging dingosdk_launcher_support dingosdk_lz4 dingosdk_zstd bcrypt
        dingosdk_word_filter dingosdk_json)
    add_test(NAME multiplayer_mesh COMMAND dingosdk_multiplayer_mesh_tests)
    add_executable(dingosdk_developer_identity_tests
        Extension/Multiplayer/Test/developer_identity_tests.cpp Extension/Multiplayer/developer_identity.cpp
        Extension/Multiplayer/developer_identity_fetch.cpp Extension/Multiplayer/word_lists.cpp)
    target_link_libraries(dingosdk_developer_identity_tests PRIVATE dingosdk_json dingosdk_https dingosdk_logging dingosdk_word_filter winhttp)
    add_test(NAME developer_identity COMMAND dingosdk_developer_identity_tests)
    add_executable(dingosdk_settings_lock_tests Engine/Game/Settings/Test/multiplayer_settings_lock_tests.cpp)
    target_include_directories(dingosdk_settings_lock_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME multiplayer_settings_lock COMMAND dingosdk_settings_lock_tests)
    add_executable(dingosdk_simulation_time_tests Engine/Game/Settings/Test/simulation_time_tests.cpp)
    target_include_directories(dingosdk_simulation_time_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME simulation_time COMMAND dingosdk_simulation_time_tests)
    add_executable(dingosdk_held_input_tests Extension/UI/Overlay/Test/held_input_tests.cpp)
    add_test(NAME overlay_held_input COMMAND dingosdk_held_input_tests)
    add_executable(dingosdk_ea_service_block_tests Extension/Boot/Test/ea_service_block_tests.cpp)
    target_include_directories(dingosdk_ea_service_block_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME ea_service_block COMMAND dingosdk_ea_service_block_tests)
    add_executable(dingosdk_discord_presence_tests Extension/Boot/Test/discord_presence_tests.cpp Extension/Boot/discord_presence.cpp)
    target_include_directories(dingosdk_discord_presence_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_discord_presence_tests PRIVATE dingosdk_json dingosdk_logging)
    add_test(NAME discord_presence COMMAND dingosdk_discord_presence_tests)
    # The dedicated server's host under a full server of simulated players, over a transport
    # that is not Steam's: what its passes cost, and a hash of what it sent (not run by ctest).
    add_executable(dingosdk_server_load_tests Server/Test/server_load_tests.cpp Server/server_host.cpp Server/server_party.cpp
        Extension/Multiplayer/Session/party_book.cpp Server/server_config.cpp
        Server/server_update.cpp Server/server_release.cpp Launcher/updater.cpp
        Server/global_bans.cpp Server/server_words.cpp Extension/Multiplayer/developer_identity.cpp Extension/Multiplayer/word_lists.cpp
        Extension/Multiplayer/Net/protocol.cpp
        Extension/Multiplayer/Net/delta_codec.cpp Extension/Multiplayer/Net/wire_codec.cpp
        Extension/Multiplayer/Remote/playback_buffers.cpp Extension/Multiplayer/Session/password.cpp
        Server/server_activity.cpp Server/server_votes.cpp Server/server_commands.cpp Server/server_parks.cpp Extension/Throwdowns/throwdown_wire.cpp)
    target_include_directories(dingosdk_server_load_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_include_directories(dingosdk_server_load_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/External/steam_networking")
    target_link_libraries(dingosdk_server_load_tests PRIVATE dingosdk_launcher_support dingosdk_world_layer_scan dingosdk_json
        dingosdk_lz4 dingosdk_zstd dingosdk_logging dingosdk_miniz dingosdk_word_filter dingosdk_https winhttp bcrypt winmm)
endif()
# The route and logging tests live in the local, untracked tests/ folder.
option(DINGOSDK_BUILD_ROUTE_TESTS "Build native route cycle regression tests" OFF)
if(DINGOSDK_BUILD_ROUTE_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/tests/route_cycle_tests.cpp")
    enable_testing()
    add_executable(dingosdk_route_cycle_tests tests/route_cycle_tests.cpp)
    target_include_directories(dingosdk_route_cycle_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME route_cycle_guard COMMAND dingosdk_route_cycle_tests)
    add_executable(dingosdk_route_endpoint_tests tests/route_endpoint_tests.cpp)
    target_link_libraries(dingosdk_route_endpoint_tests PRIVATE dingosdk_hooks)
    add_test(NAME route_endpoint_guard COMMAND dingosdk_route_endpoint_tests)
endif()
if(DINGOSDK_BUILD_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/tests/logging_tests.cpp")
    enable_testing()
    add_executable(dingosdk_logging_tests tests/logging_tests.cpp)
    target_link_libraries(dingosdk_logging_tests PRIVATE dingosdk_logging)
    add_test(NAME logging_regressions COMMAND dingosdk_logging_tests)
endif()

# Header-only game contracts belong to the runtime's subsystem tree.
if(WIN32)
    file(GLOB_RECURSE game_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/Engine/Game/*.h")
    target_sources(dingosdk_runtime PRIVATE ${game_headers})
endif()
if(DINGOSDK_BUILD_TRAINER_TESTS)
    enable_testing()
    add_executable(dingosdk_trainer_tuning_dump Extension/Trainer/Test/tuning_dump.cpp Extension/Skater/physics_tuning_model.cpp)
    target_include_directories(dingosdk_trainer_tuning_dump PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_trainer_tuning_dump PRIVATE dingosdk_game_archives)
    # Fails when the game's data stops naming a tuning value (the trainer's table comes from those names).
    add_test(NAME trainer_tuning_names COMMAND dingosdk_trainer_tuning_dump "${DINGOSDK_TEST_GAME_ROOT}")
    # What a session's host shares beyond its tuning: the encoding, and what a guest refuses.
    add_executable(dingosdk_trainer_session_tests Extension/Trainer/Test/session_extras_tests.cpp Extension/Trainer/trainer_session.cpp)
    target_include_directories(dingosdk_trainer_session_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME trainer_session_extras COMMAND dingosdk_trainer_session_tests)
    # Ground-safe arrivals: the downward ray, the landing watch, and the remembered map waypoint.
    add_executable(dingosdk_trainer_landing_tests Extension/Trainer/Test/landing_tests.cpp)
    target_include_directories(dingosdk_trainer_landing_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME trainer_landing COMMAND dingosdk_trainer_landing_tests)
    # Built-in presets: Hardcore goes at least as far as Realistic, class names are spelt right, and
    # the player's own presets never keep a built-in's name.
    add_executable(dingosdk_trainer_presets_tests Extension/Trainer/Test/presets_tests.cpp Extension/Trainer/trainer_presets.cpp)
    target_include_directories(dingosdk_trainer_presets_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME trainer_presets COMMAND dingosdk_trainer_presets_tests)
    add_executable(dingosdk_trainer_asset_list Extension/Trainer/Test/asset_list.cpp)
    target_include_directories(dingosdk_trainer_asset_list PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_trainer_asset_list PRIVATE dingosdk_game_archives)
    # The game's own clips read out of its data (needs the game: <Skate folder> [<clip file> <asset>]).
    add_executable(dingosdk_game_clips_tests Extension/AnimationStudio/Test/game_clips_tests.cpp
        Extension/AnimationStudio/game_clips.cpp Extension/AnimationStudio/anim_core.cpp)
    target_include_directories(dingosdk_game_clips_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_game_clips_tests PRIVATE dingosdk_game_archives dingosdk_json)
endif()
option(DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS "Build Animation Studio regression tests" OFF)
if(DINGOSDK_BUILD_ANIMATION_STUDIO_TESTS)
    enable_testing()
    # Keyframes, their blending, the profile and clip files, and joints hanging from their parents.
    add_executable(dingosdk_animation_studio_tests Extension/AnimationStudio/Test/anim_core_tests.cpp
        Extension/AnimationStudio/anim_core.cpp)
    target_include_directories(dingosdk_animation_studio_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_animation_studio_tests PRIVATE dingosdk_json)
    add_test(NAME animation_studio COMMAND dingosdk_animation_studio_tests)
    # An arm reaching (reskate.skater.reach) on a made-up pose.
    add_executable(dingosdk_arm_reach_tests Extension/Skater/Test/arm_reach_tests.cpp Extension/Skater/arm_reach.cpp
        Extension/AnimationStudio/anim_core.cpp)
    target_include_directories(dingosdk_arm_reach_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_arm_reach_tests PRIVATE dingosdk_json)
    add_test(NAME arm_reach COMMAND dingosdk_arm_reach_tests)
    # Something carried (reskate.skater.carry): held out, aimed, both hands, crouched with the feet kept.
    add_executable(dingosdk_carry_tests Extension/Skater/Test/carry_tests.cpp Extension/Skater/carry.cpp Extension/Skater/arm_reach.cpp
        Extension/AnimationStudio/anim_core.cpp)
    target_include_directories(dingosdk_carry_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_carry_tests PRIVATE dingosdk_json)
    add_test(NAME carry COMMAND dingosdk_carry_tests)
    # A joint set to a place (reskate.skater.pose's place): in a frame, its children kept, the order.
    add_executable(dingosdk_pose_place_tests Extension/Skater/Test/pose_place_tests.cpp Extension/Skater/pose_place.cpp
        Extension/AnimationStudio/anim_core.cpp)
    target_include_directories(dingosdk_pose_place_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_pose_place_tests PRIVATE dingosdk_json)
    add_test(NAME pose_place COMMAND dingosdk_pose_place_tests)
endif()
option(DINGOSDK_BUILD_REPLAY_KEYFRAMES_TESTS "Build replay keyframe smoothing regression tests" OFF)
if(DINGOSDK_BUILD_REPLAY_KEYFRAMES_TESTS)
    enable_testing()
    # The smoother curve: stock at 0, every key met, C2 at 1, and a framing segment the game's spline plays as
    # meant; the saved settings read back as written.
    add_executable(dingosdk_replay_keyframes_tests Extension/ReplayKeyframes/Test/replay_keyframes_tests.cpp
        Extension/ReplayKeyframes/replay_curve.cpp Extension/ReplayKeyframes/replay_settings.cpp)
    target_include_directories(dingosdk_replay_keyframes_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME replay_keyframes COMMAND dingosdk_replay_keyframes_tests)
endif()
option(DINGOSDK_BUILD_XRAY_TESTS "Build the skater x-ray's skeleton tests" OFF)
if(DINGOSDK_BUILD_XRAY_TESTS)
    enable_testing()
    # skate.'s own skeleton mesh, read from the game's data and posed (with a Skate folder).
    add_executable(dingosdk_xray_skeleton_tests Extension/Skater/Test/xray_skeleton_tests.cpp Extension/Skater/xray_skeleton.cpp)
    target_include_directories(dingosdk_xray_skeleton_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_xray_skeleton_tests PRIVATE dingosdk_game_archives dingosdk_logging)
    add_test(NAME xray_skeleton COMMAND dingosdk_xray_skeleton_tests "${DINGOSDK_TEST_GAME_ROOT}")
endif()
option(DINGOSDK_BUILD_CAPTURE_TESTS "Build screen capture regression tests" OFF)
if(DINGOSDK_BUILD_CAPTURE_TESTS AND TARGET dingosdk_capture)
    enable_testing()
    # Matting, resizing, the card, GIF / APNG / WebM files that decode back to what went in, and a
    # recording of a turning board found, cut out and saved (reskate.capture).
    add_executable(dingosdk_capture_tests Extension/Rendering/Capture/Test/capture_tests.cpp)
    target_link_libraries(dingosdk_capture_tests PRIVATE dingosdk_capture)
    add_test(NAME capture COMMAND dingosdk_capture_tests "${PROJECT_SOURCE_DIR}/External/fonts")
endif()
option(DINGOSDK_BUILD_PLUGIN_TESTS "Build plugin host regression tests" OFF)
if(DINGOSDK_BUILD_PLUGIN_TESTS)
    enable_testing()
    # The owned-body contracts behind reskate.world.physics (Engine/Game/World): definitions, the
    # command mailbox, ray batches and their budget, hit identity, measured contact and shape
    # categories, over constructed values and fake memory.
    foreach(test owned_body body_query body_contact_read body_shape_categories)
        add_executable(dingosdk_${test}_tests Engine/Game/World/Test/${test}_tests.cpp)
        target_include_directories(dingosdk_${test}_tests PRIVATE "${PROJECT_SOURCE_DIR}")
        add_test(NAME ${test} COMMAND dingosdk_${test}_tests)
    endforeach()
    # The plugin host apart from the game: manifests, the reskate API, storage, modules, hot reload,
    # and the sandbox against plugins written to break it. These build on Linux too, so they can be
    # run without Windows.
    add_executable(dingosdk_plugin_tests Extension/Plugins/Test/plugin_tests.cpp)
    target_include_directories(dingosdk_plugin_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_plugin_tests PRIVATE dingosdk_plugins)
    if(NOT WIN32)
        # The Linux plugins library leaves out finding plugins among the mods; the tests need it.
        target_sources(dingosdk_plugin_tests PRIVATE Extension/Plugins/plugin_discovery.cpp Engine/Vfs/mod_list.cpp)
    endif()
    # It also runs the test plugins in contrib/plugins.
    target_compile_definitions(dingosdk_plugin_tests PRIVATE "RESKATE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
    add_test(NAME plugins COMMAND dingosdk_plugin_tests)
    # reskate.viewmodel's file and clips (Extension/UI/Overlay/viewmodel_model.h), and the FPS plugin's
    # exported pistol when contrib/plugins has it.
    add_executable(dingosdk_viewmodel_tests Extension/UI/Overlay/Test/viewmodel_model_tests.cpp Extension/UI/Overlay/viewmodel_model.cpp)
    target_include_directories(dingosdk_viewmodel_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME viewmodel COMMAND dingosdk_viewmodel_tests "${PROJECT_SOURCE_DIR}/contrib/plugins/fps/viewmodel/pistol.rsvm")
endif()
if(DINGOSDK_BUILD_PLUGIN_TESTS AND WIN32)
    # The owned-body service itself (Extension/World/owned_bodies.cpp), over a constructed local
    # client, physics world and TLS: ray batches, measured contact, presets, categories and
    # preparation. The test includes owned_bodies.cpp once and defines the hook, No Bail, session
    # and ray functions it calls; do not add a second object for any of them.
    add_executable(dingosdk_owned_body_source_tests Extension/World/Test/owned_body_source_tests.cpp)
    target_include_directories(dingosdk_owned_body_source_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_options(dingosdk_owned_body_source_tests PRIVATE /Gy /Gw)
    target_link_options(dingosdk_owned_body_source_tests PRIVATE /OPT:REF)
    add_test(NAME owned_body_source COMMAND dingosdk_owned_body_source_tests)
    # How often reskate.entities.bounds may search every asset domain (world_entities_contract.h).
    add_executable(dingosdk_world_entities_contract_tests Extension/World/Test/world_entities_contract_tests.cpp)
    target_include_directories(dingosdk_world_entities_contract_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME world_entities_contract COMMAND dingosdk_world_entities_contract_tests)
    set_tests_properties(owned_body_source PROPERTIES TIMEOUT 15)
    add_executable(dingosdk_effect_geometry_tests Extension/Rendering/Test/effect_geometry_tests.cpp
        Extension/Skater/skater_physics.cpp Extension/Plugins/plugin_effects.cpp)
    target_include_directories(dingosdk_effect_geometry_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_effect_geometry_tests PRIVATE dingosdk_logging)
    add_test(NAME effect_geometry COMMAND dingosdk_effect_geometry_tests)
    add_executable(dingosdk_effect_lifetime_tests Extension/Rendering/Test/effect_lifetime_tests.cpp
        Extension/Plugins/plugin_effects.cpp)
    target_include_directories(dingosdk_effect_lifetime_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_effect_lifetime_tests PRIVATE dingosdk_logging)
    add_test(NAME effect_lifetime COMMAND dingosdk_effect_lifetime_tests)
    # Tricks read from the score HUD's values (Extension/Skater/trick_watch.h), what plugins hear.
    add_executable(dingosdk_trick_watch_tests Extension/Skater/Test/trick_watch_tests.cpp Extension/Skater/trick_watch.cpp)
    target_include_directories(dingosdk_trick_watch_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME trick_watch COMMAND dingosdk_trick_watch_tests)
    # The board's shader parameters (Extension/Customization/board_material.h): the walk over a board
    # laid out as the game holds it, what plugins may set, and holding and putting back.
    add_executable(dingosdk_board_material_tests Extension/Customization/Test/board_material_tests.cpp)
    target_include_directories(dingosdk_board_material_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME board_material COMMAND dingosdk_board_material_tests)
    # What a catalog category is worn as (Extension/Customization/outfit_kind.h, reskate.outfit.worn).
    add_executable(dingosdk_outfit_kind_tests Extension/Customization/Test/outfit_kind_tests.cpp)
    target_include_directories(dingosdk_outfit_kind_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME outfit_kind COMMAND dingosdk_outfit_kind_tests)
endif()
get_property(project_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS project_targets)
    # (A build step, such as packing the built-in content, compiles nothing.)
    get_target_property(target_kind ${target} TYPE)
    if(target_kind STREQUAL "UTILITY")
        continue()
    endif()
    if(NOT target MATCHES "^(dingosdk_imgui|dingosdk_detours|dingosdk_sqlite|dingosdk_lz4|dingosdk_zstd|dingosdk_miniz|dingosdk_luau|dingosdk_spotify_device)$")
        dingosdk_configure_target(${target})
    endif()
endforeach()
