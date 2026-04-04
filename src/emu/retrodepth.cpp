// retrodepth.cpp — Shared memory layer export for RetroDepth VR

#include "retrodepth.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
// Stub for non-Windows builds — retrodepth is Windows-only
void retrodepth_init() {}
bool retrodepth_active() { return false; }
void retrodepth_write_layer(uint32_t, const char*, const uint32_t*, const uint16_t*, uint32_t, uint32_t) {}
void retrodepth_write_palette_data(const uint32_t*, uint32_t) {}
void retrodepth_write_palette_thumbs(const uint32_t*, uint32_t) {}
void retrodepth_commit() {}
void retrodepth_read_palette_route(RDPaletteRoute* out) { *out = {}; }
#endif

#ifdef _WIN32

#include <algorithm>

static HANDLE   g_mapping  = nullptr;
static uint8_t* g_shmem    = nullptr;
static uint32_t g_next_slot = 0;

static HANDLE              g_ctrl_mapping = nullptr;
static const RDPaletteRoute* g_ctrl_view  = nullptr;

static RDHeader* header() { return reinterpret_cast<RDHeader*>(g_shmem); }

void retrodepth_init()
{
	if (g_shmem) return;

	g_mapping = CreateFileMappingA(
		INVALID_HANDLE_VALUE, nullptr,
		PAGE_READWRITE, 0, RD_SHMEM_SIZE,
		"Local\\RetroDepthFB4");

	if (!g_mapping) return;

	g_shmem = reinterpret_cast<uint8_t*>(
		MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, RD_SHMEM_SIZE));

	if (!g_shmem) {
		CloseHandle(g_mapping);
		g_mapping = nullptr;
		return;
	}

	RDHeader* hdr = header();
	hdr->magic       = RD_MAGIC;
	hdr->version     = RD_VERSION;
	hdr->frame_id    = 0;
	hdr->layer_count = 0;
}

bool retrodepth_active()
{
	return g_shmem != nullptr;
}

void retrodepth_write_layer(uint32_t z_order, const char* name,
							const uint32_t* bgra_pixels,
							const uint16_t* owner_ids,
							uint32_t width, uint32_t height)
{
	if (!g_shmem) return;
	if (g_next_slot >= RD_MAX_LAYERS) return;
	if (width > RD_MAX_WIDTH || height > RD_MAX_HEIGHT) return;

	uint32_t slot = g_next_slot++;

	RDHeader* hdr = header();
	RDLayerDesc& desc = hdr->layers[slot];
	strncpy_s(desc.name, name, sizeof(desc.name) - 1);
	desc.z_order     = z_order;
	desc.width       = width;
	desc.height      = height;
	desc.data_offset = RD_DATA_OFFSET + slot * (RD_PIXEL_BYTES + RD_OWNER_BYTES);
	desc.owner_offset = desc.data_offset + RD_PIXEL_BYTES;
	desc.flags       = owner_ids ? RD_LAYER_FLAG_HAS_OWNER : 0;

	uint32_t byte_count = width * height * 4;
	memcpy(g_shmem + desc.data_offset, bgra_pixels, byte_count);
	if (owner_ids) {
		uint32_t owner_byte_count = width * height * sizeof(uint16_t);
		memcpy(g_shmem + desc.owner_offset, owner_ids, owner_byte_count);
	}

	hdr->layer_count = g_next_slot;
}

void retrodepth_write_palette_data(const uint32_t* argb_data, uint32_t count)
{
	if (!g_shmem) return;
	uint32_t copy_count = (count < 256u * 16u) ? count : 256u * 16u;
	memcpy(header()->palette_argb, argb_data, copy_count * sizeof(uint32_t));
}

void retrodepth_write_palette_thumbs(const uint32_t* pixels, uint32_t pal_idx)
{
	if (!g_shmem || pal_idx >= 256) return;
	memcpy(header()->pal_thumb[pal_idx], pixels,
	       RD_THUMB_DIM * RD_THUMB_DIM * sizeof(uint32_t));
}

void retrodepth_commit()
{
	if (!g_shmem) return;
	header()->frame_id++;
	g_next_slot = 0;
}

void retrodepth_read_palette_route(RDPaletteRoute* out)
{
	if (!g_ctrl_mapping) {
		g_ctrl_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE,
		                                  "Local\\RetroDepthControl");
		if (g_ctrl_mapping) {
			g_ctrl_view = reinterpret_cast<const RDPaletteRoute*>(
				MapViewOfFile(g_ctrl_mapping, FILE_MAP_READ,
				              0, 0, sizeof(RDPaletteRoute)));
			if (!g_ctrl_view) {
				CloseHandle(g_ctrl_mapping);
				g_ctrl_mapping = nullptr;
			}
		}
	}
	if (g_ctrl_view && g_ctrl_view->magic == RD_CTRL_MAGIC) {
		*out = *g_ctrl_view;
	} else {
		// Default: all palettes → grp0
		out->magic = RD_CTRL_MAGIC;
		memset(out->route, 0, sizeof(out->route));
	}
}

#endif // _WIN32
