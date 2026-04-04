# RetroDepth

Play retro arcade and console games in VR as a layered 3D diorama — each video layer (background, sprites, HUD) rendered at a different depth so the screen has real parallax.

RetroDepth runs alongside a patched build of MAME (**rdmame**). MAME handles emulation normally; rdmame exports per-layer bitmaps through Windows shared memory; RetroDepth reads them and renders them as quads in OpenXR.

---

## Requirements

- Windows 10/11 64-bit
- A VR headset supported by **SteamVR** (Quest via Air Link, Index, Vive, WMR, etc.)
- SteamVR installed and running before launching RetroDepth

---

## Quick start (pre-built release)

1. Download `retrodepth.zip` from the [Releases](../../releases) page and extract it anywhere.
2. Copy your ROMs into the `roms\` folder (and SNES BIOS into `bios\` if needed).
3. Launch **retrodepth.exe** — the game launcher opens.
4. Pick a game and click **Launch VR**.

The launcher starts rdmame automatically. Put on your headset and the game appears as a floating diorama.

---

## Supported systems

| System | Notes |
|--------|-------|
| **Neo Geo** | Full palette routing — assign sprite palettes to depth groups via the built-in editor |
| **CPS1** (Capcom) | scroll1–3, sprites, background layers |
| **CPS2** (Capcom) | scroll1–3, sprites, background layers |
| **TMNT** (Konami) | Multi-layer export |
| **The Simpsons** (Konami) | Multi-layer export |
| **SNES / Super Famicom** | BG1–4, sprites, colour math layers |
| **Genesis / Mega Drive** | Plane A/B, sprites, window |
| **Sega Master System** | Background, sprites |
| **Game Boy / Game Boy Color** | BG, sprites, window |

Game configs are included for ~40 Neo Geo and CPS titles. Other systems use an auto-generated default config that can be tuned in the editor.

---

## Building from source

### One-step build (recommended)

Requirements: [MSYS2](https://www.msys2.org) + MinGW-w64, [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) (Desktop C++ workload), and a clone of the MAME source.

```bat
REM 1. Get MAME source at the tested commit
git clone https://github.com/mamedev/mame.git C:\mame-src
cd C:\mame-src
git checkout 3bca6291cc76b2b1ebfe7c50f225eb6ad44c9847

REM 2. Run the build script from this repo
build.bat C:\mame-src
```

`build.bat` will:
- Auto-install vcpkg (clones + bootstraps into `C:\vcpkg` if not found)
- Overlay the MAME patches onto your MAME source
- Build **rdmame.exe** (~30–60 min first time)
- Build **retrodepth.exe**
- Package everything into `output\`

The MSYS2 MinGW64 toolchain must be installed first:
```bash
pacman -S mingw-w64-x86_64-gcc make
```

### Manual build

#### rdmame (patched MAME)

```bash
# Apply patches (from repo root)
cp -r src/   C:/mame-src/src/
cp -r scripts/ C:/mame-src/scripts/

# Build (in MSYS2 MinGW64 shell)
cd C:/mame-src
make -j8 IGNORE_GIT=1 REGENIE=1 SUBTARGET=rdmame \
  SOURCES=src/mame/neogeo/neogeo.cpp,src/mame/capcom/cps1.cpp,src/mame/capcom/cps2.cpp,\
src/mame/konami/tmnt.cpp,src/mame/konami/simpsons.cpp,\
src/mame/nintendo/snes.cpp,src/mame/nintendo/snes_m.cpp,\
src/mame/sega/megadriv.cpp,src/mame/sega/mdconsole.cpp,src/mame/sega/megacd.cpp,\
src/mame/shared/mega32x.cpp,src/mame/sega/sms.cpp,src/mame/sega/sms_m.cpp,\
src/mame/nintendo/gb.cpp
```

Output: `C:\mame-src\rdmame.exe`

#### retrodepth.exe

Requires vcpkg at `C:\vcpkg` and VS 2022 Build Tools.

```bat
cmake -S retrodepth -B retrodepth\out\build -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
ninja -C retrodepth\out\build retrodepth
```

---

## Repository layout

```
build.bat                    one-step build script
retrodepth/                  RetroDepth VR app source (C++17, CMake, vcpkg)
  src/                       application source files
  CMakeLists.txt
  vcpkg.json                 dependencies: openxr-loader, nlohmann-json
src/                         MAME patch files (overlay onto MAME source tree)
  emu/retrodepth.h           shared-memory structs and API
  emu/retrodepth.cpp         API implementation
  emu/machine.cpp            MAME machine loop integration
  mame/neogeo/               Neo Geo driver patches
  mame/capcom/               CPS1 / CPS2 driver patches
  mame/konami/               TMNT / Simpsons driver patches
  mame/nintendo/             SNES / GB driver patches
  mame/sega/                 Genesis / SMS driver patches
  devices/video/             shared video device patches (PPU, VDP, LCD)
scripts/src/emu.lua          MAME build system: registers retrodepth module
```

---

## Shared memory protocol (for developers)

rdmame exposes two named shared-memory blocks each frame.

### `Local\RetroDepthFrameBuffer` — frame data

```
RDHeader (1.125 MB reserved)
  magic           0x52445650 ('RDVP') — valid when set
  version         5
  frame_id        increments each frame — poll to detect new frames
  layer_count     number of active layers (up to 8)
  layers[]        RDLayerDesc per layer (name, width, height, z_order, data_offset, owner_offset)
  palette_argb    [256][16] ARGB8888 — full Neo Geo palette RAM, gamma-corrected
  pal_thumb       [256][32×32] ARGB8888 — per-palette thumbnails (on request)
pixel + owner data
  per layer: 512×256×4 bytes BGRA + 512×256×2 bytes uint16 palette owner IDs
```

Total size: ~10 MB.

**Layer names by system:**

| System | Layers |
|--------|--------|
| Neo Geo | `background`, `grp0`–`grp3` (sprites by depth group), `fix` (HUD) |
| CPS1/CPS2 | `background`, `scroll1`–`scroll3`, `sprites` |
| SNES | `bg1`–`bg4`, `sprites` |
| Genesis | `plane_a`, `plane_b`, `sprites`, `window` |
| SMS | `background`, `sprites` |
| GB/GBC | `bg`, `sprites`, `window` |

### `Local\RetroDepthControl` — depth routing (written by consumer)

```c
struct RDPaletteRoute {
    uint32_t magic;        // 0x52445052 ('RDPR') when valid
    uint8_t  route[256];   // palette index → depth group (0–3); 0xFF = group 0
    uint8_t  thumb_requested; // 1 = render per-palette thumbnails
};
```

### Minimal consumer example

```cpp
HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\RetroDepthFB4");
void*  base = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
RDHeader* hdr = (RDHeader*)base;

uint32_t last_id = hdr->frame_id;
while (true) {
    while (hdr->frame_id == last_id) Sleep(1);  // wait for new frame
    last_id = hdr->frame_id;

    for (uint32_t i = 0; i < hdr->layer_count; i++) {
        const RDLayerDesc& l = hdr->layers[i];
        const uint32_t* pixels = (uint32_t*)((uint8_t*)base + l.data_offset);
        // l.name, l.width, l.height, l.z_order
    }
}
```

---

## License

The MAME patches (`src/` and `scripts/`) modify MAME source code and are distributed under the same terms as MAME:

- [MAME license (COPYING)](https://github.com/mamedev/mame/blob/master/COPYING)
- [GPL-2.0 full text](https://github.com/mamedev/mame/blob/master/docs/legal/GPL-2.0)

If you redistribute binaries built from these patches you must also make the corresponding modified source available.

The RetroDepth application source (`retrodepth/`) is released under the MIT License.
