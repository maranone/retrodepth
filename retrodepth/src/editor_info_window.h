#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include "game_config.h"
#include "layer_processor.h"
#include "dynamic_router.h"

class LayerEditor;

class EditorInfoWindow {
public:
    EditorInfoWindow(GameConfig& cfg, LayerEditor& editor);
    ~EditorInfoWindow();

    void show_and_sync();
    void hide();
    void on_new_frame(const std::vector<LayerFrame>& frames);
    void set_mame_hwnd(HWND h) { m_mame_hwnd = h; }
    // Called by PreviewWindow whenever the DynamicRouter is created or destroyed.
    void set_router(DynamicRouter* r) { m_router = r; }

    // Called by PreviewWindow each frame with current live palette colours.
    void set_palette_colors(const uint32_t (*palettes)[16]);
    // Called by PreviewWindow each frame with per-palette 32×32 thumbnails.
    void set_palette_thumbs(const uint32_t (*thumbs)[32 * 32]);

private:
    void create_controls();
    void apply_font_to_children();
    void populate_layer_list();
    void populate_detail(int idx);
    void update_copies_display();
    void mark_dirty();

    std::string layer_list_text(int idx) const;
    void refresh_list_entry(int idx);

    void adjust_depth(float delta);
    void adjust_width(float delta);
    void adjust_copy_cnt(int delta);
    void adjust_copy_spc(float delta);

    void on_name_commit();
    void on_depth_commit();
    void on_width_commit();
    void on_copy_cnt_commit();
    void on_copy_spc_commit();

    void on_load_default();
    void on_layer_select();
    void on_spread();
    void on_save();

    // Palette grid
    void create_palette_grid();
    void repaint_palette_grid();
    int  pal_idx_at(int mx, int my) const;
    void assign_palette_group(int pal, int group);
    void draw_palette_cell(HDC hdc, int pal_idx, RECT cell);
    static LRESULT CALLBACK palette_grid_proc(HWND, UINT, WPARAM, LPARAM);

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    GameConfig&  m_cfg;
    LayerEditor& m_editor;
    HWND  m_hwnd          = nullptr;
    HFONT m_font          = nullptr;
    HWND  m_mame_hwnd     = nullptr;
    bool  m_mame_paused   = false;

    // Left panel
    HWND m_list           = nullptr;
    HWND m_btn_spread     = nullptr;

    // Name row
    HWND m_edt_name       = nullptr;

    // Depth row
    HWND m_edt_depth      = nullptr;
    HWND m_btn_dep_dec    = nullptr;
    HWND m_btn_dep_inc    = nullptr;

    // Width row
    HWND m_edt_width      = nullptr;
    HWND m_btn_wid_dec    = nullptr;
    HWND m_btn_wid_inc    = nullptr;

    // Copies row
    HWND m_edt_copy_cnt   = nullptr;
    HWND m_edt_copy_spc   = nullptr;
    HWND m_lbl_copy_eff   = nullptr;

    // Solo checkbox
    HWND m_chk_solo       = nullptr;

    // Dynamic scoring checkboxes
    HWND m_chk_density    = nullptr;
    HWND m_chk_motion     = nullptr;
    DynamicRouter* m_router = nullptr;

    // Palette grid (custom painted child window)
    HWND m_pal_grid       = nullptr;
    uint32_t m_pal_colors[256][16]       = {};  // live palette from MAME
    bool     m_pal_valid                 = false;
    uint32_t m_pal_thumb[256][32 * 32]   = {};  // per-palette 32×32 thumbnails
    bool     m_pal_thumb_valid           = false;
    bool     m_pal_thumb_has_content[256] = {};  // true once MAME has written non-zero data

    // Group legend labels
    HWND m_lbl_grp[4]     = {};

    HWND m_btn_default    = nullptr;
    HWND m_btn_save       = nullptr;

    int  m_cur_sel        = -1;
    bool m_updating       = false;
    bool m_list_dirty     = true;
    bool m_saved          = true;

    // Palette grid geometry (set in create_palette_grid)
    int m_grid_cols       = 16;
    int m_grid_rows       = 16;
    int m_cell_w          = 0;
    int m_cell_h          = 0;

    // Palette grid drag state
    bool m_pal_dragging   = false;
    int  m_drag_group     = 0;    // group being painted during drag
    int  m_drag_last_pal  = -1;   // last palette index painted (avoid redundant work)
};
