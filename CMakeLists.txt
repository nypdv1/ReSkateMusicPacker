cmake_minimum_required(VERSION 3.20)
project(ReSkateMusicPacker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Include directories
include_directories(
    src
    External
    External/imgui
    External/lz4
    External/miniz
    External/zstd/lib
)

# External dependencies (without /WX)
# 1. lz4
add_library(lz4 STATIC External/lz4/lz4.c)
target_include_directories(lz4 PUBLIC External/lz4)

# 2. miniz
add_library(miniz STATIC External/miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC External/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_ZLIB_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)

# 3. zstd
file(GLOB zstd_sources CONFIGURE_DEPENDS
    External/zstd/lib/common/*.c
    External/zstd/lib/compress/*.c
    External/zstd/lib/decompress/*.c
)
add_library(zstd STATIC ${zstd_sources})
target_include_directories(zstd SYSTEM PUBLIC External/zstd/lib)
target_compile_definitions(zstd PRIVATE ZSTD_LEGACY_SUPPORT=0 ZSTD_DISABLE_ASM)

# 4. imgui
add_library(imgui STATIC
    External/imgui/imgui.cpp
    External/imgui/imgui_draw.cpp
    External/imgui/imgui_tables.cpp
    External/imgui/imgui_widgets.cpp
    External/imgui/backends/imgui_impl_dx12.cpp
    External/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui SYSTEM PUBLIC External/imgui)
target_compile_definitions(imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)

# Target configuration helper for our code (enforces strict /W4 /WX)
function(configure_strict_target target)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive- /EHsc /Zc:preprocessor)
    endif()
endfunction()

# 5. Engine Core / Frostbite / VFS Library
set(ENGINE_SOURCES
    src/Engine/Core/Json/json.cpp
    src/Engine/Resource/binary_bundle.cpp
    src/Engine/Resource/binary_io.cpp
    src/Engine/Resource/bundle_ref_table.cpp
    src/Engine/Resource/cas_codec.cpp
    src/Engine/Resource/ebx_document.cpp
    src/Engine/Resource/ebx_merge.cpp
    src/Engine/Resource/ebx_writer.cpp
    src/Engine/Resource/material_grid.cpp
    src/Engine/Resource/shader_lookup.cpp
    src/Engine/Resource/toc.cpp
    src/Engine/Vfs/native_db.cpp
    src/Engine/Vfs/game_archives.cpp
    src/Engine/Vfs/game_bundles.cpp
)
add_library(engine STATIC ${ENGINE_SOURCES})
configure_strict_target(engine)
target_link_libraries(engine PUBLIC lz4 miniz zstd)

# 6. Packer Core Library
add_library(music_packer_lib STATIC src/packer.cpp src/artwork.cpp src/ffmpeg_fetch.cpp)
configure_strict_target(music_packer_lib)
target_link_libraries(music_packer_lib PUBLIC engine bcrypt ole32 gdi32 windowscodecs winhttp shell32)

# 7. Main Application (unified CLI + GUI, WIN32 subsystem)
add_executable(ReSkateMusicPacker WIN32
    src/main.cpp
    src/gui_renderer.cpp
)
configure_strict_target(ReSkateMusicPacker)
target_link_libraries(ReSkateMusicPacker PRIVATE
    music_packer_lib
    imgui
    windowscodecs
    winmm
    shell32
    ole32
    dwmapi
    dxgi
    d3d12
)

# 8. Regression Tests
add_executable(ReSkateMusicPackerTests test/packer_tests.cpp)
configure_strict_target(ReSkateMusicPackerTests)
target_link_libraries(ReSkateMusicPackerTests PRIVATE music_packer_lib)
enable_testing()
add_test(NAME music_packer_tests COMMAND ReSkateMusicPackerTests)
add_executable(ReSkateArtworkTests test/artwork_tests.cpp)
configure_strict_target(ReSkateArtworkTests)
target_link_libraries(ReSkateArtworkTests PRIVATE music_packer_lib)
add_test(NAME artwork_tests COMMAND ReSkateArtworkTests "${CMAKE_BINARY_DIR}/artwork-preview.png")

add_executable(ReSkateFfmpegTests test/ffmpeg_fetch_tests.cpp)
configure_strict_target(ReSkateFfmpegTests)
target_link_libraries(ReSkateFfmpegTests PRIVATE music_packer_lib)
add_test(NAME ffmpeg_fetch_tests COMMAND ReSkateFfmpegTests)
