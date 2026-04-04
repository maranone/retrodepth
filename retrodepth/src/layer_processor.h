#pragma once
#include "game_config.h"
#include <vector>
#include <cstdint>

struct LayerFrame {
    static constexpr uint16_t OWNER_NONE = 0xFFFFu;
    std::string id;
    float depth_meters;
    float quad_width_meters;
    std::vector<float> copies;
    int   width;
    int   height;
    std::vector<uint8_t> rgba; // BGRA, 4 bytes/pixel
    std::vector<uint16_t> owner_ids; // optional per-pixel palette owner ids
};

// Slices a captured BGRA frame into per-layer BGRA+alpha frames
// according to the game config extraction rules.
class LayerProcessor {
public:
    explicit LayerProcessor(const GameConfig& config);

    // src: BGRA pixels (from WindowCapture), src_w x src_h
    // Returns one LayerFrame per layer in the config.
    std::vector<LayerFrame> process(
        const uint8_t* src, int src_w, int src_h);

private:
    const GameConfig& m_config;

    LayerFrame extract_full_frame(const LayerConfig& lc,
        const uint8_t* src, int src_w, int src_h);
    LayerFrame extract_region(const LayerConfig& lc,
        const uint8_t* src, int src_w, int src_h);
    LayerFrame extract_color_key(const LayerConfig& lc,
        const uint8_t* src, int src_w, int src_h, bool invert);
};
