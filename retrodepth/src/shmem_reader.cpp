#include "shmem_reader.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <unordered_map>

ShmemReader::ShmemReader()
    : m_pal_thumb_buf(new uint32_t[256 * RD_THUMB_DIM * RD_THUMB_DIM]())
{}

ShmemReader::~ShmemReader() {
    if (m_view)    { UnmapViewOfFile(m_view);  m_view    = nullptr; }
    if (m_mapping) { CloseHandle(m_mapping);   m_mapping = nullptr; }
}

bool ShmemReader::try_open() {
    if (m_mapping) return true;
    m_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\RetroDepthFB4");
    if (!m_mapping) return false;
    m_view = MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0); // 0 = map entire object
    if (!m_view) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }
    std::cout << "[ShmemReader] Mapped shared memory (size=0 = full object).\n";
    return true;
}

bool ShmemReader::is_connected() {
    return try_open();
}

std::vector<LayerFrame> ShmemReader::poll(const GameConfig& config) {
    if (!try_open()) return {};

    const auto* hdr  = reinterpret_cast<const RDHeader*>(m_view);
    const auto* base = reinterpret_cast<const uint8_t*>(m_view);

    if (hdr->magic != RD_MAGIC || hdr->version != RD_VERSION) {
        static bool once = false;
        if (!once) {
            std::cout << "[ShmemReader] Bad magic/version: magic=0x" << std::hex << hdr->magic
                      << " ver=" << std::dec << hdr->version << "\n";
            once = true;
        }
        return {};
    }
    if (hdr->frame_id == m_last_frame_id) return {};

    // Slow-motion debug: limit how fast we consume new frames.
    // Set to 0 to disable, or e.g. 500 for 2 frames/sec.
    static constexpr DWORD SLOWMO_MS = 0; // ms per frame (0 = off)
    if constexpr (SLOWMO_MS > 0) {
        static DWORD s_last_consume = 0;
        DWORD now = GetTickCount();
        if (now - s_last_consume < SLOWMO_MS) return {};
        s_last_consume = now;
    }

    m_last_frame_id = hdr->frame_id;

    // Snapshot palette data and thumbnails
    memcpy(m_palette, hdr->palette_argb, sizeof(m_palette));
    m_has_palette = true;
    if (m_thumb_needed) {
        memcpy(m_pal_thumb_buf.get(), hdr->pal_thumb, 256 * RD_THUMB_DIM * RD_THUMB_DIM * sizeof(uint32_t));
        m_has_thumb = true;
    }

    const uint32_t incoming_count = (std::min)(hdr->layer_count, RD_MAX_LAYERS);
    const bool snes_8_bucket_layout =
        config.layers.size() == 8 &&
        config.layers[0].id == "background" &&
        config.layers[1].id == "bg3" &&
        config.layers[2].id == "bg2_low" &&
        config.layers[3].id == "bg2_high" &&
        config.layers[4].id == "bg1_low" &&
        config.layers[5].id == "bg1_high" &&
        config.layers[6].id == "sprites_low" &&
        config.layers[7].id == "sprites_high";
    const bool genesis_7_bucket_layout =
        config.layers.size() == 7 &&
        config.layers[0].id == "background" &&
        config.layers[1].id == "plane_b_low" &&
        config.layers[2].id == "plane_b_high" &&
        config.layers[3].id == "plane_a_low" &&
        config.layers[4].id == "plane_a_high" &&
        config.layers[5].id == "sprites_low" &&
        config.layers[6].id == "sprites_high";
    // Expanded console layouts keep the VR depth range stable even while
    // their current MAME exporters still emit fewer coarse layers.
    const bool auto_spread = !config.layers.empty() &&
        (incoming_count > config.layers.size() ||
         ((snes_8_bucket_layout || genesis_7_bucket_layout) &&
          incoming_count != config.layers.size()));

    float far_depth = 1.5f;
    float near_depth = 1.5f;
    float default_width = 2.0f;
    if (!config.layers.empty()) {
        far_depth = config.layers.front().depth_meters;
        near_depth = config.layers.front().depth_meters;
        default_width = config.layers.front().quad_width_meters;
        for (const auto& lc : config.layers) {
            far_depth = (std::max)(far_depth, lc.depth_meters);
            near_depth = (std::min)(near_depth, lc.depth_meters);
        }
    }

    auto find_name_match = [&](const std::string& layer_name) -> const LayerConfig* {
        for (const auto& lc : config.layers)
            if (lc.id == layer_name) return &lc;
        return nullptr;
    };

    auto find_positional_match = [&](uint32_t z_order) -> const LayerConfig* {
        if (z_order < config.layers.size())
            return &config.layers[z_order];
        return nullptr;
    };

    auto find_nearest_template = [&](float depth) -> const LayerConfig* {
        if (config.layers.empty()) return nullptr;
        const LayerConfig* best = &config.layers.front();
        float best_delta = std::fabs(best->depth_meters - depth);
        for (const auto& lc : config.layers) {
            float delta = std::fabs(lc.depth_meters - depth);
            if (delta < best_delta) {
                best = &lc;
                best_delta = delta;
            }
        }
        return best;
    };

    std::vector<LayerFrame> frames;
    for (uint32_t i = 0; i < incoming_count; ++i) {
        const RDLayerDesc& desc = hdr->layers[i];
        if (desc.width == 0 || desc.height == 0) continue;
        if (desc.width > RD_MAX_WIDTH || desc.height > RD_MAX_HEIGHT) continue;

        size_t name_len = 0;
        while (name_len < sizeof(desc.name) && desc.name[name_len] != '\0')
            ++name_len;
        const std::string name(desc.name, name_len);

        // Match by name first; fall back to z_order position so old MAME
        // binaries (sending "slot0"/"slot32"/"slot64") work with the new config.
        const LayerConfig* matched_by_name = find_name_match(name);
        const LayerConfig* matched_by_position = find_positional_match(desc.z_order);
        const LayerConfig* matched = matched_by_name ? matched_by_name : matched_by_position;

        if (!matched && !auto_spread) continue;

        float depth_meters = matched ? matched->depth_meters : near_depth;
        const LayerConfig* template_cfg = auto_spread ? matched_by_name : matched;
        if (auto_spread) {
            const uint32_t auto_slot = (desc.z_order < incoming_count) ? desc.z_order : i;
            const float t = (incoming_count > 1)
                ? (float)auto_slot / (float)(incoming_count - 1)
                : 0.0f;
            depth_meters = far_depth + t * (near_depth - far_depth);
            if (!template_cfg)
                template_cfg = find_nearest_template(depth_meters);
        }

        if (!template_cfg && !config.layers.empty())
            template_cfg = &config.layers.front();

        LayerFrame lf;
        if (auto_spread) {
            if (matched_by_name) lf.id = matched_by_name->id;
            else if (!name.empty()) lf.id = name;
            else if (template_cfg) lf.id = template_cfg->id;
            else lf.id = "layer" + std::to_string(i);
        } else {
            lf.id = matched->id; // use config id, not raw MAME name
        }
        lf.width             = (int)desc.width;
        lf.height            = (int)desc.height;
        lf.depth_meters      = depth_meters;
        lf.quad_width_meters = template_cfg ? template_cfg->quad_width_meters : default_width;
        lf.copies            = template_cfg ? template_cfg->copies : std::vector<float>{};

        size_t pixel_count = (size_t)desc.width * (size_t)desc.height;
        size_t rgba_bytes = pixel_count * 4u;
        if (pixel_count == 0 || rgba_bytes / 4u != pixel_count)
            continue;
        if ((uint64_t)desc.data_offset + (uint64_t)rgba_bytes > (uint64_t)RD_SHMEM_SIZE)
            continue;

        lf.rgba.resize(pixel_count * 4);
        memcpy(lf.rgba.data(), base + desc.data_offset, rgba_bytes);
        if (desc.flags & RD_LAYER_FLAG_HAS_OWNER) {
            size_t owner_bytes = pixel_count * sizeof(uint16_t);
            if (owner_bytes / sizeof(uint16_t) != pixel_count)
                continue;
            if ((uint64_t)desc.owner_offset + (uint64_t)owner_bytes > (uint64_t)RD_SHMEM_SIZE)
                continue;
            lf.owner_ids.resize(pixel_count);
            memcpy(lf.owner_ids.data(), base + desc.owner_offset, owner_bytes);
        } else {
            lf.owner_ids.clear();
        }

        // MAME bitmap_rgb32 never writes the alpha byte (stays 0 = fully transparent).
        uint8_t* px = lf.rgba.data();
        const bool is_anchor = (desc.z_order == 0 || name == "fix");

        if (is_anchor) {
            // Anchor layers (background, fix) are hardware-guaranteed stable every frame.
            // Just apply the sentinel → transparent pass.
            for (size_t j = 0; j < pixel_count; ++j) {
                uint8_t b = px[j*4+0], g = px[j*4+1], r = px[j*4+2];
                px[j*4+3] = (b == 1 && g == 0 && r == 0) ? 0 : 0xFF;
            }
            // Background solid-colour: force fully opaque (no sentinel tiles).
            if (desc.z_order == 0) {
                for (size_t j = 0; j < pixel_count; ++j)
                    px[j*4+3] = 0xFF;
            }
            // Fix layer: crop black overscan bars on the left and right edges.
            if (name == "fix" && lf.width > 0 && lf.height > 0) {
                const int w = lf.width, h = lf.height;
                auto col_has_color = [&](int col) {
                    for (int row = 0; row < h; ++row) {
                        size_t j = (size_t)row * w + col;
                        if (px[j*4+3] && (px[j*4+0] || px[j*4+1] || px[j*4+2]))
                            return true;
                    }
                    return false;
                };
                int left = 0;
                while (left < w / 2 && !col_has_color(left))   ++left;
                int right = w - 1;
                while (right > w / 2 && !col_has_color(right)) --right;
                for (int row = 0; row < h; ++row) {
                    for (int col = 0; col < left; ++col)
                        px[((size_t)row * w + col) * 4 + 3] = 0;
                    for (int col = right + 1; col < w; ++col)
                        px[((size_t)row * w + col) * 4 + 3] = 0;
                }
            }
        } else {
            // Sprite layers — MAME fills with sentinel 1 (b=1,g=0,r=0) before drawing.
            // Pass 1: mark every pixel opaque except the exact sentinel value.
            for (size_t j = 0; j < pixel_count; ++j) {
                uint8_t b = px[j*4+0], g = px[j*4+1], r = px[j*4+2];
                px[j*4+3] = (b == 1 && g == 0 && r == 0) ? 0 : 0xFF;
            }
            // Pass 2: layer-level black-frame removal.
            bool has_colored = false;
            for (size_t j = 0; j < pixel_count && !has_colored; ++j)
                if (px[j*4+3] && (px[j*4+0] || px[j*4+1] || px[j*4+2]))
                    has_colored = true;
            if (!has_colored)
                for (size_t j = 0; j < pixel_count; ++j)
                    px[j*4+3] = 0;
        }

        frames.push_back(std::move(lf));
    }

    // Sort back-to-front so alpha blending composites correctly
    std::sort(frames.begin(), frames.end(),
        [](const LayerFrame& a, const LayerFrame& b) {
            return a.depth_meters > b.depth_meters;
        });

    // Debug: print frame number only when a depth change > 1 cm is detected.
    {
        static std::unordered_map<std::string, float> s_last_depths;
        static int s_frame = 0;
        ++s_frame;
        bool header_printed = false;
        for (const auto& f : frames) {
            auto it = s_last_depths.find(f.id);
            if (it != s_last_depths.end() && std::fabs(f.depth_meters - it->second) > 0.01f) {
                if (!header_printed) {
                    std::cout << "[frame " << (s_frame % 60) << "/60]\n";
                    header_printed = true;
                }
                std::cout << "  [depth change] " << f.id
                          << "  " << it->second << " -> " << f.depth_meters << " m\n";
            }
            s_last_depths[f.id] = f.depth_meters;
        }
    }

    return frames;
}
