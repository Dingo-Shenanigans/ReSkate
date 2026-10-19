# Settings and pure math imported from the recorded First Person reference.
# No bootstrap, renderer, stock DLL address contracts or input detours.
add_library(dingosdk_first_person_core STATIC
    Extension/Skater/FirstPerson/Upstream/src/config.cpp
    Extension/Skater/FirstPerson/Upstream/src/settings.cpp
    Extension/Skater/FirstPerson/Upstream/src/bail_collision.cpp)
target_include_directories(dingosdk_first_person_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(dingosdk_first_person_core PRIVATE dingosdk_json shell32 ole32)
target_compile_definitions(dingosdk_first_person_core PRIVATE _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
add_library(dingosdk_first_person_integration STATIC Extension/Skater/FirstPerson/integration.cpp
    Extension/Skater/FirstPerson/controls_context.cpp
    Extension/Skater/FirstPerson/native_spectator.cpp)
target_include_directories(dingosdk_first_person_integration PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(dingosdk_first_person_integration PRIVATE dingosdk_first_person_core dingosdk_logging)
target_link_libraries(dingosdk_overlay PRIVATE dingosdk_first_person_core dingosdk_first_person_integration)
target_link_libraries(dingosdk_runtime PRIVATE dingosdk_first_person_core dingosdk_first_person_integration)
if(DINGOSDK_BUILD_FIRST_PERSON_HOST_TESTS)
    enable_testing()
    add_executable(dingosdk_first_person_profile_queue_tests Extension/Skater/FirstPerson/Test/profile_queue_tests.cpp)
    target_include_directories(dingosdk_first_person_profile_queue_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME first_person_profile_queue COMMAND dingosdk_first_person_profile_queue_tests)
    foreach(suite IN ITEMS board_look glance_toggle look_smoothing shared_settings native_stick camera_transition native_glance native_actions native_comfort bail_collision)
        add_executable(dingosdk_first_person_${suite}_tests Extension/Skater/FirstPerson/Test/${suite}_tests.cpp)
        target_link_libraries(dingosdk_first_person_${suite}_tests PRIVATE dingosdk_first_person_core)
        target_compile_options(dingosdk_first_person_${suite}_tests PRIVATE /wd4456)
        add_test(NAME first_person_${suite} COMMAND dingosdk_first_person_${suite}_tests)
    endforeach()
endif()

add_custom_command(TARGET dingosdk_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dingosdk_runtime>/scripts/Custom/FirstPersonNative/presets/bundled"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/Extension/Skater/FirstPerson/Upstream/recommended.json"
        "$<TARGET_FILE_DIR:dingosdk_runtime>/scripts/Custom/FirstPersonNative/presets/bundled/recommended.json")
