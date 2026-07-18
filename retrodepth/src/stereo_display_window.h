#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include "game_config.h"
#include "shmem_reader.h"
#include "d3d11_renderer.h"
#include "dynamic_router.h"

using Microsoft::WRL::ComPtr;

// Desktop side-by-side stereo output for 3D displays.
class StereoDisplayWindow {
public:
    explicit StereoDisplayWindow(GameConfig config);
    ~StereoDisplayWindow();

    void run();
    void set_auto_exit_ms(uint32_t v) { m_auto_exit_ms = v; }
    void set_mame_hwnd(HWND h) { m_mame_hwnd = h; }
    void set_dynamic_mode(bool v);
    void set_beta_depth(bool v);
    void set_density_scoring(bool v);
    void set_motion_scoring(bool v);

private:
    void create_window();
    void create_d3d11();
    void create_swapchain();
    void resize(int w, int h);
    void render_frame();
    void apply_renderer_state();
    float convergence_depth() const;

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    GameConfig m_config;
    ShmemReader m_shmem;
    std::vector<LayerFrame> m_last_frames;
    HWND m_mame_hwnd = nullptr;
    HWND m_hwnd = nullptr;
    bool m_running = true;
    int m_width = 1920;
    int m_height = 1080;
    int m_last_layer_count = -1;
    uint32_t m_auto_exit_ms = 0;

    bool m_launch_dynamic = false;
    bool m_launch_beta_depth = false;
    bool m_launch_density = false;
    bool m_launch_motion = false;
    std::unique_ptr<DynamicRouter> m_dynamic_router;

    float m_ipd_meters = 0.064f;
    DirectX::XMFLOAT4 m_bg_color = {0.02f, 0.02f, 0.05f, 1.0f};

    // Orbit camera (left-drag to rotate, scroll wheel to zoom)
    float m_yaw      = 0.0f;
    float m_pitch    = 0.0f;
    float m_distance = 0.0f;   // 0 = use convergence depth on first frame
    bool  m_dist_init = false;
    bool  m_dragging  = false;
    int   m_last_mx   = 0;
    int   m_last_my   = 0;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain> m_swapchain;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11Texture2D> m_depth_tex;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    std::unique_ptr<D3D11Renderer> m_renderer;
};
