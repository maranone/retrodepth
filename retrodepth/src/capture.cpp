#include "capture.h"
#include <stdexcept>

WindowCapture::WindowCapture(const std::string& window_title, ID3D11Device* device)
    : m_title(window_title), m_device(device)
{
    m_device->GetImmediateContext(m_ctx.GetAddressOf());
}

bool WindowCapture::find_window() {
    m_hwnd = FindWindowA(nullptr, m_title.c_str());
    if (!m_hwnd) {
        struct Search { const char* needle; HWND result; };
        Search s{ m_title.c_str(), nullptr };
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            char buf[512] = {};
            GetWindowTextA(hwnd, buf, sizeof(buf));
            if (IsWindowVisible(hwnd) && strstr(buf, s->needle)) {
                s->result = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&s));
        m_hwnd = s.result;
    }
    if (m_hwnd) find_window_monitor();
    return m_hwnd != nullptr;
}

void WindowCapture::find_window_monitor() {
    m_monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    // Reset duplication so it re-initialises on next capture (monitor may have changed)
    m_duplication.Reset();
    m_staging.Reset();
}

bool WindowCapture::init_duplication() {
    if (m_duplication) return true;

    // Walk adapters/outputs to find the one hosting our monitor
    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(dxgi_dev.GetAddressOf()))))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(adapter.GetAddressOf());

    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(i, out.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_OUTPUT_DESC desc;
        out->GetDesc(&desc);
        if (desc.Monitor != m_monitor) continue;

        ComPtr<IDXGIOutput1> out1;
        if (FAILED(out.As(&out1))) break;

        if (FAILED(out1->DuplicateOutput(m_device, m_duplication.GetAddressOf())))
            break;

        // Record desktop dimensions for this output
        m_desk_width  = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        m_desk_height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        return true;
    }
    return false;
}

bool WindowCapture::capture() {
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        if (!find_window()) return false;
    }

    if (!init_duplication()) return false;

    // Get the MAME client rect in screen coordinates
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    POINT origin = {0, 0};
    ClientToScreen(m_hwnd, &origin);

    int win_x = origin.x, win_y = origin.y;
    int win_w = client.right  - client.left;
    int win_h = client.bottom - client.top;
    if (win_w <= 0 || win_h <= 0) return false;

    // Acquire latest desktop frame (timeout 0 = non-blocking)
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = m_duplication->AcquireNextFrame(0, &info, resource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame — reuse last pixels if we have them
        return !m_pixels.empty();
    }
    if (FAILED(hr)) {
        // Lost duplication (e.g. monitor change), reset and retry next frame
        m_duplication.Reset();
        m_staging.Reset();
        return false;
    }

    ComPtr<ID3D11Texture2D> desktop_tex;
    resource.As(&desktop_tex);

    // Lazy-create or recreate staging texture
    D3D11_TEXTURE2D_DESC td;
    desktop_tex->GetDesc(&td);
    bool need_staging = !m_staging;
    if (!need_staging) {
        D3D11_TEXTURE2D_DESC sd; m_staging->GetDesc(&sd);
        need_staging = (sd.Width != td.Width || sd.Height != td.Height);
    }
    if (need_staging) {
        m_staging.Reset();
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags      = 0;
        m_device->CreateTexture2D(&td, nullptr, m_staging.GetAddressOf());
    }

    m_ctx->CopyResource(m_staging.Get(), desktop_tex.Get());
    m_duplication->ReleaseFrame();

    // Map and copy the window rect out of the desktop frame
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_ctx->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    // Clamp window rect to desktop bounds
    int x0 = max(0, win_x),        y0 = max(0, win_y);
    int x1 = min((int)td.Width,  win_x + win_w);
    int y1 = min((int)td.Height, win_y + win_h);
    int cw = x1 - x0, ch = y1 - y0;
    if (cw <= 0 || ch <= 0) { m_ctx->Unmap(m_staging.Get(), 0); return false; }

    m_width  = cw;
    m_height = ch;
    m_pixels.resize(cw * ch * 4);

    const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < ch; ++y) {
        memcpy(m_pixels.data() + y * cw * 4,
               src + (y0 + y) * mapped.RowPitch + x0 * 4,
               cw * 4);
    }

    m_ctx->Unmap(m_staging.Get(), 0);
    return true;
}
