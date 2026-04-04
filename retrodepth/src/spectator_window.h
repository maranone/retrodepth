#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <string>
#include "d3d11_renderer.h"

using Microsoft::WRL::ComPtr;

class SpectatorWindow {
public:
    SpectatorWindow(ID3D11Device* device, ID3D11DeviceContext* ctx);
    ~SpectatorWindow();

    bool is_valid() const { return m_hwnd != nullptr && m_swapchain != nullptr; }
    bool is_open() const { return m_running; }
    void pump_messages();

    void set_3d_layers_enabled(bool v);
    void set_depthmap_enabled(bool v);
    void set_depthmap_mirror_enabled(bool v);
    void set_upscale_enabled(bool v);
    void set_shadows_enabled(bool v);
    void set_ambilight_enabled(bool v);
    void set_screen_curve(float v);
    void set_gamma(float v);
    void set_contrast(float v);
    void set_saturation(float v);
    void set_rotate_screen(bool v);
    void set_roundness(float v);
    void set_background_color(const XMFLOAT4& color);
    void update_layers(const std::vector<LayerFrame>& frames);
    void update_frame(const EyeParams& eye,
                      bool editor_active,
                      int selected_layer,
                      bool blink_on,
                      const XMFLOAT3 laser_origin[2],
                      const XMFLOAT3 laser_tip[2],
                      const bool laser_on_screen[2],
                      const bool laser_on_panel[2],
                      const int laser_panel_btn[2],
                      int hovered_palette,
                      int hovered_group,
                      int hovered_layer,
                      const std::string& overlay_text,
                      const std::string& preset_overlay_text,
                      const std::string& random_overlay_text);

private:
    void create_window();
    void create_swapchain();
    void resize(int w, int h);
    void render(const EyeParams& eye);

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_ctx    = nullptr;

    HWND  m_hwnd    = nullptr;
    bool  m_running = true;
    int   m_width   = 1280;
    int   m_height  = 720;

    ComPtr<IDXGISwapChain>         m_swapchain;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11Texture2D>        m_depth_tex;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    std::unique_ptr<D3D11Renderer> m_renderer;

    std::vector<LayerFrame> m_last_frames;
    XMFLOAT4 m_bg_color = {0.02f, 0.02f, 0.05f, 1.0f};
};
