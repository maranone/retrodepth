# WSH — Windows Shared-memory Header for MAME layer export

A set of patches for MAME that exposes each video layer as a separate BGRA bitmap
through Windows shared memory, so an external application can consume them in real time.

---

## What it does

Each video frame MAME writes two shared-memory blocks:

### `Local\RetroDepthFrameBuffer` — frame data (read by the consumer)

| Field | Description |
|---|---|
| `frame_id` | Increments every frame. Poll this to detect new frames. |
| `layer_count` | Number of active layers this frame (up to 8). |
| `layers[]` | Array of `RDLayerDesc` — name, size, draw order, pixel offset. |
| `palette_argb[256][16]` | Full Neo Geo palette RAM, gamma-corrected ARGB8888. |
| `pal_thumb[256][32×32]` | Per-palette 32×32 thumbnail (written on request). |
| pixel data | BGRA 4 bytes/pixel per layer, at `layer.data_offset`. |
| owner data | `uint16` palette index per pixel, at `layer.owner_offset`. |

**Layers written for Neo Geo:**

| Name | Contents |
|---|---|
| `background` | Background tilemap |
| `grp0` – `grp3` | Sprite palettes grouped by depth assignment |
| `fix` | Fix layer (HUD / text overlay) |

**Layers written for CPS1 and CPS2:**

| Name | Contents |
|---|---|
| `background` | Solid background fill |
| `scroll1` – `scroll3` | Background scroll planes |
| `sprites` | Sprite layer |

### `Local\RetroDepthControl` — depth routing (written by the consumer)

| Field | Description |
|---|---|
| `route[256]` | For each of the 256 Neo Geo palettes: which depth group (0–3) it belongs to. `0xFF` = default group 0. |
| `thumb_requested` | Set to `1` to ask MAME to render per-palette thumbnails. |

The consumer writes this block to tell MAME how to distribute palettes across the
`grp0`–`grp3` layers. MAME reads it once per frame.

---

## Shared memory layout

```
Local\RetroDepthFrameBuffer
├── RDHeader (1.125 MB reserved)
│   ├── magic, version, frame_id, layer_count
│   ├── RDLayerDesc[8]
│   ├── palette_argb[256][16]
│   └── pal_thumb[256][32×32]
└── pixel + owner data
    └── per layer: 512×256×4 bytes BGRA + 512×256×2 bytes owner ids
```

Total size: ~10 MB.

---

## Files

```
scripts/src/emu.lua            — build system: registers retrodepth module
src/emu/retrodepth.h           — shared-memory structs and API declarations
src/emu/retrodepth.cpp         — API implementation (init, write, commit)
src/emu/machine.cpp            — hooks retrodepth_init/commit into MAME machine loop
src/mame/neogeo/neogeo.cpp     — Neo Geo driver: palette group routing
src/mame/neogeo/neogeo_spr.cpp — sprite renderer: per-pixel palette owner tagging
src/mame/neogeo/neogeo_spr.h   — sprite structures
src/mame/neogeo/neogeo_v.cpp   — video: layer write calls
src/mame/mame.lst              — game list entry
src/mame/capcom/cps1.cpp       — CPS1 driver: layer routing
src/mame/capcom/cps1_v.cpp     — CPS1 video: layer write calls
src/mame/capcom/cps2.cpp       — CPS2 driver: layer write calls
```

---

## Building

### Prerequisites

- [MSYS2](https://www.msys2.org) with the MinGW64 toolchain:
  ```bash
  pacman -S mingw-w64-x86_64-toolchain make
  ```
- ~10 GB free disk space for the MAME source tree

### 1 — Clone upstream MAME at the tested base commit

```bash
git clone https://github.com/mamedev/mame.git mame-src
cd mame-src
git checkout 3bca6291cc76b2b1ebfe7c50f225eb6ad44c9847
```

### 2 — Overlay the patches

```bash
cp -r mame-patches/. mame-src/
```

### 3 — Build

```bash
cd mame-src
make SUBTARGET=neogeo SOURCES=src/mame/neogeo/neogeo.cpp -j8
```

Output: `mame-src/neogeo.exe`

---

# Consuming the WSH memory

// Open the frame buffer
HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\RetroDepthFrameBuffer");
void*  base = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
RDHeader* hdr = (RDHeader*)base;

// Wait for a new frame
uint32_t last_id = 0;
while (hdr->frame_id == last_id) Sleep(1);
last_id = hdr->frame_id;

// Read each layer
for (uint32_t i = 0; i < hdr->layer_count; i++) {
    RDLayerDesc* layer = &hdr->layers[i];
    uint32_t* pixels   = (uint32_t*)((uint8_t*)base + layer->data_offset);
    uint16_t* owners   = (uint16_t*)((uint8_t*)base + layer->owner_offset);
    // layer->name, layer->width, layer->height, layer->z_order
}

---

## Licensing

These patches modify MAME source code and are distributed under the same terms.

- MAME license: [upstream COPYING](https://github.com/mamedev/mame/blob/master/COPYING)
- Full GPL text: [upstream GPL-2.0](https://github.com/mamedev/mame/blob/master/docs/legal/GPL-2.0)

If you redistribute binaries built from these patches you must also make the
corresponding modified source available.
