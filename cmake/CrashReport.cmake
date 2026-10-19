# The sentry_key is the project's public DSN key: it grants report submission only. Never use an auth token.
set(crash_report_default_url "https://sentry.reskate.dev/api/2/minidump/?sentry_key=8efcb389bdc033445093a7af0c66f9be")
if(NOT "$ENV{RESKATE_CRASH_REPORT_URL}" STREQUAL "")
    set(crash_report_default_url "$ENV{RESKATE_CRASH_REPORT_URL}")
endif()
set(DINGOSDK_CRASH_REPORT_URL "${crash_report_default_url}" CACHE STRING "Sentry HTTPS minidump endpoint URL (empty disables uploads)")
if(DINGOSDK_CRASH_REPORT_URL AND NOT DINGOSDK_CRASH_REPORT_URL MATCHES "^https://[^ \r\n\t]+$")
    message(FATAL_ERROR "DINGOSDK_CRASH_REPORT_URL must be an HTTPS minidump endpoint URL")
endif()
set(crash_report_url_escaped "${DINGOSDK_CRASH_REPORT_URL}")
string(REPLACE "\\" "\\\\" crash_report_url_escaped "${crash_report_url_escaped}")
string(REPLACE "\"" "\\\"" crash_report_url_escaped "${crash_report_url_escaped}")
find_package(Git QUIET)
set(crash_report_version "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=12
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE crash_report_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(crash_report_revision)
        set(crash_report_version "${crash_report_revision}")
    endif()
endif()
configure_file(cmake/templates/crash_report_config.h.in generated/crash_report_config.h @ONLY)

add_library(dingosdk_crash_report_client STATIC Engine/Core/Debug/client.cpp Engine/Core/Debug/trace.cpp)
target_link_libraries(dingosdk_crash_report_client PRIVATE dingosdk_json winhttp bcrypt)
add_library(dingosdk_crash_report_upload STATIC Engine/Core/Debug/upload.cpp
    Engine/Core/Debug/native_dump.cpp Engine/Core/Debug/report_envelope.cpp Engine/Core/Debug/report_details.cpp)
target_link_libraries(dingosdk_crash_report_upload PRIVATE dingosdk_json winhttp bcrypt)
target_sources(dingosdk_launcher PRIVATE Launcher/crash_reporter.cpp)
target_link_libraries(dingosdk_launcher PRIVATE dingosdk_crash_report_upload dingosdk_json dbghelp)
target_link_libraries(dingosdk_logging PRIVATE dingosdk_crash_report_client)
# The launcher hosts crash capture for both executables; no third binary is shipped.
add_dependencies(dingosdk_runtime dingosdk_launcher)

# Release dumps need matching symbols; keep optimization and omit incremental linking.
# PDBALTPATH embeds only the PDB's file name, not the build machine's folder.
foreach(target dingosdk_runtime dingosdk_launcher dingosdk_server)
    target_compile_options(${target} PRIVATE /Zi)
    target_link_options(${target} PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF
        "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${target}>")
endforeach()
get_property(crash_report_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS crash_report_targets)
    get_target_property(kind ${target} TYPE)
    if(kind STREQUAL "STATIC_LIBRARY")
        target_compile_options(${target} PRIVATE /Zi)
    endif()
endforeach()

option(DINGOSDK_BUILD_CRASH_REPORT_TESTS "Build crash capture and upload regression tests" OFF)
if(DINGOSDK_BUILD_CRASH_REPORT_TESTS)
    enable_testing()
    add_executable(dingosdk_crash_report_tests Engine/Core/Debug/Test/crash_report_tests.cpp)
    target_link_libraries(dingosdk_crash_report_tests PRIVATE dingosdk_logging dingosdk_crash_report_upload dingosdk_json dbghelp)
    add_dependencies(dingosdk_crash_report_tests dingosdk_launcher)
    add_test(NAME crash_report COMMAND dingosdk_crash_report_tests "${CMAKE_CURRENT_BINARY_DIR}/crash-report-fixtures")
    set_tests_properties(crash_report PROPERTIES TIMEOUT 90)
endif()
