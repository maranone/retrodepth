#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// Captures a target window using DXGI Desktop Duplication.
// Works with any GPU-rendered window (bgfx, d3d, opengl, etc.).
// Output format: BGRA, 4 bytes per pixel, top-down.
class WindowCapture {
public:
    // device: the D3D11 device already created by the renderer/XR session.
    WindowCapture(const std::string& window_title, ID3D11Device* device);
    ~WindowCapture() = default;

    bool find_window();
    bool capture(); // returns true if a new frame was captured

    const uint8_t* pixels() const { return m_pixels.data(); }
    int width()  const { return m_width; }
    int height() const { return m_height; }
    bool valid() const { return m_hwnd != nullptr; }

private:
    bool init_duplication();
    void find_window_monitor();

    std::string  m_title;
    HWND         m_hwnd    = nullptr;
    HMONITOR     m_monitor = nullptr;

    ID3D11Device*               m_device = nullptr; // non-owning
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D>        m_staging;

    int m_width  = 0;
    int m_height = 0;
    int m_desk_width  = 0;
    int m_desk_height = 0;
    int m_win_x = 0, m_win_y = 0; // window top-left on desktop
    std::vector<uint8_t> m_pixels;
};
