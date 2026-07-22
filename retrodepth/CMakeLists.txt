cmake_minimum_required(VERSION 3.20)
project(retrodepth C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenXR CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---- snes9x 1.63 (libretro static lib) ------------------------------------

set(SNES9X_DIR "${CMAKE_SOURCE_DIR}/third_party/snes9x")
set(SNES9X_LIBRETRO_COMMON_DIR "${SNES9X_DIR}/libretro/libretro-common")

set(SNES9X_SOURCES
    ${SNES9X_DIR}/filter/snes_ntsc.c
    ${SNES9X_DIR}/apu/apu.cpp
    ${SNES9X_DIR}/apu/bapu/dsp/sdsp.cpp
    ${SNES9X_DIR}/apu/bapu/smp/smp.cpp
    ${SNES9X_DIR}/apu/bapu/smp/smp_state.cpp
    ${SNES9X_DIR}/bsx.cpp
    ${SNES9X_DIR}/c4.cpp
    ${SNES9X_DIR}/c4emu.cpp
    ${SNES9X_DIR}/cheats.cpp
    ${SNES9X_DIR}/cheats2.cpp
    ${SNES9X_DIR}/clip.cpp
    ${SNES9X_DIR}/conffile.cpp
    ${SNES9X_DIR}/controls.cpp
    ${SNES9X_DIR}/cpu.cpp
    ${SNES9X_DIR}/cpuexec.cpp
    ${SNES9X_DIR}/cpuops.cpp
    ${SNES9X_DIR}/crosshairs.cpp
    ${SNES9X_DIR}/dma.cpp
    ${SNES9X_DIR}/dsp.cpp
    ${SNES9X_DIR}/dsp1.cpp
    ${SNES9X_DIR}/dsp2.cpp
    ${SNES9X_DIR}/dsp3.cpp
    ${SNES9X_DIR}/dsp4.cpp
    ${SNES9X_DIR}/fxinst.cpp
    ${SNES9X_DIR}/fxemu.cpp
    ${SNES9X_DIR}/gfx.cpp
    ${SNES9X_DIR}/globals.cpp
    ${SNES9X_DIR}/memmap.cpp
    ${SNES9X_DIR}/movie.cpp
    ${SNES9X_DIR}/msu1.cpp
    ${SNES9X_DIR}/obc1.cpp
    ${SNES9X_DIR}/ppu.cpp
    ${SNES9X_DIR}/sa1.cpp
    ${SNES9X_DIR}/sa1cpu.cpp
    ${SNES9X_DIR}/screenshot.cpp
    ${SNES9X_DIR}/sdd1.cpp
    ${SNES9X_DIR}/sdd1emu.cpp
    ${SNES9X_DIR}/seta.cpp
    ${SNES9X_DIR}/seta010.cpp
    ${SNES9X_DIR}/seta011.cpp
    ${SNES9X_DIR}/seta018.cpp
    ${SNES9X_DIR}/sha256.cpp
    ${SNES9X_DIR}/snapshot.cpp
    ${SNES9X_DIR}/snes9x.cpp
    ${SNES9X_DIR}/spc7110.cpp
    ${SNES9X_DIR}/srtc.cpp
    ${SNES9X_DIR}/stream.cpp
    ${SNES9X_DIR}/tile.cpp
    ${SNES9X_DIR}/tileimpl-h2x1.cpp
    ${SNES9X_DIR}/tileimpl-n1x1.cpp
    ${SNES9X_DIR}/tileimpl-n2x1.cpp
    ${SNES9X_DIR}/bml.cpp
    ${SNES9X_DIR}/fscompat.cpp
    ${SNES9X_DIR}/libretro/libretro.cpp
    # Layer capture / accessor files compiled into the lib so they can access snes9x internals
    ${CMAKE_SOURCE_DIR}/src/snes9x_layer_capture.cpp
    ${CMAKE_SOURCE_DIR}/src/snes9x_zbuffer_accessor.cpp
    ${CMAKE_SOURCE_DIR}/src/snes9x_visible_source_accessor.cpp
)

add_library(snes9x_libretro STATIC ${SNES9X_SOURCES})

set_target_properties(snes9x_libretro PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    C_STANDARD 11
)

target_include_directories(snes9x_libretro PRIVATE
    ${SNES9X_DIR}
    ${SNES9X_DIR}/apu
    ${SNES9X_DIR}/apu/bapu
    ${SNES9X_DIR}/libretro
    ${SNES9X_LIBRETRO_COMMON_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(snes9x_libretro PRIVATE
    RIGHTSHIFT_IS_SAR
    __LIBRETRO__
    ALLOW_CPU_OVERCLOCK
    HAVE_STDINT_H
    # HAVE_STRINGS_H omitted — strings.h is POSIX and absent from MSVC
    __WIN32__   # activates port.h's strcasecmp→stricmp mapping and windows.h include
)

target_compile_options(snes9x_libretro PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

# ---- retrodepth executable -------------------------------------------------

add_executable(retrodepth
    src/app.rc
    src/main.cpp
    src/launcher_window.cpp
    src/settings_io.cpp
    src/xr_app.cpp
    src/preview_window.cpp
    src/spectator_window.cpp
    src/stereo_display_window.cpp
    src/diagnostics_recorder.cpp
    src/dynamic_router.cpp
    src/d3d11_renderer.cpp
    src/shmem_reader.cpp
    src/game_config.cpp
    src/layer_editor.cpp
    src/palette_control.cpp
    src/editor_info_window.cpp
    src/snes9x_source.cpp
)

target_link_libraries(retrodepth PRIVATE
    OpenXR::openxr_loader
    nlohmann_json::nlohmann_json
    snes9x_libretro
    d3d11
    dxgi
    d3dcompiler
    dxguid
    gdiplus
    shell32
    ole32
    xinput
    xaudio2
)

target_include_directories(retrodepth PRIVATE
    src
    ${SNES9X_DIR}/libretro
)

# Copy configs next to the executable so retrodepth.exe finds them at runtime.
# settings.json is only copied if it doesn't already exist in the output dir,
# so user edits (mame_exe path, roms_path, etc.) survive rebuilds.
add_custom_command(TARGET retrodepth POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR="${CMAKE_SOURCE_DIR}/configs"
        -DDST_DIR="$<TARGET_FILE_DIR:retrodepth>/configs"
        -P "${CMAKE_SOURCE_DIR}/cmake/sync_configs.cmake"
)
