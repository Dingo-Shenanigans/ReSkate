# DualShock 4 / DualSense read straight from HID, for the in-game menu and the launcher.
add_library(dingosdk_playstation_input STATIC Extension/UI/Overlay/playstation_input.cpp)
target_link_libraries(dingosdk_playstation_input PUBLIC dingosdk_logging PRIVATE hid cfgmgr32)

add_library(dingosdk_overlay STATIC

    Extension/UI/Overlay/overlay.cpp
    Extension/UI/Overlay/overlay_render.cpp
    Extension/UI/Overlay/overlay_console.cpp
    Extension/UI/Overlay/overlay_input.cpp
    Extension/UI/Overlay/item_browser_overlay.cpp
    Extension/UI/Overlay/frame_grab.cpp
    Extension/UI/Overlay/depth_probe.cpp
    Extension/UI/Overlay/material_capture.cpp
    Extension/UI/Overlay/material_transmission.cpp
    Extension/UI/Overlay/transmission_config.cpp
    Extension/UI/Overlay/overlay_notices.cpp
    Extension/UI/Overlay/chat_overlay.cpp
    Extension/UI/Overlay/map_download_card.cpp
    Extension/UI/Overlay/message_card.cpp
    Extension/UI/Overlay/hub_page.cpp
    Extension/UI/Overlay/game_text_overlay.cpp
    Extension/UI/Overlay/perf_overlay.cpp
    Extension/UI/Overlay/skate_hud_overlay.cpp
    Extension/UI/Overlay/nametag_overlay.cpp
    Extension/UI/Overlay/chat_emotes.cpp
    Extension/UI/Overlay/chat_rich.cpp
    Extension/UI/Overlay/console_suggestions.cpp
    Extension/UI/Overlay/skate_menu.cpp
    Extension/UI/Overlay/skate_menu_world.cpp
    Extension/UI/Overlay/skate_menu_addons.cpp
    Extension/UI/Overlay/plugins_page.cpp
    Extension/UI/Overlay/plugin_overlay.cpp
    Extension/UI/Overlay/scripted_pad.cpp
    Extension/UI/Overlay/game_images.cpp
    Extension/UI/Overlay/xray_overlay.cpp
    Extension/UI/Overlay/plugin_images.cpp
    Extension/UI/Overlay/store_icons.cpp
    Extension/UI/Overlay/menu_map_pictures.cpp
    Extension/UI/Overlay/skate_menu_skater.cpp
    Extension/UI/Overlay/skate_menu_settings.cpp
    Extension/UI/Overlay/park_editor.cpp
    Extension/UI/Overlay/park_editor_actions.cpp
    Extension/UI/Overlay/park_editor_panels.cpp
    Extension/UI/Overlay/park_editor_viewport.cpp
    Extension/UI/Overlay/park_previews.cpp
    Extension/UI/Overlay/gpu_diagnostics.cpp
    Extension/UI/Overlay/input_capture.cpp
    Extension/UI/Overlay/plugin_dialog.cpp
    Extension/UI/Overlay/cursor.cpp
    Extension/UI/Overlay/progression_menu.cpp
    Extension/UI/Overlay/atmosphere_menu.cpp
    Extension/UI/Overlay/modding_menu.cpp
    Extension/UI/Overlay/multiplayer_menu.cpp
    Extension/UI/Overlay/multiplayer_lobbies.cpp
    Extension/UI/Overlay/multiplayer_session.cpp
    Extension/Trainer/trainer_page.cpp
    Extension/Music/music_page.cpp
    Extension/Trainer/trainer_view.cpp
    Extension/UI/Overlay/screen_look.cpp
)
target_link_libraries(dingosdk_overlay PUBLIC dingosdk_logging dingosdk_profiler dingosdk_imgui dingosdk_hooks dingosdk_console_core dxguid PRIVATE dingosdk_playstation_input shell32 dingosdk_initfs dingosdk_custom_scripts dingosdk_game_archives dingosdk_mods dingosdk_json windowscodecs ole32)

# The screen look's shader (reskate.screen.look) is built into the DLL as text and compiled when it is
# first drawn. The build checks it with the Windows SDK's fxc when there is one, so a shader that would
# not compile in the game fails here instead.
set(screen_look_path "${PROJECT_SOURCE_DIR}/Extension/UI/Overlay/screen_look.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${screen_look_path}")
file(READ "${screen_look_path}" screen_look_source)
# MSVC takes at most about 16 KB in one string literal: the shader goes in as several, which the
# compiler joins.
string(LENGTH "${screen_look_source}" screen_look_length)
set(screen_look_chunks "")
set(screen_look_at 0)
while(screen_look_at LESS screen_look_length)
    string(SUBSTRING "${screen_look_source}" ${screen_look_at} 8000 screen_look_chunk)
    string(APPEND screen_look_chunks "R\"screen_hlsl(${screen_look_chunk})screen_hlsl\"\n")
    math(EXPR screen_look_at "${screen_look_at} + 8000")
endwhile()
configure_file(cmake/templates/screen_look_hlsl.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/screen_look_hlsl.h" @ONLY)
target_include_directories(dingosdk_overlay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(material_transmission_path "${PROJECT_SOURCE_DIR}/Extension/UI/Overlay/material_transmission.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${material_transmission_path}")
file(READ "${material_transmission_path}" material_transmission_source)
configure_file(cmake/templates/material_transmission_hlsl.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/material_transmission_hlsl.h" @ONLY)
if(WIN32)
    file(GLOB screen_look_fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    if(screen_look_fxc_candidates)
        list(SORT screen_look_fxc_candidates)
        list(REVERSE screen_look_fxc_candidates)
        list(GET screen_look_fxc_candidates 0 screen_look_fxc_newest)
        set(DINGOSDK_FXC "${screen_look_fxc_newest}" CACHE FILEPATH "fxc.exe that checks the screen look's shader")
    else()
        find_program(DINGOSDK_FXC fxc)
    endif()
    if(DINGOSDK_FXC)
        set(screen_look_checked "${CMAKE_CURRENT_BINARY_DIR}/generated/screen_look.checked")
        add_custom_command(OUTPUT "${screen_look_checked}"
            COMMAND "${DINGOSDK_FXC}" /nologo /T vs_5_0 /E vs_main /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/screen_look_vs.cso" "${screen_look_path}"
            COMMAND "${DINGOSDK_FXC}" /nologo /T ps_5_0 /E ps_main /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/screen_look_ps.cso" "${screen_look_path}"
            COMMAND "${DINGOSDK_FXC}" /nologo /T ps_5_0 /E ps_adapt /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/screen_look_iris.cso" "${screen_look_path}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${screen_look_checked}"
            DEPENDS "${screen_look_path}"
            COMMENT "Checking the screen look's shader with fxc"
            VERBATIM)
        add_custom_target(dingosdk_screen_look_check DEPENDS "${screen_look_checked}")
        add_dependencies(dingosdk_overlay dingosdk_screen_look_check)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/material_transmission.checked"
            COMMAND "${DINGOSDK_FXC}" /nologo /WX /T vs_5_0 /E vs_main /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/material_transmission_vs.cso" "${material_transmission_path}"
            COMMAND "${DINGOSDK_FXC}" /nologo /WX /T ps_5_0 /E ps_main /Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/material_transmission_ps.cso" "${material_transmission_path}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/generated/material_transmission.checked"
            DEPENDS "${material_transmission_path}" VERBATIM)
        add_custom_target(dingosdk_material_transmission_check DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/material_transmission.checked")
        add_dependencies(dingosdk_overlay dingosdk_material_transmission_check)
    endif()
endif()

# The window shown from the moment ReSkate loads until the game's own window appears.
add_library(dingosdk_startup_window STATIC Extension/UI/Startup/startup_window.cpp)
target_link_libraries(dingosdk_startup_window PRIVATE gdi32 user32 msimg32 shell32 ole32 windowscodecs)
