# OpenXR VR for first person (Extension/VR). openxr_loader.dll is loaded at run
# time from beside ReSkate.dll, so the build needs only the vendored headers.
add_library(dingosdk_vr STATIC
    Extension/VR/vr_camera.cpp
    Extension/VR/vr_blit.cpp
    Extension/VR/vr_xr.cpp
    Extension/VR/vr_depth.cpp
    Extension/VR/vr_stereo.cpp
    Extension/VR/vr_input.cpp
    Extension/VR/vr_gamepad.cpp)
target_include_directories(dingosdk_vr SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/External/openxr/include")
target_link_libraries(dingosdk_vr PUBLIC dingosdk_logging dingosdk_hooks)

option(DINGOSDK_BUILD_VR_TESTS "Build VR pose and projection math tests" OFF)
if(DINGOSDK_BUILD_VR_TESTS)
    enable_testing()
    add_executable(dingosdk_vr_math_tests Extension/VR/Test/vr_math_tests.cpp)
    target_include_directories(dingosdk_vr_math_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    add_test(NAME vr_math COMMAND dingosdk_vr_math_tests)
endif()
