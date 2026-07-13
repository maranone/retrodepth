Retro Depth PC (Setup Notes) By Legaiaflame

Special Thanks to Gunstar Gamer

Setup for ROMS (SNES):

You will need a Bios file called (s_smp.zip) which contains a file called spc700.rom, which Retro Depth needs to run SNES roms. You will have to find this yourself, sorry.

Put (s_smp.zip) in the roms folder where you have all of your games. Don't unzip it, just put it in there as is. If this zip file is not in the folder you won't be able to play SNES games.

If for some reason SNES games don't show up in the rom preview list, go to settings and un-check Hide Incorrect Roms.

Setup for ROMS (Genesis):

Genesis games load as is, no zip file/Bios needed.

How to Play Arcade Games:

Launch retrodepth.exe
In Settings Uncheck Hide MAME Window
Launch into any game and when the game is loaded up close the mini MAME window that the game is in.
Launch rdmame.exe and start any game you want.
Start up (Important!)

Start rdmame.exe first, then tab/Window out but don't close the window. Now open the main retrodepth.exe emulator. Go to Settings and un-check Hide MAME Window and Hide Incorrect Roms. Hit Back and start your Game.

Left Side Menu in VR View:

There is also a Menu to the far left of your headset view where you can change contrast, saturation, screen tilt, curve as well as turning on and off shadows and 3D upscale. There are also colored squares to the far left. If you point and click on them you can change the background window to black or whatever color you want.

3D Settings: (Need VR Controller)

Move the right analog up and down to zoom in or out. Pressing in left analog multiple times, gives different 3D modes. And pressing slightly left or right on the right analog changes how far the 3D pops out. Just press it a hair, very lightly. Pressing in right analog while pointing at a direction will move the game screen left or right. So you can position it where you want.

On the left VR controller press the left trigger button to make the screen bigger. Press the left toggle button to make the screen smaller. Making the screen smaller will add duplicate layers. You can use the editor to manually select a layer and un duplicate layers. (edited)Saturday, June 13, 2026 8:01 PM

How to Manually Select Layers:

Before you run a game, open up rdmame.exe, then tab out but don't close the window. Now open the main retrodepth.exe emulator. Now, you will be able to see the following settings take effect in the preview window. The commands to manually select layers are as follows:

Press R to Turn on & off layer selection mode
Press E to Select layer (will flash yellow)
Press W to Move layer backwards
Press S to Move layer forward
Press A to Make layer smaller
Press D to Make layer bigger
Press Z to Duplicate layer
Press X to Un Duplicate layer

How to Bring Back Missing Text Boxes or Background Layers if they are not Visible:

If you can't see a text box or certain background layers are not showing, you will have to try to find it with the Manual Layer Selector as mentioned above. Press R to turn on the selector then press E to start selecting layers (will flash Yellow). Usually the missing text layer is somewhere hidden in the background. So while pressing E and cycling through the available layers, try to get a background layer that is furthest off in the background (Even if the background layer is not blinking yellow maybe it will come forward). Next keep pressing S to bring it forward from the background and into view.

Also, and easier way to find these hidden layers is to use the menu on the left side to use The Screen Tilt R/L feature. Then, you can easily see all of the visible layers hidden behind one another.

How to Save:

You can save with specific F keys:

Press F6 the game will pause. now save on number keys 1-9
Press F7 the game will pause. now load your save state from number keys 1-9

Multiple savestates do work for multiple games. Lets say I make a savestate for Sonic 3 on 1 then make a save state for Donkey Kong on 2, they will both load as long as you don't save over them. In-game saves seem to be working but I recommend to make multiple save states just to be safe and play one game at a time if you can. Just make sure you always start the rdmame.exe first before starting retrodepth.exe.

Controller Issues/Loss of Input: (Solution)

If you click out of the Retro Depth emulator window you will completely lose controller input. To fix this, before you run a game, open up rdmame.exe, then tab/Window out but don't close the window. Now open the main retrodepth.exe emulator. Next, go to settings and uncheck Hide MAME window. Now run a game. If you click outside of the window you will lose input again, but since the MAME preview window is open we can just click on that to restore controller input.

How to Setup SNES/Genesis Input for PC Controller:

Before you run a game, open up rdmame.exe, then tab out but don't close the window. Now open the main retrodepth.exe emulator. Next, go to settings and uncheck Hide MAME window and un-check Hide Incorrect Roms. Now run an SNES game.

Start a SNES/Genesis game

Press Tab to bring up the rdmame.exe settings window

Double-click "Input Settings" and then "Input Assignments (This System)

Double click each input to Setup your PC controller

Scroll all the way down to "Return to Previous Menu" and double-click to return

Double-click "Return to Previous Menu" and double-click "Close Menu" to return to game.

I lost my input settings a couple times, so you might have to reset controls each time you restart a game if you don't load from a savestate.

How to Add 6 Button Controls for Genesis Games:

Before you run a game, open up rdmame.exe, then tab out but don't close the window. Now open the main retrodepth.exe emulator. Next, go to settings and uncheck Hide MAME window. Now run a game.

Start a Genesis game

Press Tab to bring up the rdmame.exe settings window

Choose "Slot Devices",

Change ctrl1 (or ctrl2) from "mdpad" to "md6button" (just press left once.),

Select "Reset System." to enable the 6 button controls


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
