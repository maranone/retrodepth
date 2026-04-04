// retrodepth.h — Shared memory layer export for RetroDepth VR
// MAME writes per-layer bitmaps here; retrodepth.exe reads them.
//
// Shared memory name:  "Local\RetroDepthFrameBuffer"
// Total size:          RD_SHMEM_SIZE bytes
//
// Layout:
//   [RDHeader]   — magic, version, frame_id, layer descs, palette data
//   [pixel data] — BGRA 4 bytes/pixel, each layer at desc.data_offset

#pragma once

#include <cstdint>

// Maximum number of layers per frame: background + grp0-grp3 + fix + spare
static constexpr uint32_t RD_MAX_LAYERS  = 8;
// Maximum frame dimensions — sized for NeoGeo (320x224) with margin.
static constexpr uint32_t RD_MAX_WIDTH   = 512;
static constexpr uint32_t RD_MAX_HEIGHT  = 256;
// Per-palette thumbnail size (pixels per side). 32×32 shows a downsampled
// view of all on-screen content using that palette.
static constexpr uint32_t RD_THUMB_DIM   = 32;
// Header area: palette_argb(16KB) + pal_thumb(256×32×32×4=1MB) + descs + misc
// Must be >= sizeof(RDHeader).  Rounded up to a comfortable margin.
static constexpr uint32_t RD_DATA_OFFSET = 1179648u; // 1.125 MB
static constexpr uint32_t RD_PIXEL_BYTES = RD_MAX_WIDTH * RD_MAX_HEIGHT * 4;
static constexpr uint32_t RD_OWNER_BYTES = RD_MAX_WIDTH * RD_MAX_HEIGHT * 2;
static constexpr uint32_t RD_SHMEM_SIZE  = RD_DATA_OFFSET + (RD_PIXEL_BYTES + RD_OWNER_BYTES) * RD_MAX_LAYERS;

static constexpr uint32_t RD_MAGIC   = 0x52445650; // 'RDVP'
static constexpr uint32_t RD_VERSION = 5;           // v5: per-layer owner-id buffers
static constexpr uint32_t RD_LAYER_FLAG_HAS_OWNER = 0x1u;
static constexpr uint16_t RD_OWNER_NONE = 0xFFFFu;

#pragma pack(push, 1)

struct RDLayerDesc {
	char     name[32];       // e.g. "background", "grp0", "fix"
	uint32_t z_order;        // draw order: 0 = furthest back
	uint32_t width;
	uint32_t height;
	uint32_t data_offset;    // byte offset from start of shared memory to BGRA pixels
	uint32_t owner_offset;   // byte offset from start of shared memory to uint16 owner ids
	uint32_t flags;          // RD_LAYER_FLAG_*
};

struct RDHeader {
	uint32_t   magic;        // RD_MAGIC when valid
	uint32_t   version;      // RD_VERSION
	uint32_t   frame_id;     // increments every frame; retrodepth polls this
	uint32_t   layer_count;  // number of valid RDLayerDesc entries
	RDLayerDesc layers[RD_MAX_LAYERS];
	// NeoGeo palette RAM: 256 palettes × 16 colors, ARGB8888 (gamma-corrected)
	uint32_t   palette_argb[256][16];
	// Per-palette thumbnails: 32×32 downsampled view of on-screen content
	// filtered to each palette.  Written incrementally (a few per frame).
	// Only valid when retrodepth requests them (thumb_requested in ctrl shmem).
	uint32_t   pal_thumb[256][RD_THUMB_DIM * RD_THUMB_DIM];
};

#pragma pack(pop)

static_assert(sizeof(RDHeader) <= RD_DATA_OFFSET, "RDHeader exceeds reserved space");

// ---------------------------------------------------------------------------
// Palette route control — written by retrodepth, read by MAME each frame.
// Separate shmem: "Local\\RetroDepthControl"
// route[i] = group for palette i:
//   0 = grp0 (far), 1 = grp1, 2 = grp2, 3 = grp3 (near), 0xFF = default (grp0)
// ---------------------------------------------------------------------------
static constexpr uint32_t RD_CTRL_MAGIC = 0x52445052u; // 'RDPR'

#pragma pack(push, 1)
struct RDPaletteRoute {
    uint32_t magic;           // RD_CTRL_MAGIC when valid
    uint8_t  route[256];      // palette_index → group (0-3); 0xFF = grp0
    uint8_t  thumb_requested; // 1 = editor open, render per-palette thumbnails
    uint8_t  _pad[3];
};
#pragma pack(pop)

// Read palette route written by retrodepth. Returns all-zero (grp0) if not connected.
void retrodepth_read_palette_route(RDPaletteRoute* out);

// Write the NeoGeo palette into the shmem header so retrodepth can display swatches.
void retrodepth_write_palette_data(const uint32_t* argb_data, uint32_t count);

// Write a 32×32 thumbnail for one palette index (called when thumb_requested).
// pixels: RD_THUMB_DIM*RD_THUMB_DIM ARGB8888 values, top-down row-major.
void retrodepth_write_palette_thumbs(const uint32_t* pixels, uint32_t pal_idx);

// ---------------------------------------------------------------------------
// MAME-side API  (implementation in retrodepth.cpp)
// ---------------------------------------------------------------------------

// Call once at startup. No-op if already open.
void retrodepth_init();

// Returns true if retrodepth.exe is connected (shared memory mapped).
bool retrodepth_active();

// Write one layer's BGRA bitmap into the shared memory slot.
void retrodepth_write_layer(uint32_t z_order, const char* name,
							const uint32_t* bgra_pixels,
							const uint16_t* owner_ids,
							uint32_t width, uint32_t height);

// Flip the frame_id so retrodepth knows a new frame is ready.
void retrodepth_commit();
