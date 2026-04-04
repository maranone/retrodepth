cmake_minimum_required(VERSION 3.20)
project(retrodepth CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenXR CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(retrodepth
    src/app.rc
    src/main.cpp
    src/launcher_window.cpp
    src/settings_io.cpp
    src/xr_app.cpp
    src/preview_window.cpp
    src/spectator_window.cpp
    src/diagnostics_recorder.cpp
    src/dynamic_router.cpp
    src/d3d11_renderer.cpp
    src/shmem_reader.cpp
    src/game_config.cpp
    src/layer_editor.cpp
    src/palette_control.cpp
    src/editor_info_window.cpp
)

target_link_libraries(retrodepth PRIVATE
    OpenXR::openxr_loader
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
    dxguid
    gdiplus
    shell32
    ole32
)

target_include_directories(retrodepth PRIVATE src)

# Copy configs next to the executable so retrodepth.exe finds them at runtime.
# settings.json is only copied if it doesn't already exist in the output dir,
# so user edits (mame_exe path, roms_path, etc.) survive rebuilds.
add_custom_command(TARGET retrodepth POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR="${CMAKE_SOURCE_DIR}/configs"
        -DDST_DIR="$<TARGET_FILE_DIR:retrodepth>/configs"
        -P "${CMAKE_SOURCE_DIR}/cmake/sync_configs.cmake"
)
