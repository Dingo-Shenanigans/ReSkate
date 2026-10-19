add_library(dingosdk_steam_restart_guard STATIC
    Extension/Boot/steam_restart_guard.cpp
    Extension/Boot/offline_steam.cpp)

target_link_libraries(dingosdk_steam_restart_guard PUBLIC dingosdk_launcher_support dingosdk_hooks dingosdk_logging)
add_library(dingosdk_startup_interventions STATIC
    Extension/Boot/startup_interventions.cpp)

add_library(dingosdk_fast_travel_unlock STATIC
    Extension/Progression/fast_travel_unlock.cpp)

add_library(dingosdk_local_profile STATIC
    Extension/Profile/local_profile.cpp
    Extension/Profile/profile_defaults.cpp
    Extension/Profile/local_profile_store.cpp
    Extension/Profile/profile_update.cpp
    Extension/Profile/database_schema.cpp
    Extension/Profile/database_codec.cpp
    Extension/Objects/placement_database.cpp
    Extension/Objects/object_categories.cpp
    Extension/Profile/local_profile_legacy.cpp
    Extension/Customization/local_customization.cpp
    Extension/News/news_feed.cpp
    Extension/Progression/rip_score.cpp
    Extension/World/profile_policy.cpp
    Extension/Rendering/profile_policy.cpp
    Extension/Settings/input_bindings.cpp
    Extension/Progression/challenge_profile.cpp
    Extension/Objects/placements_store.cpp
    Extension/Objects/ParkEditor/park_document.cpp
    Extension/Objects/ParkEditor/park_mods.cpp
    Extension/Objects/ParkEditor/park_object_sets.cpp
)
target_link_libraries(dingosdk_local_profile PUBLIC dingosdk_fast_travel_unlock dingosdk_content_cache dingosdk_json dingosdk_storage dingosdk_mod_list)

include(cmake/ProfileData.cmake)

