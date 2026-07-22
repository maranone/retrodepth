#include "stereo_display_window.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace DirectX;

namespace {

void hr_check(HRESULT hr, const char* msg) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(msg) + " hr=" + std::to_string(hr));
}

}

StereoDisplayWindow::StereoDisplayWindow(GameConfig config)
    : m_config(std::move(config))
{
    m_source = std::make_unique<ShmemReader>();
    create_window();
    create_d3d11();
    create_swapchain();
    m_renderer = std::make_unique<D3D11Renderer>(m_device.Get(), m_ctx.Get());
    apply_renderer_state();
}

StereoDisplayWindow::~StereoDisplayWindow() {
    m_renderer.reset();
    if (m_hwnd) {
        SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void StereoDisplayWindow::set_dynamic_mode(bool v) {
    m_launch_dynamic = v;
    if (v) m_dynamic_router = std::make_unique<DynamicRouter>(m_config);
    else   m_dynamic_router.reset();
    if (m_dynamic_router) {
        m_dynamic_router->set_beta_depth(m_launch_beta_depth);
        m_dynamic_router->set_density_scoring(m_launch_density);
        m_dynamic_router->set_motion_scoring(m_launch_motion);
    }
}

void StereoDisplayWindow::set_beta_depth(bool v) {
    m_launch_beta_depth = v;
    if (m_dynamic_router)
        m_dynamic_router->set_beta_depth(v);
}

void StereoDisplayWindow::set_density_scoring(bool v) {
    m_launch_density = v;
    if (m_dynamic_router)
        m_dynamic_router->set_density_scoring(v);
}

void StereoDisplayWindow::set_motion_scoring(bool v) {
    m_launch_motion = v;
    if (m_dynamic_router)
        m_dynamic_router->set_motion_scoring(v);
}

void StereoDisplayWindow::apply_renderer_state() {
    if (!m_renderer)
        return;
    m_renderer->set_brightness(0.85f);
    m_renderer->set_gamma(1.15f);
    m_renderer->set_contrast(0.90f);
    m_renderer->set_saturation(0.80f);
}

LRESULT CALLBACK StereoDisplayWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<StereoDisplayWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_DESTROY:
        if (self) self->m_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (self && self->m_swapchain) {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) self->resize(w, h);
        }
        return 0;
    case WM_KEYDOWN:
        if (!self) return 0;
        if (wp == VK_ESCAPE) { self->m_running = false; return 0; }
        if (self->m_mame_hwnd)
            PostMessageA(self->m_mame_hwnd, WM_KEYDOWN, wp, lp);
        return 0;
    case WM_KEYUP:
        if (self && self->m_mame_hwnd)
            PostMessageA(self->m_mame_hwnd, WM_KEYUP, wp, lp);
        return 0;
    case WM_LBUTTONDOWN:
        if (self) {
            self->m_dragging = true;
            self->m_last_mx  = (int)(short)LOWORD(lp);
            self->m_last_my  = (int)(short)HIWORD(lp);
            SetCapture(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (self) { self->m_dragging = false; ReleaseCapture(); }
        return 0;
    case WM_MOUSEMOVE:
        if (self && self->m_dragging) {
            int mx = (int)(short)LOWORD(lp);
            int my = (int)(short)HIWORD(lp);
            self->m_yaw   += (mx - self->m_last_mx) * 0.005f;
            self->m_pitch += (my - self->m_last_my) * 0.005f;
            if (self->m_pitch >  1.4f) self->m_pitch =  1.4f;
            if (self->m_pitch < -1.4f) self->m_pitch = -1.4f;
            self->m_last_mx = mx;
            self->m_last_my = my;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (self) {
            float factor = ((short)HIWORD(wp) > 0) ? 0.9f : 1.1f;
            self->m_distance *= factor;
            if (self->m_distance < 0.1f)  self->m_distance = 0.1f;
            if (self->m_distance > 50.0f) self->m_distance = 50.0f;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void StereoDisplayWindow::create_window() {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RetroDepthSBS";
    RegisterClassExA(&wc);

    POINT origin = {0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(monitor, &mi)) {
        m_width = mi.rcMonitor.right - mi.rcMonitor.left;
        m_height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }

    m_hwnd = CreateWindowExA(0, "RetroDepthSBS",
        "RetroDepth Side-by-Side 3D - ESC to exit",
        WS_POPUP | WS_VISIBLE,
        mi.rcMonitor.left, mi.rcMonitor.top,
        m_width, m_height,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

    SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
}

void StereoDisplayWindow::create_d3d11() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    hr_check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               0, &fl, 1, D3D11_SDK_VERSION,
                               m_device.GetAddressOf(), nullptr,
                               m_ctx.GetAddressOf()), "D3D11CreateDevice");
}

void StereoDisplayWindow::create_swapchain() {
    ComPtr<IDXGIFactory> factory;
    hr_check(CreateDXGIFactory(IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory");

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

    hr_check(factory->CreateSwapChain(m_device.Get(), &sd, m_swapchain.GetAddressOf()),
             "Create SBS swapchain");
    resize(m_width, m_height);
}

void StereoDisplayWindow::resize(int w, int h) {
    m_width = w;
    m_height = h;

    m_rtv.Reset();
    m_dsv.Reset();
    m_depth_tex.Reset();

    m_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> back;
    hr_check(m_swapchain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), "Get SBS backbuffer");
    hr_check(m_device->CreateRenderTargetView(back.Get(), nullptr, m_rtv.GetAddressOf()), "Create SBS RTV");

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr_check(m_device->CreateTexture2D(&td, nullptr, m_depth_tex.GetAddressOf()), "Create SBS depth");

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr_check(m_device->CreateDepthStencilView(m_depth_tex.Get(), &dsvd, m_dsv.GetAddressOf()), "Create SBS DSV");
}

float StereoDisplayWindow::convergence_depth() const {
    if (!m_last_frames.empty()) {
        float min_d = m_last_frames[0].depth_meters;
        float max_d = m_last_frames[0].depth_meters;
        for (const auto& frame : m_last_frames) {
            min_d = (std::min)(min_d, frame.depth_meters);
            max_d = (std::max)(max_d, frame.depth_meters);
        }
        return (min_d + max_d) * 0.5f;
    }
    if (!m_config.layers.empty()) {
        float min_d = m_config.layers[0].depth_meters;
        float max_d = m_config.layers[0].depth_meters;
        for (const auto& layer : m_config.layers) {
            min_d = (std::min)(min_d, layer.depth_meters);
            max_d = (std::max)(max_d, layer.depth_meters);
        }
        return (min_d + max_d) * 0.5f;
    }
    return 2.5f;
}

void StereoDisplayWindow::run() {
    bool printed_connected = false;
    bool printed_frames = false;
    const DWORD start_tick = GetTickCount();

    while (m_running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!m_running) break;
        if (m_auto_exit_ms > 0 && (GetTickCount() - start_tick) >= m_auto_exit_ms) {
            m_running = false;
            break;
        }
        if (!printed_connected && m_source->is_connected()) {
            std::cout << "[SBS] MAME shared memory connected.\n";
            printed_connected = true;
        }
        if (!printed_frames && !m_last_frames.empty()) {
            std::cout << "[SBS] First frame received (" << m_last_frames.size() << " layers).\n";
            printed_frames = true;
        }
        render_frame();
    }
}

void StereoDisplayWindow::render_frame() {
    auto new_frames = m_source->poll(m_config);
    if (!new_frames.empty()) {
        if ((int)new_frames.size() != m_last_layer_count) {
            m_renderer->resize_layers((int)new_frames.size());
            m_last_layer_count = (int)new_frames.size();
        }
        for (int i = 0; i < (int)new_frames.size(); ++i)
            m_renderer->update_layer(i, new_frames[i]);
        m_last_frames = std::move(new_frames);
    }
    if (m_dynamic_router && !m_last_frames.empty())
        m_dynamic_router->on_frame(m_last_frames);

    ID3D11RenderTargetView* rtvs[] = { m_rtv.Get() };
    m_ctx->OMSetRenderTargets(1, rtvs, m_dsv.Get());
    float clear[4] = {m_bg_color.x, m_bg_color.y, m_bg_color.z, m_bg_color.w};
    m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);
    m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    if (!m_last_frames.empty()) {
        const float conv = convergence_depth();
        const float eye_y = m_config.quad_y_meters;
        const float half_ipd = m_ipd_meters * 0.5f;
        const int eye_px_w = (std::max)(1, m_width / 2);
        const int render_px_w = eye_px_w * 2;
        const int margin_px_x = (std::max)(0, (m_width - render_px_w) / 2);
        const float half_w = (float)eye_px_w;
        const float aspect = half_w / (float)(std::max)(1, m_height);

        // Init orbit distance to convergence depth on first frame
        if (!m_dist_init) { m_distance = conv; m_dist_init = true; }

        const XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR anchor = XMVectorSet(0.0f, eye_y, -conv, 0.0f);
        const XMVECTOR dir    = XMVectorSet(
            sinf(m_yaw) * cosf(m_pitch),
            sinf(m_pitch),
            cosf(m_yaw) * cosf(m_pitch),
            0.0f);
        const XMVECTOR center_eye = XMVectorAdd(anchor, XMVectorScale(dir, m_distance));
        const XMVECTOR fwd  = XMVector3Normalize(XMVectorSubtract(anchor, center_eye));
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(fwd, up));

        for (int eye = 0; eye < 2; ++eye) {
            const float side    = (eye == 0) ? -half_ipd : half_ipd;
            XMVECTOR eye_pos = XMVectorAdd(center_eye, XMVectorScale(right, side));
            XMVECTOR target  = XMVectorAdd(anchor,     XMVectorScale(right, side));

            EyeParams ep;
            ep.view = XMMatrixLookAtRH(eye_pos, target, up);
            ep.proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(80.0f), aspect, 0.05f, 100.0f);
            ep.quad_y_meters = eye_y;
            ep.viewport = {
                (float)(margin_px_x + (eye == 0 ? 0 : eye_px_w)),
                0.0f,
                half_w,
                (float)m_height,
                0.0f,
                1.0f
            };
            m_renderer->render_frame(m_last_frames, ep);
        }
    }

    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_swapchain->Present(1, 0);
}
