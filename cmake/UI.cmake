# DualShock 4 / DualSense read straight from HID, for the in-game menu and the launcher.
add_library(dingosdk_playstation_input STATIC Extension/UI/Overlay/playstation_input.cpp)
target_link_libraries(dingosdk_playstation_input PUBLIC dingosdk_logging PRIVATE hid cfgmgr32)

add_library(dingosdk_overlay STATIC

    Extension/UI/Overlay/overlay.cpp
    Extension/UI/Overlay/overlay_render.cpp
    Extension/UI/Overlay/overlay_console.cpp
    Extension/UI/Overlay/overlay_input.cpp
    Extension/UI/Overlay/overlay_notices.cpp
    Extension/UI/Overlay/chat_overlay.cpp
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
    Extension/UI/Overlay/skate_menu_skater.cpp
    Extension/UI/Overlay/skate_menu_settings.cpp
    Extension/UI/Overlay/park_editor.cpp
    Extension/UI/Overlay/park_editor_actions.cpp
    Extension/UI/Overlay/park_editor_panels.cpp
    Extension/UI/Overlay/park_editor_viewport.cpp
    Extension/UI/Overlay/park_previews.cpp
    Extension/UI/Overlay/gpu_diagnostics.cpp
    Extension/UI/Overlay/input_capture.cpp
    Extension/UI/Overlay/cursor.cpp
    Extension/UI/Overlay/progression_menu.cpp
    Extension/UI/Overlay/atmosphere_menu.cpp
    Extension/UI/Overlay/modding_menu.cpp
    Extension/UI/Overlay/multiplayer_menu.cpp
    Extension/UI/Overlay/multiplayer_lobbies.cpp
    Extension/UI/Overlay/multiplayer_session.cpp
    Extension/Trainer/trainer_page.cpp
    Extension/Trainer/trainer_view.cpp
    Extension/HallOfMeat/hall_of_meat_overlay.cpp
)
target_link_libraries(dingosdk_overlay PUBLIC dingosdk_logging dingosdk_profiler dingosdk_imgui dingosdk_hooks dingosdk_console_core dxguid PRIVATE dingosdk_playstation_input shell32 dingosdk_initfs dingosdk_custom_scripts dingosdk_game_archives dingosdk_mods dingosdk_json windowscodecs ole32)

# The window shown from the moment ReSkate loads until the game's own window appears.
add_library(dingosdk_startup_window STATIC Extension/UI/Startup/startup_window.cpp)
target_link_libraries(dingosdk_startup_window PRIVATE gdi32 user32 msimg32 shell32 ole32 windowscodecs)
