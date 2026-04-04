#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <array>
#include <string>
#include "game_config.h"
#include "shmem_reader.h"
#include "d3d11_renderer.h"
#include "layer_editor.h"
#include "spectator_window.h"
#include "dynamic_router.h"

using Microsoft::WRL::ComPtr;

// Per-eye swapchain data
struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t     width  = 0;
    int32_t     height = 0;
    std::vector<ID3D11Texture2D*> images; // non-owning; owned by OpenXR
};

struct VrPresetLayerState {
    float depth_meters = 1.0f;
    float quad_width_meters = 2.0f;
    std::vector<float> copies;
};

struct VrPresetState {
    std::string name = "Preset";
    float gamma = 1.15f;
    float contrast = 0.90f;
    float saturation = 0.80f;
    bool layers3d = false;
    bool depthmap = false;
    bool depthmap_mirror = false;
    bool upscale = false;
    bool shadows = false;
    bool ambilight = false;
    float screen_curve = 0.0f;
    bool rotate_screen = false;
    float tilt_x = 0.0f;
    float tilt_y = 0.0f;
    float roundness = 0.0f;
    float quad_y_meters = 1.6f;
    std::array<float, 4> bg_color = {0.02f, 0.02f, 0.05f, 1.0f};
    std::array<uint8_t, 256> palette_route = {};
    std::vector<VrPresetLayerState> layers;
};

class XrApp {
public:
    explicit XrApp(GameConfig config);
    ~XrApp();
    void set_spectator_enabled(bool v) { m_spectator_enabled = v; }
    void set_dynamic_mode(bool v);
    void set_beta_depth(bool v)     { m_launch_beta_depth = v; if (m_dynamic_router) m_dynamic_router->set_beta_depth(v); }
    void set_density_scoring(bool v) { m_launch_density = v; if (m_dynamic_router) m_dynamic_router->set_density_scoring(v); }
    void set_motion_scoring(bool v)  { m_launch_motion = v; if (m_dynamic_router) m_dynamic_router->set_motion_scoring(v); }
    void set_hide_mame(bool v)       { m_launch_hide_mame = v; }

    // Runs until the user exits. Blocks.
    void run();

private:
    void create_instance();
    void create_system();
    void create_d3d11_device();
    void create_session();
    void create_swapchains();
    void create_depth_buffers();
    void create_spaces();
    void destroy();

    void poll_events(bool& should_exit);
    void handle_session_state_changed(XrEventDataSessionStateChanged* ev, bool& should_exit);
    void render_frame();
    void render_eye(int eye_idx, ID3D11Texture2D* color_tex,
                    ID3D11Texture2D* depth_tex,
                    const XrView& view, const XrSwapchainImageD3D11KHR* img);

    void init_actions();
    void poll_actions(XrTime predicted_display_time, const XrView* views, uint32_t view_count);
    void apply_runtime_vr_state();
    VrPresetState capture_current_vr_state() const;
    void apply_vr_preset_state(const VrPresetState& state);
    void load_vr_preset(int idx);
    void save_current_to_vr_preset(int idx);
    void randomize_vr_state();
    void save_vr_presets() const;
    void load_vr_presets();
    void reset_vr_presets_to_defaults();
    void build_default_vr_presets();
    bool launch_random_validated_game();

    GameConfig        m_config;
    LayerEditor       m_editor{m_config}; // must be after m_config
    GameConfig        m_factory_default_config;
    ShmemReader       m_shmem;
    std::vector<LayerFrame> m_last_frames;

    // Eye-height locking (F5 to snap/lock, F5 again to resume live tracking)
    float             m_locked_eye_y  = 0.0f;
    bool              m_eye_y_locked  = false;
    bool              m_f5_was_down   = false;
    float             m_scene_yaw     = 0.0f;
    float             m_scene_x       = 0.0f;
    float             m_scene_z       = 0.0f;

    // --- OpenXR ---
    XrInstance        m_instance  = XR_NULL_HANDLE;
    XrSystemId        m_system_id = XR_NULL_SYSTEM_ID;
    XrSession         m_session   = XR_NULL_HANDLE;
    XrSpace           m_stage_space = XR_NULL_HANDLE;
    XrSessionState    m_session_state = XR_SESSION_STATE_UNKNOWN;
    bool              m_session_running = false;

    int64_t           m_color_format = 0;
    int64_t           m_depth_format = 0;

    EyeSwapchain      m_eye_swapchain[2];
    // Per-eye depth buffers (we manage these, not OpenXR)
    ComPtr<ID3D11Texture2D>          m_depth_tex[2];
    ComPtr<ID3D11DepthStencilView>   m_depth_dsv[2];

    // --- D3D11 ---
    ComPtr<ID3D11Device>        m_d3d_device;
    ComPtr<ID3D11DeviceContext> m_d3d_ctx;
    std::unique_ptr<D3D11Renderer> m_renderer;

    // --- Swapchain image arrays (indexed per eye) ---
    std::vector<XrSwapchainImageD3D11KHR> m_eye_images[2];

    // Extension function pointers
    PFN_xrGetD3D11GraphicsRequirementsKHR m_pfn_get_d3d11_reqs = nullptr;

    // --- Controller input ---
public:
    void set_mame_hwnd(HWND h) { m_mame_hwnd = h; }
private:
    // Actions
    XrActionSet m_action_set    = XR_NULL_HANDLE;
    XrAction    m_act_move      = XR_NULL_HANDLE; // left stick  → UDLR keys
    XrAction    m_act_a         = XR_NULL_HANDLE; // right A     → fire 1 (Z)
    XrAction    m_act_b         = XR_NULL_HANDLE; // right B     → fire 2 (X)
    XrAction    m_act_trig      = XR_NULL_HANDLE; // right index → fire 3 (C)
    XrAction    m_act_grip      = XR_NULL_HANDLE; // right grip  → fire 4 (V)
    XrAction    m_act_start     = XR_NULL_HANDLE; // left X      → start  (1)
    XrAction    m_act_coin      = XR_NULL_HANDLE; // left Y      → coin   (5)
    XrAction    m_act_depth     = XR_NULL_HANDLE; // right stick  → depth control
    XrAction    m_act_lstick    = XR_NULL_HANDLE; // left stick click → randomize VR state
    XrAction    m_act_rstick    = XR_NULL_HANDLE; // right stick click → recenter scene
    XrAction    m_act_pause     = XR_NULL_HANDLE; // right menu/system → MAME pause
    XrAction    m_act_widen    = XR_NULL_HANDLE; // left trigger → widen all layers
    XrAction    m_act_narrow   = XR_NULL_HANDLE; // left grip    → narrow all layers
    XrAction    m_act_pose[2]  = {};             // grip poses left[0] right[1]
    XrSpace     m_ctrl_space[2] = {};            // controller spaces

    // Aim spaces (OpenXR aim pose — better for pointing than grip)
    XrAction    m_act_aim[2]   = {};
    XrSpace     m_aim_space[2] = {};

    HWND        m_mame_hwnd     = nullptr;

    // Edge-detection: previous pressed state per button (A B trig grip start coin)
    bool        m_btn_prev[6]   = {};
    bool        m_lstick_prev   = false;
    bool        m_rstick_prev   = false;
    bool        m_pause_prev    = false;
    bool        m_editor_mode   = false;
    bool        m_escape_prev   = false;
    // Which directional keys are currently held (to know when to release)
    bool        m_dir_held[4]   = {}; // UP DOWN LEFT RIGHT
    ULONGLONG   m_stick_next_fire = 0; // rate-limiter for right stick depth control
    ULONGLONG   m_width_next_fire = 0; // rate-limiter for left trigger/grip width control

    // --- Laser pointer state ---
    // Laser endpoints in stage space (computed each frame by update_laser_hits)
    XMFLOAT3    m_laser_origin[2]    = {};
    XMFLOAT3    m_laser_tip[2]       = {};
    bool        m_laser_on_screen[2] = {};
    bool        m_laser_on_panel[2]  = {};
    int         m_laser_panel_btn[2] = {-1, -1}; // 0=LoadDefault, 1=LoadSaved, 2=Editor, 16=3DLayers, 17=Depthmap, 18=Mirror, 19=Upscale, 20=Shadows, 21=Ambilight, 22=EvenSpread, 23=Curve-, 24=Curve+, 100-104=PresetLoad, 110-114=PresetSave, 200=RandomGame
    float       m_laser_panel_pu[2] = {};         // horizontal UV on panel (0=left, 1=right) for +/- discrimination

    int         m_hovered_palette    = -1;
    int         m_hovered_group      = 0;
    int         m_hovered_layer      = -1;
    int         m_hovered_px         = -1;
    int         m_hovered_py         = -1;
    int         m_hovered_w          = 0;
    int         m_hovered_h          = 0;
    std::string m_hovered_matches;
    bool        m_rtrig_prev         = false;
    float       m_eye_y_cache        = 1.6f; // last computed eye_y for ray test

    bool        m_rotate_screen      = false; // 90° CW UV rotation for tate/vertical games
    float       m_screen_tilt_x     = 0.0f;  // up/down tilt in radians (+ = pinball lean)
    float       m_screen_tilt_y     = 0.0f;  // left/right angle in radians (+ = right)
    XMFLOAT4    m_bg_color           = {0.02f, 0.02f, 0.05f, 1.0f}; // VR clear color
    int         m_colorpal_hover[2]  = {-1, -1}; // hovered swatch index per hand
    bool        m_on_colorpal[2]     = {};

    // VR color grading (applied via renderer, controlled from left panel)
    float       m_vr_gamma           = 1.15f;
    float       m_vr_contrast        = 0.90f;
    float       m_vr_saturation      = 0.80f;
    bool        m_vr_3d_layers       = false;
    bool        m_vr_depthmap        = false;
    bool        m_vr_depthmap_mirror = false;
    bool        m_vr_upscale         = false;
    bool        m_vr_shadows         = false;
    bool        m_vr_ambilight       = false;
    float       m_vr_screen_curve    = 0.0f;
    float       m_vr_roundness       = 0.0f;  // sphere/cone effect on depth copies
    int         m_active_vr_preset   = 0;
    std::array<VrPresetState, 5> m_vr_presets = {};
    std::array<VrPresetState, 5> m_default_vr_presets = {};
    bool        m_spectator_enabled  = false;
    bool        m_launch_dynamic     = false;
    bool        m_launch_beta_depth  = false;
    bool        m_launch_density     = false;
    bool        m_launch_motion      = false;
    bool        m_launch_hide_mame   = false;
    bool        m_request_exit       = false;
    std::unique_ptr<SpectatorWindow> m_spectator;
    std::unique_ptr<DynamicRouter>   m_dynamic_router;

    void update_laser_hits(XrTime time, float eye_y);
    void save_vr_grading();
    void load_vr_grading();
};
