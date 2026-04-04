#include "editor_info_window.h"
#include "layer_editor.h"
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <string>
#include <iostream>
#pragma comment(lib, "comctl32.lib")

static const char* k_cls      = "RDEditorInfo6";
static const char* k_grid_cls = "RDPaletteGrid";

// Client area — palette grid + two buttons only
static const int CW = 500;
static const int CH = 516;

// Grid geometry constants
static const int GRID_X    = 10;
static const int GRID_Y    = 32;
static const int GRID_W    = CW - 20;   // 480
static const int CELL_W    = GRID_W / 16; // 30
static const int CELL_H    = 26;
static const int GRID_H    = 16 * CELL_H; // 416

// Custom messages
static const UINT WM_WHEEL_ADJUST = WM_APP + 1;
static const UINT WM_ENTER_COMMIT = WM_APP + 2;
// Sent by palette grid to parent when a cell was clicked: wParam=palette_idx
static const UINT WM_PALETTE_CLICK = WM_APP + 3;

static bool is_snes_cart_rom(const GameConfig& cfg) {
    static const std::string k_prefix = "snes -cart \"";
    return cfg.rom_name.size() >= k_prefix.size() &&
           cfg.rom_name.compare(0, k_prefix.size(), k_prefix) == 0;
}

static bool is_genesis_cart_rom(const GameConfig& cfg) {
    static const std::string k_prefix = "genesis -cart \"";
    return cfg.rom_name.size() >= k_prefix.size() &&
           cfg.rom_name.compare(0, k_prefix.size(), k_prefix) == 0;
}

static bool is_gb_cart_rom(const GameConfig& cfg) {
    static const std::string k_prefix = "gameboy -cart \"";
    return cfg.rom_name.size() >= k_prefix.size() &&
           cfg.rom_name.compare(0, k_prefix.size(), k_prefix) == 0;
}

static bool is_gbc_cart_rom(const GameConfig& cfg) {
    static const std::string k_prefix = "gbcolor -cart \"";
    return cfg.rom_name.size() >= k_prefix.size() &&
           cfg.rom_name.compare(0, k_prefix.size(), k_prefix) == 0;
}

static bool use_simple_layer_editor(const GameConfig& cfg) {
    return is_snes_cart_rom(cfg)
        || is_genesis_cart_rom(cfg)
        || is_gb_cart_rom(cfg)
        || is_gbc_cart_rom(cfg);
}

static const char* editor_base_title(const GameConfig& cfg) {
    return use_simple_layer_editor(cfg)
        ? "RetroDepth Layer Editor"
        : "RetroDepth Palette Editor";
}

// Group colours (BGR for COLORREF): gray, blue, green, red
static const COLORREF k_grp_colors[4] = {
    RGB(120,120,120), // grp0 — far (gray)
    RGB( 60,120,220), // grp1 — blue
    RGB( 60,200, 80), // grp2 — green
    RGB(220, 60, 60), // grp3 — near (red)
};
static const char* k_grp_labels[4] = { "grp0 (far)", "grp1", "grp2", "grp3 (near)" };

enum {
    IDC_LIST           = 200,
    IDC_BTN_SPREAD     = 201,
    IDC_EDT_DEPTH      = 202,
    IDC_BTN_DEP_DEC    = 203,
    IDC_BTN_DEP_INC    = 204,
    IDC_EDT_WIDTH      = 205,
    IDC_BTN_WID_DEC    = 206,
    IDC_BTN_WID_INC    = 207,
    IDC_EDT_NAME       = 208,
    IDC_EDT_COPY_CNT   = 209,
    IDC_EDT_COPY_SPC   = 210,
    IDC_CHK_SOLO       = 215,
    IDC_BTN_DEFAULT    = 218,
    IDC_BTN_SAVE       = 219,
    IDC_CHK_DENSITY    = 220,
    IDC_CHK_MOTION     = 221,
};

// ---------------------------------------------------------------------------
static LRESULT CALLBACK numeric_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetParent(hwnd), WM_ENTER_COMMIT,
            (WPARAM)(UINT_PTR)GetDlgCtrlID(hwnd), (LPARAM)hwnd);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        PostMessageA(GetParent(hwnd), WM_WHEEL_ADJUST,
            (WPARAM)(UINT_PTR)GetDlgCtrlID(hwnd), delta > 0 ? 1 : -1);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
static int copies_count(const LayerConfig& lc)    { return (int)lc.copies.size(); }
static float copies_spacing(const LayerConfig& lc){ return lc.copies.empty() ? 0.20f : lc.copies[0]; }

static void regen_copies(LayerConfig& lc, int count, float spacing) {
    count   = std::max(0, std::min(count, 16));
    spacing = std::max(0.005f, spacing);
    lc.copies.resize(count);
    for (int i = 0; i < count; ++i)
        lc.copies[i] = (i + 1) * spacing;
}

// ---------------------------------------------------------------------------
// Palette grid window proc
// ---------------------------------------------------------------------------
LRESULT CALLBACK EditorInfoWindow::palette_grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<EditorInfoWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (self) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int cw = self->m_cell_w, ch = self->m_cell_h;
            for (int p = 0; p < 256; ++p) {
                int col = p % self->m_grid_cols;
                int row = p / self->m_grid_cols;
                RECT cell = { col * cw, row * ch, (col+1)*cw, (row+1)*ch };
                self->draw_palette_cell(hdc, p, cell);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!self) return 0;
        int mx = LOWORD(lp), my = HIWORD(lp);
        int pal = self->pal_idx_at(mx, my);
        if (pal < 0) return 0;
        // Cycle the clicked cell to determine the drag group
        int next = (self->m_editor.get_palette_group(pal) + 1) % 4;
        self->m_drag_group    = next;
        self->m_drag_last_pal = pal;
        self->m_pal_dragging  = true;
        self->assign_palette_group(pal, next);
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!self || !self->m_pal_dragging) return 0;
        int mx = LOWORD(lp), my = HIWORD(lp);
        int pal = self->pal_idx_at(mx, my);
        if (pal >= 0 && pal != self->m_drag_last_pal) {
            self->m_drag_last_pal = pal;
            self->assign_palette_group(pal, self->m_drag_group);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self) {
            self->m_pal_dragging  = false;
            self->m_drag_last_pal = -1;
        }
        ReleaseCapture();
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void EditorInfoWindow::draw_palette_cell(HDC hdc, int pal_idx, RECT cell)
{
    int cw = cell.right  - cell.left;
    int ch = cell.bottom - cell.top;

    // Group color border around the whole cell.
    int grp = m_editor.get_palette_group(pal_idx);
    HPEN grp_pen = CreatePen(PS_SOLID, 3, k_grp_colors[grp]);
    HGDIOBJ old_pen = SelectObject(hdc, grp_pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, cell.left, cell.top, cell.right, cell.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(grp_pen);

    int inner_x = cell.left + 3;
    int inner_w = cw - 6;
    RECT inner_cell = { inner_x, cell.top + 3, cell.right - 3, cell.bottom - 3 };
    int inner_h = inner_cell.bottom - inner_cell.top;

    if (m_pal_thumb_has_content[pal_idx]) {
        // Render 32×32 thumbnail bitmap
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = 32;
        bmi.bmiHeader.biHeight      = -32; // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(hdc, inner_cell.left, inner_cell.top, inner_w, inner_h,
                      0, 0, 32, 32,
                      m_pal_thumb[pal_idx], &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        // Fallback: 4×4 grid of all 16 palette colors
        int sw = inner_w / 4;
        int sh = inner_h / 4;
        for (int ci = 0; ci < 16; ++ci) {
            int sc = ci % 4, sr = ci / 4;
            uint32_t argb = m_pal_valid ? m_pal_colors[pal_idx][ci] : 0;
            COLORREF cr = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
            HBRUSH br = CreateSolidBrush(cr);
            RECT sw_rc = { inner_cell.left + sc * sw,       inner_cell.top + sr * sh,
                           inner_cell.left + (sc + 1) * sw, inner_cell.top + (sr + 1) * sh };
            FillRect(hdc, &sw_rc, br);
            DeleteObject(br);
        }
    }

    HPEN gray_pen = CreatePen(PS_SOLID, 1, RGB(24, 24, 24));
    old_pen = (HPEN)SelectObject(hdc, gray_pen);
    old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, inner_cell.left, inner_cell.top, inner_cell.right, inner_cell.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(gray_pen);
}

int EditorInfoWindow::pal_idx_at(int mx, int my) const
{
    if (m_cell_w == 0 || m_cell_h == 0) return -1;
    int col = mx / m_cell_w;
    int row = my / m_cell_h;
    if (col < 0 || col >= m_grid_cols || row < 0 || row >= m_grid_rows) return -1;
    int pal = row * m_grid_cols + col;
    return (pal >= 0 && pal <= 255) ? pal : -1;
}

void EditorInfoWindow::assign_palette_group(int pal, int group)
{
    m_editor.set_palette_group(pal, group);
    mark_dirty();
    if (m_pal_grid) {
        int col = pal % m_grid_cols;
        int row = pal / m_grid_cols;
        RECT cell = { col * m_cell_w, row * m_cell_h,
                      (col+1)*m_cell_w, (row+1)*m_cell_h };
        InvalidateRect(m_pal_grid, &cell, FALSE);
    }
}

void EditorInfoWindow::create_palette_grid()
{
    HINSTANCE hi = GetModuleHandleA(nullptr);

    // Register grid window class once
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXA wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = palette_grid_proc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = k_grid_cls;
        RegisterClassExA(&wc);
        registered = true;
    }

    m_grid_cols = 16;
    m_grid_rows = 16;
    m_cell_w    = CELL_W;
    m_cell_h    = CELL_H;

    m_pal_grid = CreateWindowExA(WS_EX_CLIENTEDGE, k_grid_cls, "",
        WS_CHILD | WS_VISIBLE,
        GRID_X, GRID_Y, GRID_W, GRID_H,
        m_hwnd, nullptr, hi, nullptr);

    SetWindowLongPtrA(m_pal_grid, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Group legend labels below the grid
    const int leg_y = GRID_Y + GRID_H + 4;
    const int leg_w = GRID_W / 4;
    for (int g = 0; g < 4; ++g) {
        m_lbl_grp[g] = CreateWindowExA(0, "STATIC", k_grp_labels[g],
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            GRID_X + g * leg_w, leg_y, leg_w, 16,
            m_hwnd, nullptr, hi, nullptr);
    }
}

void EditorInfoWindow::repaint_palette_grid()
{
    if (m_pal_grid)
        InvalidateRect(m_pal_grid, nullptr, FALSE);
}

void EditorInfoWindow::set_palette_colors(const uint32_t (*palettes)[16])
{
    if (!palettes) return;
    memcpy(m_pal_colors, palettes, sizeof(m_pal_colors));
    m_pal_valid = true;
    repaint_palette_grid();
}

void EditorInfoWindow::set_palette_thumbs(const uint32_t (*thumbs)[32 * 32])
{
    if (!thumbs) return;
    for (int p = 0; p < 256; ++p) {
        bool has = false;
        for (int i = 0; i < 32 * 32 && !has; ++i)
            if (thumbs[p][i]) has = true;
        if (has) {
            memcpy(m_pal_thumb[p], thumbs[p], 32 * 32 * sizeof(uint32_t));
            m_pal_thumb_has_content[p] = true;
            m_pal_thumb_valid = true;
        }
    }
    repaint_palette_grid();
}

// ---------------------------------------------------------------------------
EditorInfoWindow::EditorInfoWindow(GameConfig& cfg, LayerEditor& editor)
    : m_cfg(cfg), m_editor(editor)
{
    INITCOMMONCONTROLSEX icce = { sizeof(icce), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icce);

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = k_cls;
    RegisterClassExA(&wc);

    RECT rc = { 0, 0, CW, CH };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, FALSE);

    m_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        k_cls, editor_base_title(m_cfg),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
        560, 60,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

    if (!m_hwnd) return;
    SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    create_controls();
    apply_font_to_children();
}

EditorInfoWindow::~EditorInfoWindow() {
    if (m_edt_depth)    RemoveWindowSubclass(m_edt_depth,    numeric_edit_proc, 0);
    if (m_edt_width)    RemoveWindowSubclass(m_edt_width,    numeric_edit_proc, 0);
    if (m_edt_copy_cnt) RemoveWindowSubclass(m_edt_copy_cnt, numeric_edit_proc, 0);
    if (m_edt_copy_spc) RemoveWindowSubclass(m_edt_copy_spc, numeric_edit_proc, 0);

    if (m_pal_grid)
        SetWindowLongPtrA(m_pal_grid, GWLP_USERDATA, 0);

    if (m_hwnd) {
        SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    m_list = nullptr;
    m_btn_spread = nullptr;
    m_edt_name = nullptr;
    m_edt_depth = nullptr;
    m_btn_dep_dec = nullptr;
    m_btn_dep_inc = nullptr;
    m_edt_width = nullptr;
    m_btn_wid_dec = nullptr;
    m_btn_wid_inc = nullptr;
    m_edt_copy_cnt = nullptr;
    m_edt_copy_spc = nullptr;
    m_lbl_copy_eff = nullptr;
    m_chk_solo = nullptr;
    m_chk_density = nullptr;
    m_chk_motion = nullptr;
    m_pal_grid = nullptr;
    for (HWND& h : m_lbl_grp)
        h = nullptr;
    m_btn_default = nullptr;
    m_btn_save = nullptr;

    if (m_font) { DeleteObject(m_font);  m_font = nullptr; }
}

// ---------------------------------------------------------------------------
void EditorInfoWindow::create_controls() {
    HINSTANCE hi = GetModuleHandleA(nullptr);
    if (!use_simple_layer_editor(m_cfg)) {
        // Instruction label
        CreateWindowExA(0, "STATIC", "Palette -> Group  (click + drag to paint):",
            WS_CHILD | WS_VISIBLE,
            GRID_X, 10, GRID_W, 18,
            m_hwnd, nullptr, hi, nullptr);

        // Palette grid + group legend
        create_palette_grid();

        // Bottom buttons
        m_btn_default = CreateWindowExA(0, "BUTTON", "Load Default",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            GRID_X, CH - 38, 130, 28,
            m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_DEFAULT, hi, nullptr);
        m_btn_save = CreateWindowExA(0, "BUTTON", "Save to JSON",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            GRID_X + 140, CH - 38, 130, 28,
            m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_SAVE, hi, nullptr);

        m_chk_density = CreateWindowExA(0, "BUTTON", "Density scoring",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            GRID_X + 290, CH - 50, 190, 18,
            m_hwnd, (HMENU)(UINT_PTR)IDC_CHK_DENSITY, hi, nullptr);

        m_chk_motion = CreateWindowExA(0, "BUTTON", "Motion scoring",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            GRID_X + 290, CH - 30, 190, 18,
            m_hwnd, (HMENU)(UINT_PTR)IDC_CHK_MOTION, hi, nullptr);
        return;
    }

    CreateWindowExA(0, "STATIC", "Stable hardware layers: select one and tune depth/size/copies.",
        WS_CHILD | WS_VISIBLE,
        10, 10, CW - 20, 18,
        m_hwnd, nullptr, hi, nullptr);

    m_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        10, 34, 168, 310,
        m_hwnd, (HMENU)(UINT_PTR)IDC_LIST, hi, nullptr);

    int x = 196;
    CreateWindowExA(0, "STATIC", "Layer", WS_CHILD | WS_VISIBLE,
        x, 38, 64, 18, m_hwnd, nullptr, hi, nullptr);
    m_edt_name = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        x + 64, 34, 200, 24,
        m_hwnd, (HMENU)(UINT_PTR)IDC_EDT_NAME, hi, nullptr);

    CreateWindowExA(0, "STATIC", "Depth", WS_CHILD | WS_VISIBLE,
        x, 76, 64, 18, m_hwnd, nullptr, hi, nullptr);
    m_edt_depth = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 64, 72, 76, 24,
        m_hwnd, (HMENU)(UINT_PTR)IDC_EDT_DEPTH, hi, nullptr);
    m_btn_dep_dec = CreateWindowExA(0, "BUTTON", "-", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 148, 72, 28, 24, m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_DEP_DEC, hi, nullptr);
    m_btn_dep_inc = CreateWindowExA(0, "BUTTON", "+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 180, 72, 28, 24, m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_DEP_INC, hi, nullptr);

    CreateWindowExA(0, "STATIC", "Width", WS_CHILD | WS_VISIBLE,
        x, 112, 64, 18, m_hwnd, nullptr, hi, nullptr);
    m_edt_width = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 64, 108, 76, 24,
        m_hwnd, (HMENU)(UINT_PTR)IDC_EDT_WIDTH, hi, nullptr);
    m_btn_wid_dec = CreateWindowExA(0, "BUTTON", "-", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 148, 108, 28, 24, m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_WID_DEC, hi, nullptr);
    m_btn_wid_inc = CreateWindowExA(0, "BUTTON", "+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x + 180, 108, 28, 24, m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_WID_INC, hi, nullptr);

    CreateWindowExA(0, "STATIC", "Copies", WS_CHILD | WS_VISIBLE,
        x, 148, 64, 18, m_hwnd, nullptr, hi, nullptr);
    m_edt_copy_cnt = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 64, 144, 76, 24,
        m_hwnd, (HMENU)(UINT_PTR)IDC_EDT_COPY_CNT, hi, nullptr);

    CreateWindowExA(0, "STATIC", "Spacing", WS_CHILD | WS_VISIBLE,
        x, 184, 64, 18, m_hwnd, nullptr, hi, nullptr);
    m_edt_copy_spc = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x + 64, 180, 76, 24,
        m_hwnd, (HMENU)(UINT_PTR)IDC_EDT_COPY_SPC, hi, nullptr);

    m_lbl_copy_eff = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        x, 214, 280, 18,
        m_hwnd, nullptr, hi, nullptr);

    m_chk_solo = CreateWindowExA(0, "BUTTON", "Solo selected layer",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, 246, 180, 20,
        m_hwnd, (HMENU)(UINT_PTR)IDC_CHK_SOLO, hi, nullptr);

    m_btn_spread = CreateWindowExA(0, "BUTTON", "Even Spread",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, 278, 112, 28,
        m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_SPREAD, hi, nullptr);

    m_btn_default = CreateWindowExA(0, "BUTTON", "Load Default",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, CH - 38, 130, 28,
        m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_DEFAULT, hi, nullptr);
    m_btn_save = CreateWindowExA(0, "BUTTON", "Save to JSON",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, CH - 38, 130, 28,
        m_hwnd, (HMENU)(UINT_PTR)IDC_BTN_SAVE, hi, nullptr);

    SetWindowSubclass(m_edt_depth,    numeric_edit_proc, 0, 0);
    SetWindowSubclass(m_edt_width,    numeric_edit_proc, 0, 0);
    SetWindowSubclass(m_edt_copy_cnt, numeric_edit_proc, 0, 0);
    SetWindowSubclass(m_edt_copy_spc, numeric_edit_proc, 0, 0);
}

void EditorInfoWindow::apply_font_to_children() {
    m_font = CreateFontA(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!m_font) return;
    HFONT f = m_font;
    EnumChildWindows(m_hwnd, [](HWND h, LPARAM lp) -> BOOL {
        SendMessageA(h, WM_SETFONT, (WPARAM)(HFONT)lp, FALSE);
        return TRUE;
    }, (LPARAM)f);
}

// ---------------------------------------------------------------------------
void EditorInfoWindow::mark_dirty() {
    if (!m_saved) return;
    m_saved = false;
    std::string title = std::string(editor_base_title(m_cfg)) + "  [unsaved]";
    SetWindowTextA(m_hwnd, title.c_str());
}

void EditorInfoWindow::show_and_sync() {
    if (!m_hwnd) return;

    bool was_hidden = !IsWindowVisible(m_hwnd);
    if (was_hidden) m_list_dirty = true;

    if (m_list_dirty) {
        populate_layer_list();
        m_list_dirty = false;
    }

    HWND focused = GetFocus();
    bool editing = (focused == m_edt_name   ||
                    focused == m_edt_depth  || focused == m_edt_width ||
                    focused == m_edt_copy_cnt || focused == m_edt_copy_spc);
    if (!editing)
        populate_detail(m_cur_sel);

    if (was_hidden) {
        m_editor.set_thumb_requested(true);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
}

void EditorInfoWindow::hide() {
    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        m_editor.set_solo(false);
        if (m_chk_solo) SendMessageA(m_chk_solo, BM_SETCHECK, BST_UNCHECKED, 0);
        m_editor.set_thumb_requested(false);
        ShowWindow(m_hwnd, SW_HIDE);
    }
}

void EditorInfoWindow::on_new_frame(const std::vector<LayerFrame>& /*frames*/) {
    // No-op.
}

// ---------------------------------------------------------------------------
void EditorInfoWindow::update_copies_display() {
    if (m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) {
        SetWindowTextA(m_lbl_copy_eff, "");
        return;
    }
    const auto& copies = m_cfg.layers[m_cur_sel].copies;
    if (copies.empty()) {
        SetWindowTextA(m_lbl_copy_eff, "(no copies)");
        return;
    }
    char buf[256];
    int off = snprintf(buf, sizeof(buf), "-> ");
    if (off < 0)
        off = 0;
    if (off >= (int)sizeof(buf))
        off = (int)sizeof(buf) - 1;

    for (int i = 0; i < (int)copies.size() && i < 8; ++i) {
        int remaining = (int)sizeof(buf) - off;
        if (remaining <= 1)
            break;
        int wrote = snprintf(buf + off, remaining, i ? ", %.2f" : "%.2f", copies[i]);
        if (wrote < 0)
            break;
        if (wrote >= remaining) {
            off = (int)sizeof(buf) - 1;
            break;
        }
        off += wrote;
    }
    if (copies.size() > 8) {
        int remaining = (int)sizeof(buf) - off;
        if (remaining > 1)
            snprintf(buf + off, remaining, " ...");
    }
    SetWindowTextA(m_lbl_copy_eff, buf);
}

// ---------------------------------------------------------------------------
std::string EditorInfoWindow::layer_list_text(int idx) const {
    if (idx < 0 || idx >= (int)m_cfg.layers.size()) return {};
    if (!use_simple_layer_editor(m_cfg))
        return m_cfg.layers[idx].id;

    char buf[128];
    snprintf(buf, sizeof(buf), "%d. %-14s  %.2fm",
        idx, m_cfg.layers[idx].id.c_str(), m_cfg.layers[idx].depth_meters);
    return buf;
}

void EditorInfoWindow::refresh_list_entry(int idx) {
    std::string txt = layer_list_text(idx);
    m_updating = true;
    SendMessageA(m_list, LB_DELETESTRING, idx, 0);
    SendMessageA(m_list, LB_INSERTSTRING, idx, (LPARAM)txt.c_str());
    SendMessageA(m_list, LB_SETCURSEL, m_cur_sel, 0);
    m_updating = false;
}

void EditorInfoWindow::populate_layer_list() {
    m_updating = true;
    SendMessageA(m_list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(m_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < (int)m_cfg.layers.size(); ++i) {
        std::string txt = layer_list_text(i);
        SendMessageA(m_list, LB_ADDSTRING, 0, (LPARAM)txt.c_str());
    }
    int n = (int)m_cfg.layers.size();
    if (m_cur_sel < 0 && n > 0) m_cur_sel = 0;
    if (m_cur_sel >= n) m_cur_sel = n - 1;
    if (m_cur_sel >= 0)
        SendMessageA(m_list, LB_SETCURSEL, m_cur_sel, 0);
    m_editor.set_selected(m_cur_sel);
    SendMessageA(m_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_list, nullptr, FALSE);
    m_updating = false;
}

void EditorInfoWindow::populate_detail(int idx) {
    m_updating = true;

    if (idx < 0 || idx >= (int)m_cfg.layers.size()) {
        SetWindowTextA(m_edt_name,     "");
        SetWindowTextA(m_edt_depth,    "");
        SetWindowTextA(m_edt_width,    "");
        SetWindowTextA(m_edt_copy_cnt, "0");
        SetWindowTextA(m_edt_copy_spc, "0.20");
        SetWindowTextA(m_lbl_copy_eff, "");
        m_updating = false;
        return;
    }

    const LayerConfig& lc = m_cfg.layers[idx];
    char buf[128];

    SetWindowTextA(m_edt_name, lc.id.c_str());

    snprintf(buf, sizeof(buf), "%.2f", lc.depth_meters);
    SetWindowTextA(m_edt_depth, buf);

    snprintf(buf, sizeof(buf), "%.2f", lc.quad_width_meters);
    SetWindowTextA(m_edt_width, buf);

    snprintf(buf, sizeof(buf), "%d", copies_count(lc));
    SetWindowTextA(m_edt_copy_cnt, buf);

    snprintf(buf, sizeof(buf), "%.3f", copies_spacing(lc));
    SetWindowTextA(m_edt_copy_spc, buf);

    m_updating = false;
    update_copies_display();
}

// ---------------------------------------------------------------------------
void EditorInfoWindow::adjust_depth(float delta) {
    if (m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    float& d = m_cfg.layers[m_cur_sel].depth_meters;
    d = std::max(0.05f, d + delta);
    char buf[16]; snprintf(buf, sizeof(buf), "%.2f", d);
    m_updating = true; SetWindowTextA(m_edt_depth, buf); m_updating = false;
    refresh_list_entry(m_cur_sel);
    mark_dirty();
}

void EditorInfoWindow::adjust_width(float delta) {
    if (m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    float& w = m_cfg.layers[m_cur_sel].quad_width_meters;
    w = std::max(0.05f, w + delta);
    char buf[16]; snprintf(buf, sizeof(buf), "%.2f", w);
    m_updating = true; SetWindowTextA(m_edt_width, buf); m_updating = false;
    mark_dirty();
}

void EditorInfoWindow::adjust_copy_cnt(int delta) {
    if (m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    LayerConfig& lc = m_cfg.layers[m_cur_sel];
    int cnt = std::max(0, std::min(16, copies_count(lc) + delta));
    regen_copies(lc, cnt, copies_spacing(lc));
    char buf[8]; snprintf(buf, sizeof(buf), "%d", cnt);
    m_updating = true; SetWindowTextA(m_edt_copy_cnt, buf); m_updating = false;
    update_copies_display();
    mark_dirty();
}

void EditorInfoWindow::adjust_copy_spc(float delta) {
    if (m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    LayerConfig& lc = m_cfg.layers[m_cur_sel];
    float spc = std::max(0.005f, copies_spacing(lc) + delta);
    regen_copies(lc, copies_count(lc), spc);
    char buf[16]; snprintf(buf, sizeof(buf), "%.3f", spc);
    m_updating = true; SetWindowTextA(m_edt_copy_spc, buf); m_updating = false;
    update_copies_display();
    mark_dirty();
}

// ---------------------------------------------------------------------------
void EditorInfoWindow::on_name_commit() {
    if (m_updating || m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    char buf[64]; GetWindowTextA(m_edt_name, buf, sizeof(buf));
    std::string newname(buf);
    if (newname.empty() || newname == m_cfg.layers[m_cur_sel].id) return;
    m_cfg.layers[m_cur_sel].id = newname;
    std::string txt = layer_list_text(m_cur_sel);
    m_updating = true;
    SendMessageA(m_list, LB_DELETESTRING, m_cur_sel, 0);
    SendMessageA(m_list, LB_INSERTSTRING, m_cur_sel, (LPARAM)txt.c_str());
    SendMessageA(m_list, LB_SETCURSEL, m_cur_sel, 0);
    m_updating = false;
    mark_dirty();
}

void EditorInfoWindow::on_depth_commit() {
    if (m_updating || m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    char buf[32]; GetWindowTextA(m_edt_depth, buf, sizeof(buf));
    float val = std::max(0.05f, (float)atof(buf));
    m_cfg.layers[m_cur_sel].depth_meters = val;
    snprintf(buf, sizeof(buf), "%.2f", val);
    m_updating = true; SetWindowTextA(m_edt_depth, buf); m_updating = false;
    refresh_list_entry(m_cur_sel);
    mark_dirty();
}

void EditorInfoWindow::on_width_commit() {
    if (m_updating || m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    char buf[32]; GetWindowTextA(m_edt_width, buf, sizeof(buf));
    float val = std::max(0.05f, (float)atof(buf));
    m_cfg.layers[m_cur_sel].quad_width_meters = val;
    snprintf(buf, sizeof(buf), "%.2f", val);
    m_updating = true; SetWindowTextA(m_edt_width, buf); m_updating = false;
    mark_dirty();
}

void EditorInfoWindow::on_copy_cnt_commit() {
    if (m_updating || m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    char buf[16]; GetWindowTextA(m_edt_copy_cnt, buf, sizeof(buf));
    int cnt = std::max(0, std::min(16, atoi(buf)));
    LayerConfig& lc = m_cfg.layers[m_cur_sel];
    regen_copies(lc, cnt, copies_spacing(lc));
    snprintf(buf, sizeof(buf), "%d", cnt);
    m_updating = true; SetWindowTextA(m_edt_copy_cnt, buf); m_updating = false;
    update_copies_display();
    mark_dirty();
}

void EditorInfoWindow::on_copy_spc_commit() {
    if (m_updating || m_cur_sel < 0 || m_cur_sel >= (int)m_cfg.layers.size()) return;
    char buf[32]; GetWindowTextA(m_edt_copy_spc, buf, sizeof(buf));
    float spc = std::max(0.005f, (float)atof(buf));
    LayerConfig& lc = m_cfg.layers[m_cur_sel];
    regen_copies(lc, copies_count(lc), spc);
    snprintf(buf, sizeof(buf), "%.3f", spc);
    m_updating = true; SetWindowTextA(m_edt_copy_spc, buf); m_updating = false;
    update_copies_display();
    mark_dirty();
}

void EditorInfoWindow::on_load_default() {
    if (use_simple_layer_editor(m_cfg)) {
        GameConfig fresh = is_snes_cart_rom(m_cfg)
            ? GameConfig::make_default_snes(m_cfg.game)
            : is_genesis_cart_rom(m_cfg)
                ? GameConfig::make_default_genesis(m_cfg.game)
                : is_gbc_cart_rom(m_cfg)
                    ? GameConfig::make_default_gbc(m_cfg.game)
                    : GameConfig::make_default_gb(m_cfg.game);
        fresh.config_path = m_cfg.config_path;
        m_cfg.virtual_width = fresh.virtual_width;
        m_cfg.virtual_height = fresh.virtual_height;
        m_cfg.quad_y_meters = fresh.quad_y_meters;
        m_cfg.layers = std::move(fresh.layers);
        memset(m_cfg.palette_route, 0, sizeof(m_cfg.palette_route));
        m_cur_sel = std::clamp(m_cur_sel, 0, (int)m_cfg.layers.size() - 1);
        m_editor.set_selected(m_cur_sel);
        populate_layer_list();
        populate_detail(m_cur_sel);
        mark_dirty();
        return;
    }
    memset(m_cfg.palette_route, 0, sizeof(m_cfg.palette_route));
    m_editor.push_ctrl_update();
    repaint_palette_grid();
    mark_dirty();
}

void EditorInfoWindow::on_layer_select() {
    if (m_updating) return;
    int sel = (int)SendMessageA(m_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    m_cur_sel = sel;
    m_editor.set_selected(sel);
    populate_detail(m_cur_sel);
}

void EditorInfoWindow::on_spread() {
    even_spread_layer_depths(m_cfg.layers);
    populate_layer_list();
    populate_detail(m_cur_sel);
    mark_dirty();
}

void EditorInfoWindow::on_save() {
    m_cfg.save();
    m_saved = true;
    std::string title = std::string(editor_base_title(m_cfg)) + "  [saved]";
    SetWindowTextA(m_hwnd, title.c_str());
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK EditorInfoWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<EditorInfoWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_KEYDOWN: {
        if (!self) return 0;
        if (wp == 'P' && self->m_mame_hwnd) {
            self->m_mame_paused = !self->m_mame_paused;
            PostMessageA(self->m_mame_hwnd, WM_KEYDOWN, 'P', 1);
            PostMessageA(self->m_mame_hwnd, WM_KEYUP,   'P', 1);
            std::string title = editor_base_title(self->m_cfg);
            title += self->m_mame_paused ? "  [paused]" : "";
            SetWindowTextA(self->m_hwnd, title.c_str());
        }
        return 0;
    }

    case WM_COMMAND: {
        if (!self) return 0;
        int id    = LOWORD(wp);
        int notif = HIWORD(wp);
        switch (id) {
        case IDC_LIST:
            if (notif == LBN_SELCHANGE) self->on_layer_select();
            break;
        case IDC_EDT_NAME:
            if (notif == EN_KILLFOCUS) self->on_name_commit();       break;
        case IDC_EDT_DEPTH:
            if (notif == EN_KILLFOCUS) self->on_depth_commit();      break;
        case IDC_EDT_WIDTH:
            if (notif == EN_KILLFOCUS) self->on_width_commit();      break;
        case IDC_EDT_COPY_CNT:
            if (notif == EN_KILLFOCUS) self->on_copy_cnt_commit();   break;
        case IDC_EDT_COPY_SPC:
            if (notif == EN_KILLFOCUS) self->on_copy_spc_commit();   break;
        case IDC_BTN_DEP_DEC:    self->adjust_depth(-0.05f);  break;
        case IDC_BTN_DEP_INC:    self->adjust_depth(+0.05f);  break;
        case IDC_BTN_WID_DEC:    self->adjust_width(-0.05f);  break;
        case IDC_BTN_WID_INC:    self->adjust_width(+0.05f);  break;
        case IDC_BTN_DEFAULT:    self->on_load_default();       break;
        case IDC_BTN_SPREAD:     self->on_spread();             break;
        case IDC_BTN_SAVE:       self->on_save();               break;
        case IDC_CHK_DENSITY: {
            bool checked = (SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (self->m_router) self->m_router->set_density_scoring(checked);
            break;
        }
        case IDC_CHK_MOTION: {
            bool checked = (SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (self->m_router) self->m_router->set_motion_scoring(checked);
            break;
        }
        case IDC_CHK_SOLO: {
            bool checked = (SendMessageA((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            self->m_editor.set_solo(checked);
            break;
        }
        }
        return 0;
    }

    case WM_APP + 1: {  // scroll wheel
        if (!self) return 0;
        int id  = (int)(UINT_PTR)wp;
        int dir = (int)lp;
        switch (id) {
        case IDC_EDT_DEPTH:    self->adjust_depth(dir * 0.05f);    break;
        case IDC_EDT_WIDTH:    self->adjust_width(dir * 0.05f);    break;
        case IDC_EDT_COPY_CNT: self->adjust_copy_cnt(dir);         break;
        case IDC_EDT_COPY_SPC: self->adjust_copy_spc(dir * 0.01f); break;
        }
        return 0;
    }

    case WM_APP + 2: {  // Enter key commit
        if (!self) return 0;
        int  id   = (int)(UINT_PTR)wp;
        HWND edit = (HWND)lp;
        switch (id) {
        case IDC_EDT_NAME:     self->on_name_commit();      break;
        case IDC_EDT_DEPTH:    self->on_depth_commit();     break;
        case IDC_EDT_WIDTH:    self->on_width_commit();     break;
        case IDC_EDT_COPY_CNT: self->on_copy_cnt_commit();  break;
        case IDC_EDT_COPY_SPC: self->on_copy_spc_commit();  break;
        }
        SendMessageA(edit, EM_SETSEL, 0, -1);
        return 0;
    }

    case WM_SIZE: {
        if (!self) return 0;
        int cw = (int)LOWORD(lp);
        int ch = (int)HIWORD(lp);

        if (use_simple_layer_editor(self->m_cfg)) {
            if (self->m_list)
                SetWindowPos(self->m_list, nullptr, 10, 34, 168, ch - 84, SWP_NOZORDER | SWP_NOACTIVATE);

            int x = 196;
            int edit_w = (std::max)(76, cw - x - 170);
            if (self->m_edt_name)
                SetWindowPos(self->m_edt_name, nullptr, x + 64, 34, edit_w + 60, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_edt_depth)
                SetWindowPos(self->m_edt_depth, nullptr, x + 64, 72, edit_w, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_dep_dec)
                SetWindowPos(self->m_btn_dep_dec, nullptr, x + 72 + edit_w, 72, 28, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_dep_inc)
                SetWindowPos(self->m_btn_dep_inc, nullptr, x + 104 + edit_w, 72, 28, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_edt_width)
                SetWindowPos(self->m_edt_width, nullptr, x + 64, 108, edit_w, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_wid_dec)
                SetWindowPos(self->m_btn_wid_dec, nullptr, x + 72 + edit_w, 108, 28, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_wid_inc)
                SetWindowPos(self->m_btn_wid_inc, nullptr, x + 104 + edit_w, 108, 28, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_edt_copy_cnt)
                SetWindowPos(self->m_edt_copy_cnt, nullptr, x + 64, 144, edit_w, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_edt_copy_spc)
                SetWindowPos(self->m_edt_copy_spc, nullptr, x + 64, 180, edit_w, 24, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_lbl_copy_eff)
                SetWindowPos(self->m_lbl_copy_eff, nullptr, x, 214, cw - x - 16, 18, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_chk_solo)
                SetWindowPos(self->m_chk_solo, nullptr, x, 246, cw - x - 16, 20, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_spread)
                SetWindowPos(self->m_btn_spread, nullptr, x, 278, 112, 28, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_default)
                SetWindowPos(self->m_btn_default, nullptr, 10, ch - 38, 130, 28, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_btn_save)
                SetWindowPos(self->m_btn_save, nullptr, 150, ch - 38, 130, 28, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        // Reserve space below grid for: legend (18) + gap (6) + buttons (28) + margin (10)
        static const int BOTTOM = 18 + 6 + 28 + 10;
        int grid_w = cw - 2 * GRID_X;
        int grid_h = ch - GRID_Y - BOTTOM;
        if (grid_w < 32 || grid_h < 32) return 0;

        // Snap to whole cells so cells are always equal-sized
        self->m_cell_w = grid_w / 16;
        self->m_cell_h = grid_h / 16;
        int gw = self->m_cell_w * 16;
        int gh = self->m_cell_h * 16;

        if (self->m_pal_grid)
            SetWindowPos(self->m_pal_grid, nullptr, GRID_X, GRID_Y, gw, gh, SWP_NOZORDER | SWP_NOACTIVATE);

        int leg_y  = GRID_Y + gh + 6;
        int leg_w  = gw / 4;
        for (int g = 0; g < 4; ++g)
            if (self->m_lbl_grp[g])
                SetWindowPos(self->m_lbl_grp[g], nullptr,
                             GRID_X + g * leg_w, leg_y, leg_w, 18,
                             SWP_NOZORDER | SWP_NOACTIVATE);

        int btn_y = ch - 38;
        if (self->m_btn_default)
            SetWindowPos(self->m_btn_default, nullptr, GRID_X,       btn_y, 130, 28, SWP_NOZORDER | SWP_NOACTIVATE);
        if (self->m_btn_save)
            SetWindowPos(self->m_btn_save,    nullptr, GRID_X + 140, btn_y, 130, 28, SWP_NOZORDER | SWP_NOACTIVATE);

        InvalidateRect(self->m_pal_grid, nullptr, TRUE);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
