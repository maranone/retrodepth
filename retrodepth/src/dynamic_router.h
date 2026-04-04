#pragma once
#include <vector>
#include <utility>
#include "game_config.h"
#include "layer_processor.h"
#include "palette_control.h"

// ---------------------------------------------------------------------------
// DynamicRouter
// ---------------------------------------------------------------------------
// Runs every k_update_every frames (~5 s). For each palette 0-63 it computes
// the "spatial spread" of that palette's pixels across the screen using
// owner_ids data in the LayerFrames.
//
// Palettes whose pixels cover a wide area (background tiles) are placed on
// the far depth group.  Concentrated palettes (characters, projectiles) go
// on the near group.  This heuristic is perspective-independent.
//
// Transitions: when a palette P in source group A is committed to destination
// group B, the SOURCE group A animates its depth from D_A toward D_B over
// k_trans_frames (~2.5 s, smoothstep).  P is still rendered in group A during
// the animation, so it visually slides from D_A to D_B.  When the animation
// ends the MAME route byte is written and group A snaps back to D_A.
// Destination group B (and its existing palettes) are never disturbed.
// ---------------------------------------------------------------------------

class DynamicRouter {
public:
    explicit DynamicRouter(GameConfig& config);

    // Call every frame with the latest layer data.
    void on_frame(const std::vector<LayerFrame>& frames);

    // Number of palette groups (grp0..grpN-1) found in the config.
    int n_groups() const { return m_n_groups; }

    // When enabled, uses spread×(1−density) so large dense sprites
    // (fighters) score lower than sparse background tiles.
    void set_density_scoring(bool v) { m_use_density = v; }

    // When enabled, palettes whose centroid moves significantly between checks
    // (animated sprites / fighters) are pushed toward the near group.
    void set_motion_scoring(bool v) { m_use_motion = v; }

    // Experimental classifier:
    // - only visible palettes participate
    // - absolute score thresholds instead of percentile ranking
    // - slower / stickier commits
    // - moving palettes that are already near resist being pushed far again
    void set_beta_depth(bool v) { m_use_beta_depth = v; }

private:
    void recompute(const std::vector<LayerFrame>& frames);

    GameConfig&        m_config;
    PaletteRouteWriter m_writer;

    int   m_n_groups    = 4;
    int   m_frame_count = 0;

    bool  m_use_density       = false;
    bool  m_use_motion        = false;
    bool  m_use_beta_depth    = false;
    float m_last_cx[256]      = {};  // bounding-box centroid X from previous check
    float m_last_cy[256]      = {};  // bounding-box centroid Y from previous check
    bool  m_centroid_valid[256] = {};

    float m_spread_ema[256]   = {};  // exponential moving average of spread score
    int   m_proposed[256]     = {};  // group most recently proposed for each palette
    int   m_stable_ticks[256] = {};  // consecutive ticks proposing the same new group

    // Per-group state (max 8 groups)
    int   m_group_layer_idx[8]  = {};  // index into m_config.layers
    float m_group_orig_depth[8] = {};  // original depth from config

    // Pending MAME route writes per source group.
    // Each entry is (palette_index, destination_group).
    std::vector<std::pair<int,int>> m_group_pending[8];

    // --- Timing constants ---
    static constexpr int k_update_every         = 20;  // ~0.33 s at 60 fps
    static constexpr int k_stability_ticks      = 3;   // ~0.5 s of consistent signal to commit
    static constexpr int k_beta_update_every    = 30;  // ~0.5 s at 60 fps
    static constexpr int k_beta_stability_ticks = 8;   // ~4.0 s of consistent signal to commit
};
