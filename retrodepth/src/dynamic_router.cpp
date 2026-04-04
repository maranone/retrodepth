#include "dynamic_router.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

DynamicRouter::DynamicRouter(GameConfig& config)
    : m_config(config)
{
    // Build group → config-layer index mapping, skipping background and fix.
    int g = 0;
    for (int i = 0; i < (int)config.layers.size() && g < 8; ++i) {
        const auto& id = config.layers[i].id;
        if (id != "background" && id != "fix") {
            m_group_layer_idx[g]  = i;
            m_group_orig_depth[g] = config.layers[i].depth_meters;
            ++g;
        }
    }
    m_n_groups = (g > 0) ? g : 4;

    memset(m_spread_ema,   0, sizeof(m_spread_ema));
    memset(m_stable_ticks, 0, sizeof(m_stable_ticks));

    // Seed proposed[] from the current config so we start stable.
    for (int p = 0; p < 256; ++p)
        m_proposed[p] = config.palette_route[p];

    // Write initial route with thumb_requested=false.
    // thumb_requested is only raised for ~5 frames around each check.
    m_writer.write(config.palette_route, /*thumb_requested=*/false);

}

namespace {
float beta_group_threshold(int n_groups, int band) {
    static constexpr float k_thresholds_4[] = { 0.10f, 0.20f, 0.34f };
    if (n_groups == 4 && band >= 0 && band < 3)
        return k_thresholds_4[band];
    float t = (float)(band + 1) / (float)n_groups;
    return t * t * 0.45f;
}
} // namespace

// ---------------------------------------------------------------------------
void DynamicRouter::on_frame(const std::vector<LayerFrame>& frames)
{
    const int update_every = m_use_beta_depth ? k_beta_update_every : k_update_every;

    // Flush any pending commits immediately.
    for (int g = 0; g < m_n_groups; ++g) {
        if (m_group_pending[g].empty()) continue;
        for (auto& [p, dest] : m_group_pending[g]) {
            m_writer.write_entry(p, (uint8_t)dest);
            m_config.palette_route[p] = (uint8_t)dest;
        }
        m_group_pending[g].clear();
    }

    // Gate thumb_requested: raise 3 frames before check so MAME has time
    // to see the flag, render with owner_ids, and have retrodepth read it back.
    if (m_frame_count > 0 && (m_frame_count + 3) % update_every == 0)
        m_writer.set_thumb_requested(true);

    ++m_frame_count;
    if (m_frame_count % update_every == 0) {
        recompute(frames);
        m_writer.set_thumb_requested(false);
    }
}

// ---------------------------------------------------------------------------
void DynamicRouter::recompute(const std::vector<LayerFrame>& frames)
{
    // --- Pass 1: per-palette bounding box from owner_ids ------------------
    // Also build excluded[] — palettes that belong to static layers (background, fix)
    // that should never be dynamically re-routed.
    struct Box {
        int cov = 0;
        int x0 = 9999, y0 = 9999, x1 = -1, y1 = -1;
    };
    Box  boxes[256]    = {};
    bool excluded[256] = {};
    int W = 1, H = 1;

    for (const auto& f : frames) {
        if (f.owner_ids.empty()) continue;
        if (f.width  > W) W = f.width;
        if (f.height > H) H = f.height;
        bool is_static = (f.id == "background" || f.id == "fix");
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                uint16_t owner = f.owner_ids[(size_t)y * f.width + x];
                if (owner == LayerFrame::OWNER_NONE || owner >= 256) continue;
                if (is_static) {
                    excluded[owner] = true;
                    continue;
                }
                Box& b = boxes[owner];
                ++b.cov;
                if (x < b.x0) b.x0 = x;
                if (x > b.x1) b.x1 = x;
                if (y < b.y0) b.y0 = y;
                if (y > b.y1) b.y1 = y;
            }
        }
    }

    // Bail out if MAME didn't supply owner_ids this tick.
    {
        int total_cov = 0;
        for (int p = 0; p < 256; ++p) total_cov += boxes[p].cov;
        if (total_cov == 0)
            return;
    }

    // --- Pass 2: normalised diagonal spread, update EMA -------------------
    const float screen_diag = std::sqrtf((float)(W * W + H * H));
    const float alpha = 0.15f;
    float motion_amount[256] = {};

    for (int p = 0; p < 256; ++p) {
        if (excluded[p]) continue;
        float score = 0.0f;
        if (boxes[p].cov > 0 && boxes[p].x1 >= boxes[p].x0) {
            float dx     = (float)(boxes[p].x1 - boxes[p].x0);
            float dy     = (float)(boxes[p].y1 - boxes[p].y0);
            float spread = std::sqrtf(dx * dx + dy * dy) / screen_diag;
            float box_area = (dx + 1.0f) * (dy + 1.0f);
            float density  = (box_area > 0.0f) ? ((float)boxes[p].cov / box_area) : 0.0f;
            if (m_use_density) {
                score = spread * (1.0f - density);
            } else {
                score = spread;
            }
            if ((m_use_motion || m_use_beta_depth) && m_centroid_valid[p]) {
                float cx     = (boxes[p].x0 + boxes[p].x1) * 0.5f;
                float cy     = (boxes[p].y0 + boxes[p].y1) * 0.5f;
                float mdx    = (cx - m_last_cx[p]) / W;
                float mdy    = (cy - m_last_cy[p]) / H;
                float motion = std::sqrtf(mdx * mdx + mdy * mdy);
                motion_amount[p] = motion;
                // Suppress "far" score for moving palettes → they rank near.
                // Factor 5: motion of ~20% screen/check → full suppression.
                if (m_use_motion)
                    score *= std::max(0.0f, 1.0f - motion * 5.0f);
            }
            if (m_use_beta_depth) {
                // Beta motion promotion:
                // 1. reward compact moving palettes more than wide ones
                // 2. ignore very wide palettes for motion promotion
                float compactness = density * std::max(0.0f, 1.0f - spread * 1.6f);
                float wide_gate   = std::max(0.0f, 1.0f - spread * 2.2f);
                float motion_bias = std::min(0.22f, motion_amount[p] * 3.5f * compactness * wide_gate);
                score = std::max(0.0f, score - motion_bias);
            }
        }
        // Update centroid for next check.
        if (m_use_motion || m_use_beta_depth) {
            if (boxes[p].cov > 0) {
                m_last_cx[p]        = (boxes[p].x0 + boxes[p].x1) * 0.5f;
                m_last_cy[p]        = (boxes[p].y0 + boxes[p].y1) * 0.5f;
                m_centroid_valid[p] = true;
            } else {
                m_centroid_valid[p] = false;
            }
        }
        m_spread_ema[p] = m_spread_ema[p] * (1.0f - alpha) + score * alpha;
    }

    // --- Pass 3: rank by spread, assign candidate group -------------------
    // High spread = background tiles = farthest group.
    // Low  spread = sprites/effects  = nearest group.
    // Sort groups by original depth descending so index 0 = farthest.
    int depth_order[8];
    for (int i = 0; i < m_n_groups; ++i) depth_order[i] = i;
    std::sort(depth_order, depth_order + m_n_groups, [this](int a, int b) {
        return m_group_orig_depth[a] > m_group_orig_depth[b]; // descending = far first
    });

    // Collect only non-excluded palettes for ranking.
    int order[256];
    int n_active = 0;
    for (int i = 0; i < 256; ++i) {
        if (excluded[i]) continue;
        if (m_use_beta_depth && boxes[i].cov <= 0) continue;
        order[n_active++] = i;
    }
    std::sort(order, order + n_active, [this](int a, int b) {
        return m_spread_ema[a] > m_spread_ema[b]; // descending
    });

    int candidate[256] = {};
    int group_rank_by_id[8] = {};
    for (int rank = 0; rank < m_n_groups; ++rank)
        group_rank_by_id[depth_order[rank]] = rank;

    if (m_use_beta_depth) {
        for (int p = 0; p < 256; ++p)
            candidate[p] = (int)m_config.palette_route[p];
        for (int idx = 0; idx < n_active; ++idx) {
            int p = order[idx];
            float score = m_spread_ema[p];
            int depth_rank = 0;
            while (depth_rank < m_n_groups - 1 && score > beta_group_threshold(m_n_groups, depth_rank))
                ++depth_rank;
            candidate[p] = depth_order[depth_rank];
        }
    } else {
        for (int rank = 0; rank < n_active; ++rank) {
            int p          = order[rank];
            int depth_rank = (rank * m_n_groups) / n_active;
            candidate[p]   = depth_order[depth_rank];
        }
    }

    // --- Pass 4: hysteresis — commit on stability -------------------------
    const int required_stability = m_use_beta_depth ? k_beta_stability_ticks : k_stability_ticks;
    for (int p = 0; p < 256; ++p) {
        if (excluded[p]) continue;
        if (m_use_beta_depth && boxes[p].cov <= 0)
            continue;
        int cur  = (int)m_config.palette_route[p];
        int prop = candidate[p];

        if (m_use_beta_depth && boxes[p].cov > 0) {
            int cur_rank = group_rank_by_id[cur];
            int prop_rank = group_rank_by_id[prop];
            bool currently_near = cur_rank >= (m_n_groups / 2);
            bool moving = motion_amount[p] > 0.0125f;
            if (currently_near && moving && prop_rank < cur_rank)
                prop = cur;
        }

        // Also skip if this palette is already pending a transition.
        bool already_pending = false;
        for (int g = 0; g < m_n_groups && !already_pending; ++g)
            for (auto& [pp, _] : m_group_pending[g])
                if (pp == p) { already_pending = true; break; }
        if (already_pending) continue;

        if (prop == cur) {
            m_proposed[p]     = cur;
            m_stable_ticks[p] = 0;
            continue;
        }

        if (m_proposed[p] == prop) {
            ++m_stable_ticks[p];
        } else {
            m_proposed[p]     = prop;
            m_stable_ticks[p] = 1;
        }

        if (m_stable_ticks[p] >= required_stability) {
            int src = cur;
            int dst = prop;

            m_group_pending[src].push_back({p, dst});

            m_stable_ticks[p] = 0;
            m_proposed[p]     = prop;
        }
    }

}
