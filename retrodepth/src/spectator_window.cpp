#include "spectator_window.h"
#include <stdexcept>

static void hr_check(HRESULT hr, const char* msg) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(msg) + " hr=" + std::to_string(hr));
}

SpectatorWindow::SpectatorWindow(ID3D11Device* device, ID3D11DeviceContext* ctx)
    : m_device(device), m_ctx(ctx)
{
    create_window();
    create_swapchain();
    m_renderer = std::make_unique<D3D11Renderer>(m_device, m_ctx);
    m_renderer->set_brightness(0.65f);
}

SpectatorWindow::~SpectatorWindow() {
    m_renderer.reset();
    if (m_hwnd) {
        SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK SpectatorWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SpectatorWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CLOSE:
        if (self) {
            self->m_running = false;
            DestroyWindow(hwnd);
            self->m_hwnd = nullptr;
        }
        return 0;
    case WM_DESTROY:
        if (self) self->m_running = false;
        return 0;
    case WM_SIZE:
        if (self && self->m_swapchain) {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) self->resize(w, h);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void SpectatorWindow::create_window() {
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RetroDepthSpectator";
    RegisterClassExA(&wc);

    RECT rc = {0, 0, m_width, m_height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExA(
        0, "RetroDepthSpectator",
        "RetroDepth VR View",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

    SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hwnd, SW_SHOW);
}

void SpectatorWindow::create_swapchain() {
    ComPtr<IDXGIDevice> dxgi_device;
    hr_check(m_device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf())), "Query IDXGIDevice");

    ComPtr<IDXGIAdapter> adapter;
    hr_check(dxgi_device->GetAdapter(adapter.GetAddressOf()), "GetAdapter");

    ComPtr<IDXGIFactory> factory;
    hr_check(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())), "GetFactory");

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr_check(factory->CreateSwapChain(m_device, &sd, m_swapchain.GetAddressOf()), "Create spectator swapchain");
    resize(m_width, m_height);
}

void SpectatorWindow::resize(int w, int h) {
    m_width = w;
    m_height = h;

    m_rtv.Reset();
    m_dsv.Reset();
    m_depth_tex.Reset();

    m_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> back;
    hr_check(m_swapchain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), "Get spectator backbuffer");
    hr_check(m_device->CreateRenderTargetView(back.Get(), nullptr, m_rtv.GetAddressOf()), "Create spectator RTV");

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr_check(m_device->CreateTexture2D(&td, nullptr, m_depth_tex.GetAddressOf()), "Create spectator depth");

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr_check(m_device->CreateDepthStencilView(m_depth_tex.Get(), &dsvd, m_dsv.GetAddressOf()), "Create spectator DSV");
}

void SpectatorWindow::pump_messages() {
    if (!m_hwnd) return;
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void SpectatorWindow::update_layers(const std::vector<LayerFrame>& frames) {
    m_last_frames = frames;
    m_renderer->resize_layers((int)frames.size());
    for (int i = 0; i < (int)frames.size(); ++i)
        m_renderer->update_layer(i, frames[i]);
}

void SpectatorWindow::set_3d_layers_enabled(bool v) {
    if (m_renderer)
        m_renderer->set_3d_layers_enabled(v);
}

void SpectatorWindow::set_depthmap_enabled(bool v) {
    if (m_renderer)
        m_renderer->set_depthmap_enabled(v);
}

void SpectatorWindow::set_depthmap_mirror_enabled(bool v) {
    if (m_renderer)
        m_renderer->set_depthmap_mirror_enabled(v);
}

void SpectatorWindow::set_upscale_enabled(bool v) {
    if (m_renderer)
        m_renderer->set_upscale_enabled(v);
}

void SpectatorWindow::set_shadows_enabled(bool v) {
    if (m_renderer)
        m_renderer->set_shadows_enabled(v);
}

void SpectatorWindow::set_ambilight_enabled(bool v) {
    if (m_renderer)
        m_renderer->set_ambilight_enabled(v);
}

void SpectatorWindow::set_screen_curve(float v) {
    if (m_renderer)
        m_renderer->set_screen_curve(v);
}

void SpectatorWindow::set_gamma(float v) {
    if (m_renderer)
        m_renderer->set_gamma(v);
}

void SpectatorWindow::set_contrast(float v) {
    if (m_renderer)
        m_renderer->set_contrast(v);
}

void SpectatorWindow::set_saturation(float v) {
    if (m_renderer)
        m_renderer->set_saturation(v);
}

void SpectatorWindow::set_rotate_screen(bool v) {
    if (m_renderer)
        m_renderer->set_rotate_screen(v);
}

void SpectatorWindow::set_roundness(float v) {
    if (m_renderer)
        m_renderer->set_roundness(v);
}

void SpectatorWindow::set_background_color(const XMFLOAT4& color) {
    m_bg_color = color;
}

void SpectatorWindow::update_frame(const EyeParams& eye,
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
                                   const std::string& random_overlay_text) {
    if (!m_running || !m_hwnd || !m_renderer) return;
    pump_messages();
    if (!m_running || !m_hwnd || m_last_frames.empty()) return;

    m_renderer->set_editor_state(editor_active, selected_layer, blink_on);
    m_renderer->set_laser_state(laser_origin, laser_tip, laser_on_screen, laser_on_panel,
                                laser_panel_btn, hovered_palette, hovered_group, hovered_layer);
    m_renderer->update_overlay(overlay_text);
    m_renderer->update_preset_overlay(preset_overlay_text);
    m_renderer->update_random_overlay(random_overlay_text);
    render(eye);
}

void SpectatorWindow::render(const EyeParams& eye_in) {
    ID3D11RenderTargetView* rtvs[] = { m_rtv.Get() };
    m_ctx->OMSetRenderTargets(1, rtvs, m_dsv.Get());

    float clear[4] = {m_bg_color.x, m_bg_color.y, m_bg_color.z, 1.0f};
    m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);
    m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    EyeParams eye = eye_in;
    eye.viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_renderer->render_frame(m_last_frames, eye);

    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_swapchain->Present(1, 0);
}
