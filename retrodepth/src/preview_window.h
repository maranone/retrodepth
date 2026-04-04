#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <memory>
#include "game_config.h"
#include "shmem_reader.h"
#include "d3d11_renderer.h"
#include "layer_editor.h"
#include "editor_info_window.h"
#include "dynamic_router.h"

using Microsoft::WRL::ComPtr;

// Standalone desktop preview window — no OpenXR required.
// Camera oscillates left/right so parallax between layers is visible.
class PreviewWindow {
public:
    explicit PreviewWindow(GameConfig config);
    ~PreviewWindow();
    void run();
    void set_auto_exit_ms(uint32_t v) { m_auto_exit_ms = v; }

private:
    void create_window();
    void create_d3d11();
    void create_swapchain();
    void resize(int w, int h);
    void render_frame();
    void refresh_status_ui();
    void apply_renderer_state();
    void refresh_last_frames_from_config();
    void randomize_preview_state();
    bool launch_random_validated_game();
    void set_3d_layers_enabled(bool v);
    void set_depthmap_enabled(bool v);
    void set_depthmap_mirror_enabled(bool v);
    void set_upscale_enabled(bool v);
    void set_shadows_enabled(bool v);
    void set_ambilight_enabled(bool v);
    void set_screen_curve(float v);

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    GameConfig        m_config;
    LayerEditor       m_editor{m_config};              // must be after m_config
    EditorInfoWindow  m_info_win{m_config, m_editor};  // must be after m_editor
    ShmemReader    m_shmem;
    std::vector<LayerFrame> m_last_frames;
    HWND           m_mame_hwnd = nullptr;

public:
    void set_mame_hwnd(HWND h) { m_mame_hwnd = h; m_info_win.set_mame_hwnd(h); }
    void set_dynamic_mode(bool v);
    void set_beta_depth(bool v);
    void set_density_scoring(bool v);
    void set_motion_scoring(bool v);
private:

    HWND  m_hwnd   = nullptr;
    bool  m_running = true;
    int   m_width  = 1280;
    int   m_height = 720;
    // Orbit camera state
    float m_yaw      =  0.0f;   // horizontal angle (radians)
    float m_pitch    =  0.0f;   // vertical angle (radians; 0 = looking straight ahead)
    float m_distance =  3.0f;   // metres from anchor (overwritten on first frame)
    bool  m_dist_init  = false;
    bool  m_dragging   = false;
    int   m_last_mx  =  0;
    int   m_last_my  =  0;
    bool  m_3d_layers_enabled = false;
    bool  m_depthmap_enabled = false;
    bool  m_depthmap_mirror_enabled = false;
    bool  m_upscale_enabled  = false;
    bool  m_shadows_enabled  = false;
    bool  m_ambilight_enabled = false;
    bool  m_rotate_screen    = false;
    bool  m_launch_dynamic   = false;
    bool  m_launch_beta_depth = false;
    bool  m_launch_density   = false;
    bool  m_launch_motion    = false;
    float m_screen_curve     = 0.0f;
    float m_contrast         = 1.0f;
    float m_saturation       = 1.0f;
    float m_gamma            = 1.0f;
    float m_roundness        = 0.0f;
    DirectX::XMFLOAT4 m_bg_color = {0.03f, 0.06f, 0.18f, 1.0f};
    bool  m_status_ui_dirty  = true;
    uint32_t m_auto_exit_ms  = 0;

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_ctx;
    ComPtr<IDXGISwapChain>         m_swapchain;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11Texture2D>        m_depth_tex;
    ComPtr<ID3D11DepthStencilView> m_dsv;

    std::unique_ptr<D3D11Renderer> m_renderer;
    std::unique_ptr<DynamicRouter> m_dynamic_router;
    int m_last_layer_count = -1;
};
