#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <memory>
#include <vector>
#include <cstdint>
#include "layer_processor.h"  // LayerFrame
#include "game_config.h"

// ---------------------------------------------------------------------------
// Shared memory protocol — must match retrodepth.h in the MAME source tree
// ---------------------------------------------------------------------------
static constexpr uint32_t RD_MAX_LAYERS  = 8;
static constexpr uint32_t RD_MAX_WIDTH   = 512;
static constexpr uint32_t RD_MAX_HEIGHT  = 256;
static constexpr uint32_t RD_THUMB_DIM   = 32;
static constexpr uint32_t RD_DATA_OFFSET = 1179648u; // 1.125 MB — must match MAME
static constexpr uint32_t RD_PIXEL_BYTES = RD_MAX_WIDTH * RD_MAX_HEIGHT * 4;
static constexpr uint32_t RD_OWNER_BYTES = RD_MAX_WIDTH * RD_MAX_HEIGHT * 2;
static constexpr uint32_t RD_SHMEM_SIZE  = RD_DATA_OFFSET + (RD_PIXEL_BYTES + RD_OWNER_BYTES) * RD_MAX_LAYERS;
static constexpr uint32_t RD_MAGIC       = 0x52445650; // 'RDVP'
static constexpr uint32_t RD_VERSION     = 5;
static constexpr uint32_t RD_LAYER_FLAG_HAS_OWNER = 0x1u;
static constexpr uint16_t RD_OWNER_NONE  = 0xFFFFu;

#pragma pack(push, 1)
struct RDLayerDesc {
    char     name[32];
    uint32_t z_order;
    uint32_t width;
    uint32_t height;
    uint32_t data_offset;
    uint32_t owner_offset;
    uint32_t flags;
};
struct RDHeader {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    frame_id;
    uint32_t    layer_count;
    RDLayerDesc layers[RD_MAX_LAYERS];
    uint32_t    palette_argb[256][16];
    uint32_t    pal_thumb[256][RD_THUMB_DIM * RD_THUMB_DIM];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
class ShmemReader {
public:
    ShmemReader();
    ~ShmemReader();

    bool is_connected();

    // Returns new LayerFrames when frame_id advances; empty otherwise.
    std::vector<LayerFrame> poll(const GameConfig& config);

    // Returns the latest palette data (256×16 ARGB values).
    // Valid after the first successful poll(). nullptr if not yet connected.
    const uint32_t (*get_palettes() const)[16] {
        return m_has_palette ? m_palette : nullptr;
    }
    const uint32_t (*get_thumbs() const)[RD_THUMB_DIM * RD_THUMB_DIM] {
        if (!m_has_thumb) return nullptr;
        return reinterpret_cast<const uint32_t (*)[RD_THUMB_DIM * RD_THUMB_DIM]>(m_pal_thumb_buf.get());
    }
    // Enable/disable the 1 MB thumbnail copy each frame.
    // Set true only while the palette editor is open.
    void set_thumb_needed(bool v) { m_thumb_needed = v; }

private:
    bool try_open();

    HANDLE   m_mapping       = nullptr;
    void*    m_view          = nullptr;
    uint32_t m_last_frame_id = 0xFFFFFFFF;

    uint32_t m_palette[256][16] = {};
    bool     m_has_palette      = false;
    // Heap-allocated (1 MB) to avoid stack overflow when ShmemReader is a local variable.
    std::unique_ptr<uint32_t[]> m_pal_thumb_buf;
    bool     m_has_thumb        = false;
    bool     m_thumb_needed     = false;
};
