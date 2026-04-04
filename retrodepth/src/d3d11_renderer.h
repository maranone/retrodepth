#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdint>
#include "layer_processor.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct EyeParams {
    XMMATRIX view;
    XMMATRIX proj;
    D3D11_VIEWPORT viewport;
    float quad_y_meters = 1.6f; // vertical center of quads in STAGE space (floor=0)
    XMMATRIX scene_world = XMMatrixIdentity();
};

// One GPU texture + SRV per game layer
struct LayerTexture {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int width  = 0;
    int height = 0;
};

class D3D11Renderer {
public:
    static constexpr int   k_max_depth_copies   = 20;
    static constexpr float k_default_copy_step_m = 0.003f;

    // device and context are owned externally (by the OpenXR session)
    D3D11Renderer(ID3D11Device* device, ID3D11DeviceContext* ctx);

    // Upload CPU-side BGRA frame data for a layer.
    // Recreates the texture if dimensions changed.
    void update_layer(int idx, const LayerFrame& frame);

    // Ensure we have exactly n layer slots allocated.
    void resize_layers(int n);

    // Render all layers (back-to-front) into the currently bound render target.
    // eye_params provides view/projection matrices and the viewport.
    void render_frame(const std::vector<LayerFrame>& frames,
                      const EyeParams& eye);

    // Editor state — call once per frame before render_frame().
    void set_editor_state(bool active, int selected_layer, bool blink_on);

    // Brightness multiplier applied to all layers (1.0 = full, 0.7 = darker).
    void set_brightness(float b)  { m_brightness  = b; }
    // Color grading applied to game layers only (not UI overlay).
    void set_contrast(float v)    { m_contrast    = v; }
    void set_saturation(float v)  { m_saturation  = v; }
    void set_gamma(float v)       { m_gamma       = v; }
    void set_rotate_screen(bool v){ m_rotate_screen = v; }
    void set_3d_layers_enabled(bool v) { m_3d_layers_enabled = v; }
    void set_depthmap_enabled(bool v) { m_depthmap_enabled = v; }
    void set_depthmap_mirror_enabled(bool v) { m_depthmap_mirror_enabled = v; }
    void set_upscale_enabled(bool v) { m_upscale_enabled = v; }
    void set_shadows_enabled(bool v)  { m_shadows_enabled = v; }
    void set_ambilight_enabled(bool v) { m_ambilight_enabled = v; }
    void set_screen_curve(float v) { m_screen_curve = v; }
    // Rounded (sphere/cone) effect on depth copies: 0=flat, +1=max sphere, -1=max cone.
    void set_roundness(float v)   { m_roundness    = v; }

    // Color palette panel — 4 cols × 8 rows, dark→light top→bottom
    static constexpr int k_colorpal_cols = 1;
    static constexpr int k_colorpal_rows = 8;
    XMFLOAT4 get_palette_color(int idx) const;

    // Controller poses — call each frame before render_frame().
    // Renders a small indicator quad at each hand position.
    void set_controller_transform(int hand, bool valid, const DirectX::XMMATRIX& world) {
        m_ctrl_valid[hand] = valid;
        m_ctrl_world[hand] = world;
    }

    // Laser pointer state — call each frame before render_frame().
    void set_laser_state(const DirectX::XMFLOAT3 origin[2],
                         const DirectX::XMFLOAT3 tip[2],
                         const bool on_screen[2],
                         const bool on_panel[2],
                         const int  panel_btn[2],
                         int  hovered_palette,
                         int  hovered_group,
                         int  hovered_layer = -1) {
        for (int h = 0; h < 2; ++h) {
            m_laser_origin[h]    = origin[h];
            m_laser_tip[h]       = tip[h];
            m_laser_on_screen[h] = on_screen[h];
            m_laser_on_panel[h]  = on_panel[h];
            m_laser_panel_btn[h] = panel_btn[h];
        }
        m_hovered_palette = hovered_palette;
        m_hovered_group   = hovered_group;
        m_hovered_layer   = hovered_layer;
    }

    void update_hover_mask(int layer_idx, int width, int height, const uint8_t* alpha_mask);
    void clear_hover_mask();

    // Re-render the overlay texture from text. Call when editor is active.
    void update_overlay(const std::string& text);
    void update_preset_overlay(const std::string& text);
    void update_random_overlay(const std::string& text);

    ID3D11Device*        device()  const { return m_device; }
    ID3D11DeviceContext* context() const { return m_ctx; }

private:
    void init_pipeline();
    void init_flat_pipeline();
    void init_colorpal_texture();
    void update_text_texture(ID3D11Texture2D* tex, int tex_w, int tex_h,
                             const std::string& text, int font_scale);
    void ensure_layer_texture(int idx, int w, int h);
    void ensure_hover_texture(int w, int h);
    void draw_extruded_layer_walls(const std::vector<LayerFrame>& frames, const EyeParams& eye);
    void draw_floor_shadows(const std::vector<LayerFrame>& frames, const EyeParams& eye);
    void draw_ambilight(const std::vector<LayerFrame>& frames, const EyeParams& eye);
    void draw_laser_beams(const EyeParams& eye);

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_ctx    = nullptr;

    ComPtr<ID3D11VertexShader>  m_vs;
    ComPtr<ID3D11PixelShader>   m_ps;
    ComPtr<ID3D11InputLayout>   m_input_layout;
    ComPtr<ID3D11Buffer>        m_vb;
    ComPtr<ID3D11Buffer>        m_ib;
    ComPtr<ID3D11Buffer>        m_curve_vb;
    ComPtr<ID3D11Buffer>        m_curve_ib;
    ComPtr<ID3D11Buffer>        m_cb;         // per-draw constant buffer
    ComPtr<ID3D11SamplerState>  m_sampler;
    ComPtr<ID3D11SamplerState>  m_sampler_linear;
    ComPtr<ID3D11BlendState>    m_blend;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11DepthStencilState> m_depth_state;
    ComPtr<ID3D11DepthStencilState> m_depth_state_slice; // no write, always pass

    // Flat-color pipeline (for laser beams and reticles)
    ComPtr<ID3D11VertexShader>  m_flat_vs;
    ComPtr<ID3D11PixelShader>   m_flat_ps;
    ComPtr<ID3D11InputLayout>   m_flat_layout;
    ComPtr<ID3D11Buffer>        m_flat_vb;  // dynamic, up to 16 verts per draw
    ComPtr<ID3D11Buffer>        m_flat_cb;  // FlatCB at register b1

    std::vector<LayerTexture>   m_layers;

    // Editor overlay
    bool  m_editor_active   = false;
    int   m_editor_selected = 0;
    bool  m_editor_blink_on = true;
    float m_brightness      = 1.0f;
    float m_contrast        = 1.0f;
    float m_saturation      = 1.0f;
    float m_gamma           = 1.0f;
    bool  m_rotate_screen   = false;
    bool  m_has_preset_overlay = false;
    bool  m_has_random_overlay = false;
    bool  m_3d_layers_enabled = false;
    bool  m_depthmap_enabled = false;
    bool  m_depthmap_mirror_enabled = false;
    bool  m_upscale_enabled = false;
    bool  m_shadows_enabled = false;
    bool  m_ambilight_enabled = false;
    float m_screen_curve    = 0.0f;
    float m_roundness       = 0.0f;
    ComPtr<ID3D11Texture2D>          m_colorpal_tex;
    ComPtr<ID3D11ShaderResourceView> m_colorpal_srv;
    bool  m_ctrl_valid[2]   = {};
    DirectX::XMMATRIX m_ctrl_world[2] = {};
    ComPtr<ID3D11Texture2D>          m_overlay_tex;
    ComPtr<ID3D11ShaderResourceView> m_overlay_srv;
    ComPtr<ID3D11Texture2D>          m_preset_overlay_tex;
    ComPtr<ID3D11ShaderResourceView> m_preset_overlay_srv;
    ComPtr<ID3D11Texture2D>          m_random_overlay_tex;
    ComPtr<ID3D11ShaderResourceView> m_random_overlay_srv;
    ComPtr<ID3D11Texture2D>          m_ctrl_tex[2];
    ComPtr<ID3D11ShaderResourceView> m_ctrl_srv[2];
    ComPtr<ID3D11Texture2D>          m_hover_tex;
    ComPtr<ID3D11ShaderResourceView> m_hover_srv;
    ComPtr<ID3D11Buffer>             m_face_vb; // dynamic 4-vertex buffer for box faces

    // Laser pointer state (set by XrApp each frame)
    DirectX::XMFLOAT3 m_laser_origin[2]    = {};
    DirectX::XMFLOAT3 m_laser_tip[2]       = {};
    bool              m_laser_on_screen[2] = {};
    bool              m_laser_on_panel[2]  = {};
    int               m_laser_panel_btn[2] = {-1, -1};
    int               m_hovered_palette    = -1;
    int               m_hovered_group      = 0;
    int               m_hovered_layer      = -1;
    int               m_hover_mask_layer   = -1;
    int               m_hover_mask_width   = 0;
    int               m_hover_mask_height  = 0;

    static constexpr int k_overlay_w = 1536;
    static constexpr int k_overlay_h = 2304;
    static constexpr int k_preset_overlay_w = 2560;
    static constexpr int k_preset_overlay_h = 384;
    static constexpr int k_random_overlay_w = 1536;
    static constexpr int k_random_overlay_h = 1536;
    static constexpr UINT k_curve_segments = 32;
    UINT m_curve_index_count = 0;
};
