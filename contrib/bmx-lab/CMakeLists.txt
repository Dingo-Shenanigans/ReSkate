cmake_minimum_required(VERSION 3.24)
project(ReSkateBmxLab LANGUAGES CXX)

# This portable development harness intentionally does not configure the native runtime.
# The production Windows DLL still requires the MSVC toolchain in the root project.
get_filename_component(RESKATE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include(CTest)
include("${RESKATE_SOURCE_ROOT}/cmake/BmxTests.cmake")

function(bmx_lab_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${target} PRIVATE "${RESKATE_SOURCE_ROOT}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(MINGW)
            # Keep the test app runnable outside the compiler's PATH.
            target_link_options(${target} PRIVATE -static)
        endif()
    endif()
endfunction()

dingosdk_add_bmx_tests("${RESKATE_SOURCE_ROOT}")
foreach(target dingosdk_bmx_input_tests dingosdk_bmx_model_tests dingosdk_bmx_controls_model_tests)
    bmx_lab_target(${target})
endforeach()

if(WIN32)
    add_executable(dingosdk_bmx_lab WIN32 bmx_lab.cpp)
    bmx_lab_target(dingosdk_bmx_lab)
    target_compile_definitions(dingosdk_bmx_lab PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(dingosdk_bmx_lab PRIVATE user32 gdi32)
endif()
