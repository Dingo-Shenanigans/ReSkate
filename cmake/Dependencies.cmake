if(WIN32)
    add_library(dingosdk_imgui STATIC
        "${PROJECT_SOURCE_DIR}/External/imgui/imgui.cpp"
        "${PROJECT_SOURCE_DIR}/External/imgui/imgui_draw.cpp"
        "${PROJECT_SOURCE_DIR}/External/imgui/imgui_tables.cpp"
        "${PROJECT_SOURCE_DIR}/External/imgui/imgui_widgets.cpp"
        "${PROJECT_SOURCE_DIR}/External/imgui/backends/imgui_impl_dx12.cpp"
        "${PROJECT_SOURCE_DIR}/External/imgui/backends/imgui_impl_win32.cpp")
    target_include_directories(dingosdk_imgui PUBLIC "${PROJECT_SOURCE_DIR}/External/imgui")
    # The backend's own gamepad reads only XInput slot 0. The launcher feeds every pad
    # itself (Launcher/gamepad_input.cpp), and the in-game menu does not use ImGui's gamepad keys.
    target_compile_definitions(dingosdk_imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    target_link_libraries(dingosdk_imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)

    add_library(dingosdk_detours STATIC
        External/detours/src/detours.cpp
        External/detours/src/modules.cpp
        External/detours/src/disasm.cpp
        External/detours/src/image.cpp
        External/detours/src/creatwth.cpp
        External/detours/src/disolx86.cpp
        External/detours/src/disolx64.cpp
        External/detours/src/disolia64.cpp
        External/detours/src/disolarm.cpp
        External/detours/src/disolarm64.cpp)
    target_include_directories(dingosdk_detours SYSTEM PUBLIC External/detours/src)
    target_compile_definitions(dingosdk_detours PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(dingosdk_imgui dingosdk_detours PROPERTIES FOLDER "Dependencies")
endif()

enable_language(C)
add_library(dingosdk_miniz STATIC External/miniz/miniz.c)
target_include_directories(dingosdk_miniz SYSTEM PUBLIC External/miniz)
target_compile_definitions(dingosdk_miniz PUBLIC MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_ZLIB_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
set_target_properties(dingosdk_miniz PROPERTIES FOLDER "Dependencies")
add_library(dingosdk_lz4 STATIC External/lz4/lz4.c)
target_include_directories(dingosdk_lz4 SYSTEM PUBLIC External/lz4)
set_target_properties(dingosdk_lz4 PROPERTIES FOLDER "Dependencies")
file(GLOB zstd_sources CONFIGURE_DEPENDS External/zstd/lib/common/*.c
    External/zstd/lib/compress/*.c External/zstd/lib/decompress/*.c)
add_library(dingosdk_zstd STATIC ${zstd_sources})
target_include_directories(dingosdk_zstd SYSTEM PUBLIC External/zstd/lib)
target_compile_definitions(dingosdk_zstd PRIVATE ZSTD_LEGACY_SUPPORT=0 ZSTD_DISABLE_ASM)
set_target_properties(dingosdk_zstd PROPERTIES FOLDER "Dependencies")
add_library(dingosdk_sqlite STATIC External/sqlite/sqlite3.c)
target_include_directories(dingosdk_sqlite SYSTEM PUBLIC External/sqlite)
target_compile_definitions(dingosdk_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)
set_target_properties(dingosdk_sqlite PROPERTIES FOLDER "Dependencies")
