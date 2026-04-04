#include "layer_processor.h"
#include <algorithm>
#include <cstring>
#include <cmath>

LayerProcessor::LayerProcessor(const GameConfig& config)
    : m_config(config) {}

std::vector<LayerFrame> LayerProcessor::process(
    const uint8_t* src, int src_w, int src_h)
{
    std::vector<LayerFrame> result;
    result.reserve(m_config.layers.size());

    for (const auto& lc : m_config.layers) {
        switch (lc.extraction_type) {
        case ExtractionType::FullFrame:
            result.push_back(extract_full_frame(lc, src, src_w, src_h));
            break;
        case ExtractionType::Region:
            result.push_back(extract_region(lc, src, src_w, src_h));
            break;
        case ExtractionType::ColorKey:
            result.push_back(extract_color_key(lc, src, src_w, src_h, false));
            break;
        case ExtractionType::ColorKeyInverted:
            result.push_back(extract_color_key(lc, src, src_w, src_h, true));
            break;
        }
    }
    return result;
}

LayerFrame LayerProcessor::extract_full_frame(
    const LayerConfig& lc, const uint8_t* src, int src_w, int src_h)
{
    LayerFrame f;
    f.id               = lc.id;
    f.depth_meters     = lc.depth_meters;
    f.quad_width_meters = lc.quad_width_meters;
    f.width            = src_w;
    f.height           = src_h;
    f.rgba.resize(src_w * src_h * 4);

    // src is BGRA from BitBlt; copy as-is and set alpha=255
    for (int i = 0; i < src_w * src_h; ++i) {
        f.rgba[i*4+0] = src[i*4+0]; // B
        f.rgba[i*4+1] = src[i*4+1]; // G
        f.rgba[i*4+2] = src[i*4+2]; // R
        f.rgba[i*4+3] = 255;
    }
    return f;
}

LayerFrame LayerProcessor::extract_region(
    const LayerConfig& lc, const uint8_t* src, int src_w, int src_h)
{
    int rx = lc.rect[0], ry = lc.rect[1];
    int rw = lc.rect[2], rh = lc.rect[3];

    // Clamp to source bounds
    rx = std::clamp(rx, 0, src_w);
    ry = std::clamp(ry, 0, src_h);
    rw = std::clamp(rw, 0, src_w - rx);
    rh = std::clamp(rh, 0, src_h - ry);

    LayerFrame f;
    f.id               = lc.id;
    f.depth_meters     = lc.depth_meters;
    f.quad_width_meters = lc.quad_width_meters;
    f.width            = src_w;
    f.height           = src_h;
    f.rgba.resize(src_w * src_h * 4, 0); // transparent by default

    for (int y = ry; y < ry + rh; ++y) {
        for (int x = rx; x < rx + rw; ++x) {
            int si = (y * src_w + x) * 4;
            int di = (y * src_w + x) * 4;
            f.rgba[di+0] = src[si+0];
            f.rgba[di+1] = src[si+1];
            f.rgba[di+2] = src[si+2];
            f.rgba[di+3] = 255;
        }
    }
    return f;
}

LayerFrame LayerProcessor::extract_color_key(
    const LayerConfig& lc, const uint8_t* src, int src_w, int src_h,
    bool invert)
{
    LayerFrame f;
    f.id               = lc.id;
    f.depth_meters     = lc.depth_meters;
    f.quad_width_meters = lc.quad_width_meters;
    f.width            = src_w;
    f.height           = src_h;
    f.rgba.resize(src_w * src_h * 4);

    const int tol = lc.tolerance;
    const int kr = lc.color[0]; // Note: JSON uses RGB order
    const int kg = lc.color[1];
    const int kb = lc.color[2];

    for (int i = 0; i < src_w * src_h; ++i) {
        // BitBlt output is BGRA
        uint8_t b = src[i*4+0];
        uint8_t g = src[i*4+1];
        uint8_t r = src[i*4+2];

        bool match = (std::abs(r - kr) <= tol &&
                      std::abs(g - kg) <= tol &&
                      std::abs(b - kb) <= tol);

        // invert=false (ColorKey): matching pixels become transparent
        // invert=true  (ColorKeyInverted): non-matching pixels become transparent
        uint8_t alpha = (match == invert) ? 255 : 0;

        f.rgba[i*4+0] = b;
        f.rgba[i*4+1] = g;
        f.rgba[i*4+2] = r;
        f.rgba[i*4+3] = alpha;
    }
    return f;
}
