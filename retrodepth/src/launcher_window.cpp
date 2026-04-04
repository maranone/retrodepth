#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "launcher_window.h"
#include "settings_io.h"
#include "system_features.h"

namespace fs = std::filesystem;
using namespace Gdiplus;

namespace {

enum : int {
    IDC_COVER_GRID     = 1000,
    IDC_STATUS        = 1002,
    IDC_ROMS_PATH     = 1003,
    IDC_BIOS_PATH     = 1004,
    IDC_PICK_ROMS     = 1005,
    IDC_PICK_BIOS     = 1006,
    IDC_LAUNCH_PREV   = 1007,
    IDC_LAUNCH_VR     = 1008,
    IDC_REFRESH       = 1009,
    IDC_LAUNCH_VR_VIEW = 1010,
    IDC_INSPECT_GAME  = 1011,
    IDC_DYNAMIC_DEPTH = 1012,
    IDC_HIDE_INCORRECT = 1013,
    IDC_BETA_DEPTH    = 1014,
    IDC_GENERATE_PREVIEWS = 1015,
    IDC_README        = 1016,
    IDC_FOLDER_FILTER = 1017,
    IDC_SETTINGS      = 1018,
    IDC_FOLDER_FILTER_LIST = 1019,
};

static constexpr int kMargin = 16;
static constexpr int kTitleH = 56;
static constexpr int kToolbarH = 36;
static constexpr int kToolbarGap = 12;
static constexpr int kPathH = 20;
static constexpr int kButtonH = 28;
static constexpr int kLaunchH = 36;
static constexpr int kCoverPad = 12;
static constexpr int kCoverTileW = 206;
static constexpr int kCoverTileH = 214;
static constexpr int kCoverImageH = 156;
static constexpr int kDefaultLauncherCols = 4;
static constexpr int kDefaultLauncherClientH = 660;
static constexpr int kDefaultLauncherSlackW = 24;
static constexpr UINT WM_PREVIEW_RESULT = WM_APP + 3;
static constexpr UINT WM_PREVIEW_DONE   = WM_APP + 4;
static constexpr UINT_PTR kPreviewTimerId = 1;
static constexpr UINT kPreviewTimerMs = 100;
static constexpr int kPreviewFrameCount = 20;
static constexpr int kPreviewFrameSeconds = 1;
static constexpr int kPreviewSkipSeconds = 12;
static constexpr ULONGLONG kSelectedPreviewPeriodMs = 125ULL;
static constexpr ULONGLONG kBackgroundPreviewPeriodMs = 10000ULL;
static constexpr int kPreviewMaxWidth = 320;
static constexpr int kPreviewMaxHeight = 240;
static constexpr long kPreviewJpegQuality = 70;

constexpr int default_cover_grid_width_for_cols(int cols) {
    return kCoverPad + cols * (kCoverTileW + kCoverPad);
}

constexpr int kDefaultLauncherClientW =
    (2 * kMargin) + default_cover_grid_width_for_cols(kDefaultLauncherCols) + kDefaultLauncherSlackW;

enum AuditStatus : int {
    AUDIT_PENDING = 0,
    AUDIT_OK = 1,
    AUDIT_BROKEN = 2,
};

struct RomEntry {
    std::string short_name;
    std::string storage_key;
    std::string title;
    fs::path cover_path;
    fs::path source_dir;
    std::string folder_key;
    std::string folder_label;
    std::vector<fs::path> preview_frames;
    std::string system; // "neogeo", "cps1", "cps2", or empty
    AuditStatus audit_status = AUDIT_PENDING;
    std::string audit_summary;
    bool hidden = false;
};

struct PreviewResultMessage {
    int generation = 0;
    std::string short_name;
    std::string storage_key;
    bool success = false;
    int frame_count = 0;
    std::string summary;
    AuditStatus audit_status = AUDIT_PENDING;
    std::string audit_summary;
};

struct PreviewWorkerPayload {
    HWND hwnd = nullptr;
    int generation = 0;
    HANDLE cancel_event = nullptr;
    Settings settings;
    fs::path exe_dir;
    std::vector<RomEntry> roms;
};

struct FolderScanStats {
    std::string label;
    int files_found = 0;
    int rom_files_detected = 0;
    int roms_accepted = 0;
    int roms_discarded_unknown = 0;
    int roms_discarded_duplicate = 0;
    int roms_discarded_special = 0;
};

struct LauncherState {
    fs::path exe_path;
    fs::path exe_dir;
    Settings settings;
    std::vector<RomEntry> roms;
    std::unordered_map<std::string, std::string> title_map;
    HWND title = nullptr;
    HWND cover_grid = nullptr;
    HWND status = nullptr;
    HWND settings_btn = nullptr;
    HWND roms_path = nullptr;
    HWND bios_path = nullptr;
    HWND folder_filter_label = nullptr;
    HWND folder_filter_combo = nullptr;
    HWND folder_filter_list = nullptr;
    HWND pick_roms_btn = nullptr;
    HWND pick_bios_btn = nullptr;
    HWND refresh_btn = nullptr;
    HWND readme_btn = nullptr;
    HWND launch_prev_btn = nullptr;
    HWND launch_vr_btn = nullptr;
    HWND launch_vr_view_btn = nullptr;
    HWND dynamic_chk   = nullptr;
    HWND beta_chk      = nullptr;
    HWND density_chk   = nullptr;
    HWND motion_chk    = nullptr;
    HWND hide_mame_chk = nullptr;
    HWND hide_incorrect_chk = nullptr;
    HWND generate_previews_chk = nullptr;
    int selected_index = -1;
    int scroll_y = 0;
    int preview_generation = 0;
    int preview_total = 0;
    int preview_completed = 0;
    int preview_success = 0;
    bool preview_running = false;
    HANDLE preview_thread = nullptr;
    HANDLE preview_cancel_event = nullptr;
    bool settings_open = false;
    HFONT title_font = nullptr;
    HBRUSH title_bg_brush = nullptr;
    std::vector<std::string> folder_filter_keys;
    std::vector<std::wstring> folder_filter_labels;
    std::string active_folder_filter_key;
    int folder_filter_drop_width = 320;
    std::unordered_map<std::wstring, std::unique_ptr<Image>> preview_image_cache;
};

ULONG_PTR g_gdiplus_token = 0;

bool launch_mode(LauncherState* st, bool preview, bool spectator, bool inspect = false, bool dynamic = false, bool beta = false, bool density = false, bool motion = false, bool hide_mame = true);

bool preview_cancelled(HANDLE cancel_event) {
    return cancel_event && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0;
}

void hide_folder_filter_list(LauncherState* st) {
    if (!st || !st->folder_filter_list)
        return;
    ShowWindow(st->folder_filter_list, SW_HIDE);
}

int folder_filter_selected_index(const LauncherState* st) {
    if (!st)
        return 0;
    for (int i = 0; i < (int)st->folder_filter_keys.size(); ++i) {
        if (st->folder_filter_keys[i] == st->active_folder_filter_key)
            return i;
    }
    return 0;
}

void sync_folder_filter_button(LauncherState* st) {
    if (!st || !st->folder_filter_combo)
        return;
    int sel = folder_filter_selected_index(st);
    std::wstring label = L"All folders";
    if (sel >= 0 && sel < (int)st->folder_filter_labels.size() && !st->folder_filter_labels[sel].empty())
        label = st->folder_filter_labels[sel];
    label += L"  v";
    SetWindowTextW(st->folder_filter_combo, label.c_str());
}

void show_folder_filter_list(LauncherState* st, HWND parent) {
    if (!st || !st->folder_filter_combo || !st->folder_filter_list || !parent)
        return;

    SendMessageW(st->folder_filter_list, LB_RESETCONTENT, 0, 0);
    for (const auto& label : st->folder_filter_labels)
        SendMessageW(st->folder_filter_list, LB_ADDSTRING, 0, (LPARAM)label.c_str());

    int sel = folder_filter_selected_index(st);
    SendMessageW(st->folder_filter_list, LB_SETCURSEL, (WPARAM)sel, 0);

    RECT combo_rc{};
    GetWindowRect(st->folder_filter_combo, &combo_rc);
    MapWindowPoints(HWND_DESKTOP, parent, (LPPOINT)&combo_rc, 2);

    int item_h = (int)SendMessageW(st->folder_filter_list, LB_GETITEMHEIGHT, 0, 0);
    if (item_h <= 0)
        item_h = 20;
    if (item_h <= 0)
        item_h = 18;

    const int count = (int)st->folder_filter_labels.size();
    const int visible = (std::min)((std::max)(count, 1), 12);
    const int combo_w = combo_rc.right - combo_rc.left;
    const int dropped_w = (std::max)(combo_w, st->folder_filter_drop_width);
    const int dropped_h = item_h * visible + 8;
    SetWindowPos(st->folder_filter_list, HWND_TOP, combo_rc.left, combo_rc.bottom + 2,
                 dropped_w, dropped_h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowScrollBar(st->folder_filter_list, SB_VERT, count > visible);
    SetFocus(st->folder_filter_list);
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), size);
    return out;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string path_utf8(const fs::path& p) {
    return narrow(p.wstring());
}

std::string normalized_folder_key(const fs::path& p) {
    try {
        return lower_ascii(path_utf8(fs::absolute(p).lexically_normal()));
    } catch (...) {
        return lower_ascii(path_utf8(p.lexically_normal()));
    }
}

std::string folder_filter_label_for_path(const std::vector<fs::path>& rom_dirs, const fs::path& source_dir) {
    fs::path src_abs;
    try {
        src_abs = fs::absolute(source_dir).lexically_normal();
    } catch (...) {
        src_abs = source_dir.lexically_normal();
    }

    for (const auto& rom_dir : rom_dirs) {
        fs::path root_abs;
        try {
            root_abs = fs::absolute(rom_dir).lexically_normal();
        } catch (...) {
            root_abs = rom_dir.lexically_normal();
        }

        std::error_code ec;
        fs::path rel = fs::relative(src_abs, root_abs, ec);
        if (ec || rel.empty())
            continue;
        auto it = rel.begin();
        if (it != rel.end() && it->wstring() == L"..")
            continue;

        std::string root_name = path_utf8(root_abs.filename());
        if (root_name.empty())
            root_name = path_utf8(root_abs);
        if (rel == ".")
            return root_name;
        if (rom_dirs.size() > 1)
            return root_name + " / " + path_utf8(rel);
        return path_utf8(rel);
    }

    return source_dir.filename().empty() ? path_utf8(source_dir) : path_utf8(source_dir.filename());
}

fs::path find_repo_root(const fs::path& exe_path) {
    fs::path cur = exe_path.parent_path();
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(cur / "mame-src/src/mame/neogeo/neogeo.cpp"))
            return cur;
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return exe_path.parent_path();
}

std::unordered_map<std::string, std::string> load_title_map(const fs::path& repo_root) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream f(repo_root / "mame-src/src/mame/neogeo/neogeo.cpp");
    if (!f.is_open()) return out;

    std::string line;
    while (std::getline(f, line)) {
        size_t game_pos = line.find("GAME(");
        if (game_pos == std::string::npos) continue;

        size_t first_comma = line.find(',', game_pos);
        if (first_comma == std::string::npos) continue;
        size_t short_start = line.find_first_not_of(" \t", first_comma + 1);
        if (short_start == std::string::npos) continue;
        size_t short_end = line.find(',', short_start);
        if (short_end == std::string::npos) continue;

        std::string short_name = line.substr(short_start, short_end - short_start);
        while (!short_name.empty() && (short_name.back() == ' ' || short_name.back() == '\t'))
            short_name.pop_back();

        size_t q1 = line.find('"', short_end);
        if (q1 == std::string::npos) continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        size_t q3 = line.find('"', q2 + 1);
        if (q3 == std::string::npos) continue;
        size_t q4 = line.find('"', q3 + 1);
        if (q4 == std::string::npos) continue;

        std::string title = line.substr(q3 + 1, q4 - q3 - 1);
        if (!short_name.empty() && !title.empty())
            out[short_name] = title;
    }
    return out;
}

std::unordered_map<std::string, std::string> load_title_map_from_mame(const std::string& mame_exe) {
    std::unordered_map<std::string, std::string> out;
    if (mame_exe.empty()) return out;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return out;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + mame_exe + "\" -listfull";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return out;
    }

    CloseHandle(write_pipe);

    std::string text;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        text.append(buffer, buffer + read);

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t q1 = line.find('"');
        size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.rfind('"');
        if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1) continue;

        std::string short_name = line.substr(0, q1);
        while (!short_name.empty() && (short_name.back() == ' ' || short_name.back() == '\t'))
            short_name.pop_back();
        if (short_name.empty()) continue;

        std::string title = line.substr(q1 + 1, q2 - q1 - 1);
        if (!title.empty())
            out[short_name] = title;
    }
    return out;
}

// Parse GAME() macros from a MAME source file to collect ROM short-names.
std::unordered_map<std::string, int> load_cps_rom_sets(const fs::path& repo_root) {
    std::unordered_map<std::string, int> out; // short_name -> 1=cps1, 2=cps2
    for (int sys = 1; sys <= 2; ++sys) {
        std::string fname = (sys == 1)
            ? "mame-src/src/mame/capcom/cps1.cpp"
            : "mame-src/src/mame/capcom/cps2.cpp";
        std::ifstream f(repo_root / fname);
        if (!f.is_open()) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t p = line.find("GAME(");
            if (p == std::string::npos) p = line.find("GAME_DRIVER(");
            if (p == std::string::npos) continue;
            size_t c1 = line.find(',', p);
            if (c1 == std::string::npos) continue;
            size_t s0 = line.find_first_not_of(" \t", c1 + 1);
            if (s0 == std::string::npos) continue;
            size_t s1 = line.find_first_of(", \t", s0);
            if (s1 == std::string::npos) continue;
            std::string name = line.substr(s0, s1 - s0);
            if (!name.empty())
                out[name] = sys;
        }
    }
    return out;
}

// Parse GAME() macros from the Konami driver files (tmnt.cpp, simpsons.cpp).
std::unordered_set<std::string> load_konami_rom_set(const fs::path& repo_root) {
    std::unordered_set<std::string> out;
    static const char* k_files[] = {
        "mame-src/src/mame/konami/tmnt.cpp",
        "mame-src/src/mame/konami/simpsons.cpp",
    };
    for (const auto* fname : k_files) {
        std::ifstream f(repo_root / fname);
        if (!f.is_open()) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t p = line.find("GAME(");
            if (p == std::string::npos) continue;
            size_t c1 = line.find(',', p);
            if (c1 == std::string::npos) continue;
            size_t s0 = line.find_first_not_of(" \t", c1 + 1);
            if (s0 == std::string::npos) continue;
            size_t s1 = line.find_first_of(", \t", s0);
            if (s1 == std::string::npos) continue;
            std::string name = line.substr(s0, s1 - s0);
            if (!name.empty()) out.insert(name);
        }
    }
    return out;
}

std::wstring choose_folder(HWND owner, const wchar_t* title) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return path;
}

void set_text(HWND hwnd, const std::string& text) {
    SetWindowTextW(hwnd, widen(text).c_str());
}

void set_status(LauncherState* st, const std::string& text) {
    if (st && st->status) set_text(st->status, text);
}

void invalidate_action_buttons(LauncherState* st) {
    if (!st) return;
    if (st->launch_vr_btn) InvalidateRect(st->launch_vr_btn, nullptr, TRUE);
}

std::vector<int> visible_rom_indices(const LauncherState* st);
ULONGLONG preview_period_ms(bool selected);
bool preview_frame_changed(const RomEntry& rom, bool selected, ULONGLONG now_ms, ULONGLONG previous_ms);
Image* cached_preview_image(LauncherState* st, const fs::path& path);

void update_cover_scrollbar(LauncherState* st) {
    if (!st || !st->cover_grid) return;
    RECT rc{};
    GetClientRect(st->cover_grid, &rc);
    int cols = (int)((rc.right - kCoverPad) / (kCoverTileW + kCoverPad));
    if (cols < 1) cols = 1;
    int visible_count = 0;
    for (const auto& rom : st->roms) {
        if (!rom.hidden) ++visible_count;
    }
    int rows = (int)((visible_count + cols - 1) / cols);
    int content_h = kCoverPad + rows * (kCoverTileH + kCoverPad);
    int max_scroll = content_h > rc.bottom ? (content_h - rc.bottom) : 0;
    if (st->scroll_y < 0) st->scroll_y = 0;
    if (st->scroll_y > max_scroll) st->scroll_y = max_scroll;

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content_h > 0 ? content_h - 1 : 0;
    si.nPage = rc.bottom;
    si.nPos = st->scroll_y;
    SetScrollInfo(st->cover_grid, SB_VERT, &si, TRUE);
}

void invalidate_preview_tiles(LauncherState* st) {
    if (!st || !st->cover_grid) return;
    static ULONGLONG last_tick = GetTickCount64();
    ULONGLONG now = GetTickCount64();

    RECT rc{};
    GetClientRect(st->cover_grid, &rc);
    int cols = (int)((rc.right - kCoverPad) / (kCoverTileW + kCoverPad));
    if (cols < 1) cols = 1;

    auto visible = visible_rom_indices(st);
    for (int vis_i = 0; vis_i < (int)visible.size(); ++vis_i) {
        int i = visible[vis_i];
        if (st->roms[i].preview_frames.size() < 2)
            continue;
        bool selected = (i == st->selected_index);
        if (!preview_frame_changed(st->roms[i], selected, now, last_tick))
            continue;

        int row = vis_i / cols;
        int col = vis_i % cols;
        int x = kCoverPad + col * (kCoverTileW + kCoverPad);
        int y = kCoverPad + row * (kCoverTileH + kCoverPad) - st->scroll_y;
        if (y + kCoverTileH < 0 || y > rc.bottom)
            continue;

        RECT img_rc{};
        img_rc.left = x + 8;
        img_rc.top = y + 8;
        img_rc.right = img_rc.left + kCoverTileW - 16;
        img_rc.bottom = img_rc.top + kCoverImageH;
        InvalidateRect(st->cover_grid, &img_rc, FALSE);
    }
    last_tick = now;
}

std::vector<int> visible_rom_indices(const LauncherState* st) {
    std::vector<int> out;
    if (!st) return out;
    out.reserve(st->roms.size());
    for (int i = 0; i < (int)st->roms.size(); ++i) {
        if (!st->roms[i].hidden)
            out.push_back(i);
    }
    return out;
}

bool hide_incorrect_enabled(const LauncherState* st) {
    return st && st->hide_incorrect_chk &&
        SendMessageW(st->hide_incorrect_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void rebuild_folder_filter_combo(LauncherState* st) {
    if (!st || !st->folder_filter_combo)
        return;

    std::string keep_key = st->active_folder_filter_key;
    st->folder_filter_keys.clear();
    st->folder_filter_labels.clear();
    st->folder_filter_keys.push_back("");
    st->folder_filter_labels.push_back(L"All folders");

    std::vector<std::pair<std::string, std::string>> entries;
    std::unordered_set<std::string> seen;
    for (const auto& rom : st->roms) {
        if (rom.folder_key.empty() || !seen.insert(rom.folder_key).second)
            continue;
        entries.push_back({ rom.folder_key, rom.folder_label.empty() ? path_utf8(rom.source_dir) : rom.folder_label });
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _stricmp(a.second.c_str(), b.second.c_str()) < 0;
    });

    int sel = 0;
    for (const auto& entry : entries) {
        st->folder_filter_keys.push_back(entry.first);
        st->folder_filter_labels.push_back(widen(entry.second));
        if (!keep_key.empty() && keep_key == entry.first)
            sel = (int)st->folder_filter_keys.size() - 1;
    }

    if (sel <= 0 && !keep_key.empty())
        keep_key.clear();
    st->active_folder_filter_key = keep_key;

    const int count = (int)st->folder_filter_labels.size();
    HDC hdc = GetDC(st->folder_filter_combo);
    if (hdc) {
        HFONT font = (HFONT)SendMessageW(st->folder_filter_combo, WM_GETFONT, 0, 0);
        HGDIOBJ old_font = font ? SelectObject(hdc, font) : nullptr;
        int max_width = 0;
        for (int i = 0; i < count; ++i) {
            const std::wstring& text = st->folder_filter_labels[i];
            if (text.empty())
                continue;
            SIZE sz{};
            if (GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz) && sz.cx > max_width)
                max_width = sz.cx;
        }
        if (old_font)
            SelectObject(hdc, old_font);
        ReleaseDC(st->folder_filter_combo, hdc);
        st->folder_filter_drop_width = max_width + 48;
    }
    sync_folder_filter_button(st);
    if (st->folder_filter_list && IsWindowVisible(st->folder_filter_list))
        show_folder_filter_list(st, GetParent(st->folder_filter_combo));
}

void ensure_valid_selection(LauncherState* st) {
    if (!st) return;
    if (st->selected_index >= 0 && st->selected_index < (int)st->roms.size() && !st->roms[st->selected_index].hidden)
        return;
    st->selected_index = -1;
    for (int i = 0; i < (int)st->roms.size(); ++i) {
        if (!st->roms[i].hidden) {
            st->selected_index = i;
            break;
        }
    }
}

void apply_visibility_filter(LauncherState* st) {
    if (!st) return;
    const bool hide_broken = hide_incorrect_enabled(st);
    for (auto& rom : st->roms) {
        bool hide_for_audit = hide_broken && rom.audit_status == AUDIT_BROKEN;
        bool hide_for_folder = !st->active_folder_filter_key.empty() && rom.folder_key != st->active_folder_filter_key;
        rom.hidden = hide_for_audit || hide_for_folder;
    }
    ensure_valid_selection(st);
    update_cover_scrollbar(st);
    if (st->cover_grid) InvalidateRect(st->cover_grid, nullptr, FALSE);
    invalidate_action_buttons(st);
}

bool generate_previews_enabled(const LauncherState* st) {
    return st && st->generate_previews_chk &&
        SendMessageW(st->generate_previews_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool preview_frame_changed(const RomEntry& rom, bool selected, ULONGLONG now_ms, ULONGLONG previous_ms) {
    if (rom.preview_frames.size() < 2)
        return false;
    ULONGLONG period = preview_period_ms(selected);
    return (now_ms / period) != (previous_ms / period);
}

Image* cached_preview_image(LauncherState* st, const fs::path& path) {
    if (!st || path.empty())
        return nullptr;
    const std::wstring key = path.wstring();
    auto it = st->preview_image_cache.find(key);
    if (it != st->preview_image_cache.end())
        return it->second.get();

    if (st->preview_image_cache.size() >= 96)
        st->preview_image_cache.clear();

    auto img = std::make_unique<Image>(key.c_str());
    if (!img || img->GetLastStatus() != Ok)
        return nullptr;

    Image* raw = img.get();
    st->preview_image_cache.emplace(key, std::move(img));
    return raw;
}

void layout_launcher(LauncherState* st, HWND hwnd) {
    if (!st) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);

    auto place = [&](HWND ctl, int x, int y, int w, int h, bool visible) {
        if (!ctl) return;
        if (visible) {
            MoveWindow(ctl, x, y, w, h, TRUE);
            ShowWindow(ctl, SW_SHOWNA);
        } else {
            ShowWindow(ctl, SW_HIDE);
        }
    };

    const int full_w = rc.right - 2 * kMargin;
    const int title_y = 0;
    const int toolbar_y = kTitleH + 12;
    const int status_y = rc.bottom - 28;
    const int content_y = toolbar_y + kToolbarH + 12;
    int content_h = status_y - content_y - 8;
    if (content_h < 120) content_h = 120;

    place(st->title, 0, title_y, rc.right, kTitleH, true);
    place(st->status, kMargin, status_y, full_w, 20, true);

    if (!st->settings_open) {
        const int settings_w = 110;
        const int preview_w = 128;
        const int vr_view_w = 170;
        const int vr_w = 146;
        const int label_w = 54;
        const int settings_x = rc.right - kMargin - settings_w;
        const int preview_x = settings_x - kToolbarGap - preview_w;
        const int vr_view_x = preview_x - kToolbarGap - vr_view_w;
        const int vr_x = vr_view_x - kToolbarGap - vr_w;
        const int combo_x = kMargin + label_w + 6;
        int combo_w = vr_x - kToolbarGap - combo_x;
        if (combo_w < 140) combo_w = 140;

        place(st->folder_filter_label, kMargin, toolbar_y + 7, label_w, 22, true);
        place(st->folder_filter_combo, combo_x, toolbar_y + 2, combo_w, kButtonH, true);
        place(st->launch_vr_btn, vr_x, toolbar_y, vr_w, kLaunchH, true);
        place(st->launch_vr_view_btn, vr_view_x, toolbar_y, vr_view_w, kLaunchH, true);
        place(st->launch_prev_btn, preview_x, toolbar_y, preview_w, kLaunchH, true);
        place(st->settings_btn, settings_x, toolbar_y, settings_w, kLaunchH, true);
        SetWindowTextW(st->settings_btn, L"Settings");

        place(st->cover_grid, kMargin, content_y, full_w, content_h, true);

        place(st->roms_path, 0, 0, 0, 0, false);
        place(st->bios_path, 0, 0, 0, 0, false);
        place(st->pick_roms_btn, 0, 0, 0, 0, false);
        place(st->pick_bios_btn, 0, 0, 0, 0, false);
        place(st->refresh_btn, 0, 0, 0, 0, false);
        place(st->readme_btn, 0, 0, 0, 0, false);
        place(st->dynamic_chk, 0, 0, 0, 0, false);
        place(st->beta_chk, 0, 0, 0, 0, false);
        place(st->density_chk, 0, 0, 0, 0, false);
        place(st->motion_chk, 0, 0, 0, 0, false);
        place(st->hide_mame_chk, 0, 0, 0, 0, false);
        place(st->hide_incorrect_chk, 0, 0, 0, 0, false);
        place(st->generate_previews_chk, 0, 0, 0, 0, false);
    } else {
        const int back_w = 110;
        const int button_w = 170;
        const int label_w = full_w - button_w - 16;
        const int left_x = kMargin + 18;
        const int row_h = 32;
        const int btn_x = rc.right - kMargin - button_w - 18;
        const int base_y = content_y + 18;
        const int checkbox_col_w = (full_w - 72) / 2;

        place(st->settings_btn, rc.right - kMargin - back_w, toolbar_y, back_w, kLaunchH, true);
        SetWindowTextW(st->settings_btn, L"Back");
        place(st->folder_filter_label, 0, 0, 0, 0, false);
        place(st->folder_filter_combo, 0, 0, 0, 0, false);
        place(st->launch_vr_btn, 0, 0, 0, 0, false);
        place(st->launch_vr_view_btn, 0, 0, 0, 0, false);
        place(st->cover_grid, 0, 0, 0, 0, false);

        place(st->roms_path, left_x, base_y + 4, label_w, kPathH, true);
        place(st->pick_roms_btn, btn_x, base_y, button_w, kButtonH, true);
        place(st->bios_path, left_x, base_y + row_h + 4, label_w, kPathH, true);
        place(st->pick_bios_btn, btn_x, base_y + row_h, button_w, kButtonH, true);

        place(st->refresh_btn, left_x, base_y + 76, 140, kButtonH, true);
        place(st->readme_btn, left_x + 152, base_y + 76, 140, kButtonH, true);
        place(st->launch_prev_btn, 0, 0, 0, 0, false);

        place(st->dynamic_chk, left_x, base_y + 128, checkbox_col_w, 22, true);
        place(st->beta_chk, left_x, base_y + 154, checkbox_col_w, 22, true);
        place(st->density_chk, left_x, base_y + 180, checkbox_col_w, 22, true);
        place(st->motion_chk, left_x, base_y + 206, checkbox_col_w, 22, true);
        place(st->hide_mame_chk, left_x + checkbox_col_w + 24, base_y + 128, checkbox_col_w, 22, true);
        place(st->hide_incorrect_chk, left_x + checkbox_col_w + 24, base_y + 154, checkbox_col_w, 22, true);
        place(st->generate_previews_chk, left_x + checkbox_col_w + 24, base_y + 180, checkbox_col_w, 22, true);
    }

    if (st->settings_open || !st->folder_filter_combo || !IsWindowVisible(st->folder_filter_combo))
        hide_folder_filter_list(st);

    if (st->cover_grid && IsWindowVisible(st->cover_grid))
        update_cover_scrollbar(st);
}

bool is_cover_ext(const fs::path& p) {
    auto ext = path_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
}

std::unordered_map<std::string, fs::path> index_covers(const fs::path& rom_dir) {
    std::unordered_map<std::string, fs::path> out;
    if (rom_dir.empty() || !fs::exists(rom_dir)) return out;
    for (const auto& entry : fs::recursive_directory_iterator(rom_dir, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        if (!is_cover_ext(entry.path())) continue;
        std::string stem = path_utf8(entry.path().stem());
        if (!stem.empty() && !out.count(stem))
            out[stem] = entry.path();
    }
    return out;
}

std::string summarize_rom_paths(const std::vector<fs::path>& rom_dirs) {
    if (rom_dirs.empty()) return "(none)";
    std::string out;
    for (size_t i = 0; i < rom_dirs.size(); ++i) {
        if (i) out += " | ";
        out += path_utf8(rom_dirs[i]);
    }
    return out;
}

std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string make_storage_key(const std::string& system, const std::string& short_name) {
    return make_storage_key_for_system(system, short_name);
}

std::string pick_sms_machine_for_title(const std::string& title) {
    std::string lower = to_lower_ascii(title);
    if (lower.find("korea") != std::string::npos)
        return "smskr";
    return "sms1";
}

bool is_cart_system(const std::string& system) {
    return is_cart_system_id(system);
}

HWND find_process_window_by_pid(DWORD pid, bool require_visible) {
    struct EnumData {
        DWORD pid = 0;
        HWND hwnd = nullptr;
        bool require_visible = true;
    } data { pid, nullptr, require_visible };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* data = reinterpret_cast<EnumData*>(lp);
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid != data->pid)
            return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;
        if (data->require_visible && !IsWindowVisible(hwnd))
            return TRUE;
        data->hwnd = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&data));

    return data.hwnd;
}

struct StartupKeypressPayload {
    DWORD pid = 0;
    DWORD delay_ms = 1500;
};

DWORD WINAPI startup_keypress_proc(LPVOID param) {
    std::unique_ptr<StartupKeypressPayload> payload(reinterpret_cast<StartupKeypressPayload*>(param));
    if (!payload)
        return 0;

    Sleep(payload->delay_ms);
    for (int i = 0; i < 20; ++i) {
        HWND hwnd = find_process_window_by_pid(payload->pid, false);
        if (hwnd) {
            PostMessageA(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
            Sleep(50);
            PostMessageA(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
            break;
        }
        Sleep(250);
    }
    return 0;
}

void queue_startup_return_keypress(DWORD pid, DWORD delay_ms = 1500) {
    auto payload = std::make_unique<StartupKeypressPayload>();
    payload->pid = pid;
    payload->delay_ms = delay_ms;
    HANDLE thread = CreateThread(nullptr, 0, startup_keypress_proc, payload.release(), 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

std::string sanitize_preview_component(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch))
            out.push_back((char)std::tolower(ch));
        else
            out.push_back('_');
    }
    if (out.empty())
        out = "__";
    return out;
}

fs::path preview_root_dir(const fs::path& exe_dir) {
    return exe_dir / "previews";
}

fs::path preview_rom_dir(const fs::path& exe_dir, const std::string& short_name) {
    std::string safe = sanitize_preview_component(short_name);
    std::string bucket = safe.substr(0, (std::min<size_t>)(2, safe.size()));
    if (bucket.size() < 2) bucket.append(2 - bucket.size(), '_');
    return preview_root_dir(exe_dir) / bucket / safe;
}

fs::path preview_status_path(const fs::path& exe_dir, const std::string& short_name) {
    return preview_rom_dir(exe_dir, short_name) / "status.txt";
}

bool load_preview_status(const fs::path& exe_dir, const std::string& short_name, AuditStatus& status, std::string& summary) {
    std::ifstream in(preview_status_path(exe_dir, short_name), std::ios::binary);
    if (!in.is_open())
        return false;
    auto trim_local = [](std::string s) {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };
    std::string line1;
    std::string line2;
    std::getline(in, line1);
    std::getline(in, line2);
    line1 = to_lower_ascii(trim_local(line1));
    summary = trim_local(line2);
    if (line1 == "ok")
        status = AUDIT_OK;
    else if (line1 == "broken")
        status = AUDIT_BROKEN;
    else
        return false;
    if (summary.empty())
        summary = (status == AUDIT_OK) ? "OK" : "Verification failed";
    return true;
}

void save_preview_status(const fs::path& exe_dir, const std::string& short_name, AuditStatus status, const std::string& summary) {
    fs::path path = preview_status_path(exe_dir, short_name);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << (status == AUDIT_OK ? "ok" : "broken") << "\n";
    out << summary << "\n";
}

std::vector<fs::path> load_preview_frames(const fs::path& exe_dir, const std::string& short_name) {
    std::vector<fs::path> out;
    fs::path dir = preview_rom_dir(exe_dir, short_name);
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return out;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = to_lower_ascii(path_utf8(entry.path().extension()));
        if (ext == ".jpg" || ext == ".jpeg")
            out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

const fs::path* current_visual_path(const LauncherState* st, int index) {
    if (!st || index < 0 || index >= (int)st->roms.size())
        return nullptr;
    const auto& rom = st->roms[index];
    if (!rom.preview_frames.empty())
        return &rom.preview_frames[(GetTickCount64() / ((index == st->selected_index) ? kSelectedPreviewPeriodMs : kBackgroundPreviewPeriodMs)) % rom.preview_frames.size()];
    if (!rom.cover_path.empty())
        return &rom.cover_path;
    return nullptr;
}

ULONGLONG preview_period_ms(bool selected) {
    return selected ? kSelectedPreviewPeriodMs : kBackgroundPreviewPeriodMs;
}

void draw_scaled_image(Graphics& g, Image& img, const Rect& rect) {
    if (img.GetLastStatus() != Ok) return;
    float iw = (float)img.GetWidth();
    float ih = (float)img.GetHeight();
    if (iw <= 0.0f || ih <= 0.0f) return;
    float sx = (float)rect.Width / iw;
    float sy = (float)rect.Height / ih;
    float scale = (sx < sy) ? sx : sy;
    int dw = (int)(iw * scale);
    int dh = (int)(ih * scale);
    int dx = rect.X + (rect.Width - dw) / 2;
    int dy = rect.Y + (rect.Height - dh) / 2;
    g.DrawImage(&img, dx, dy, dw, dh);
}

int get_encoder_clsid(const WCHAR* mime_type, CLSID* clsid) {
    if (!mime_type || !clsid) return -1;
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    std::vector<BYTE> buffer(size);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(num, size, encoders) != Ok)
        return -1;
    for (UINT i = 0; i < num; ++i) {
        if (encoders[i].MimeType && wcscmp(encoders[i].MimeType, mime_type) == 0) {
            *clsid = encoders[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

bool save_lowres_jpeg(const fs::path& src_path, const fs::path& dst_path) {
    Bitmap src(src_path.wstring().c_str());
    if (src.GetLastStatus() != Ok)
        return false;

    UINT src_w = src.GetWidth();
    UINT src_h = src.GetHeight();
    if (!src_w || !src_h)
        return false;

    double scale_x = (double)kPreviewMaxWidth / (double)src_w;
    double scale_y = (double)kPreviewMaxHeight / (double)src_h;
    double scale = (std::min)(1.0, (std::min)(scale_x, scale_y));
    UINT dst_w = (std::max<UINT>)(1, (UINT)(src_w * scale));
    UINT dst_h = (std::max<UINT>)(1, (UINT)(src_h * scale));

    Bitmap dst(dst_w, dst_h, PixelFormat24bppRGB);
    Graphics g(&dst);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.DrawImage(&src, 0, 0, dst_w, dst_h);

    CLSID jpeg_clsid{};
    if (get_encoder_clsid(L"image/jpeg", &jpeg_clsid) < 0)
        return false;

    EncoderParameters params{};
    params.Count = 1;
    params.Parameter[0].Guid = EncoderQuality;
    params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    ULONG quality = (ULONG)kPreviewJpegQuality;
    params.Parameter[0].Value = &quality;

    fs::create_directories(dst_path.parent_path());
    return dst.Save(dst_path.wstring().c_str(), &jpeg_clsid, &params) == Ok;
}

void clear_directory_files(const fs::path& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied))
        fs::remove_all(entry.path());
}

fs::path write_preview_lua_script(const fs::path& script_path) {
    fs::create_directories(script_path.parent_path());
    std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
    out <<
        "local machine = manager.machine\n"
        "local screen = machine.screens:at(1)\n"
        "if not screen then\n"
        "  machine:exit()\n"
        "  return\n"
        "end\n"
        "local captures = " << kPreviewFrameCount << "\n"
        "local next_second = " << (kPreviewSkipSeconds + kPreviewFrameSeconds) << "\n"
        "local captured = 0\n"
        "emu.register_frame_done(function()\n"
        "  local now = machine.time.seconds\n"
        "  while captured < captures and now >= next_second do\n"
        "    screen:snapshot(string.format(\"frame_%02d.png\", captured))\n"
        "    captured = captured + 1\n"
        "    next_second = next_second + " << kPreviewFrameSeconds << "\n"
        "  end\n"
        "  if captured >= captures then\n"
        "    machine:exit()\n"
        "  end\n"
        "end)\n";
    return script_path;
}

std::unordered_map<std::string, fs::path> index_covers_multi(const std::vector<fs::path>& rom_dirs) {
    std::unordered_map<std::string, fs::path> out;
    for (const auto& rom_dir : rom_dirs) {
        if (rom_dir.empty() || !fs::exists(rom_dir) || !fs::is_directory(rom_dir)) continue;
        auto covers = index_covers(rom_dir);
        for (auto& [name, path] : covers) {
            if (!out.count(name))
                out.emplace(std::move(name), std::move(path));
        }
    }
    return out;
}

std::string build_rompath(const Settings& settings) {
    std::string rompath = settings.bios_path;
    std::string joined_rom_paths = join_rom_paths(split_rom_paths(settings.roms_path));
    if (!joined_rom_paths.empty()) {
        if (!rompath.empty()) rompath += ";";
        rompath += joined_rom_paths;
    }
    return rompath;
}

std::string trim_copy(std::string s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string summarize_audit_failure(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;
        size_t pos = line.find(" NOT FOUND");
        if (pos != std::string::npos)
            return trim_copy(line.substr(0, pos)) + " missing";
        if (line.find("romset \"") != std::string::npos && line.find("not found") != std::string::npos)
            return line;
        if (line.find("Required files are missing") != std::string::npos)
            return "Required files are missing";
    }
    return "Verification failed";
}

bool only_missing_neogeo_bios_variants(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    bool saw_missing = false;
    while (std::getline(in, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;
        if (line.find(" NOT FOUND ") != std::string::npos || line.find(" - NOT FOUND") != std::string::npos) {
            saw_missing = true;
            if (line.find("(neogeo)") == std::string::npos)
                return false;
        }
    }
    return saw_missing;
}

PreviewResultMessage generate_preview_set(const Settings& settings, const fs::path& exe_dir, const RomEntry& rom, HANDLE cancel_event) {
    PreviewResultMessage result;
    result.short_name = rom.short_name;
    result.storage_key = rom.storage_key;
    result.audit_status = AUDIT_BROKEN;

    if (preview_cancelled(cancel_event)) {
        result.summary = "Cancelled";
        result.audit_summary = result.summary;
        return result;
    }

    if (settings.mame_exe.empty()) {
        result.summary = "MAME path is not configured";
        result.audit_summary = result.summary;
        return result;
    }
    if (rom.source_dir.empty()) {
        result.summary = "ROM source folder is unknown";
        result.audit_summary = result.summary;
        return result;
    }

    fs::path target_dir = preview_rom_dir(exe_dir, rom.storage_key);
    fs::path temp_dir = target_dir / "_tmp";
    fs::path script_path = target_dir / "capture_previews.lua";
    std::error_code ec;

    fs::create_directories(temp_dir, ec);
    clear_directory_files(temp_dir);
    if (fs::exists(target_dir) && fs::is_directory(target_dir)) {
        for (const auto& entry : fs::directory_iterator(target_dir, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = to_lower_ascii(path_utf8(entry.path().extension()));
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png")
                fs::remove(entry.path(), ec);
        }
    }

    write_preview_lua_script(script_path);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result.summary = "Failed to create preview pipe";
        result.audit_summary = result.summary;
        return result;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::string mame_game_arg;
    if (rom.system == "sms") {
        fs::path base = fs::absolute(fs::path(rom.source_dir));
        fs::path cart;
        for (const char* ext : {".zip", ".7z", ".sms"}) {
            fs::path candidate = base / (rom.short_name + ext);
            if (fs::exists(candidate)) { cart = candidate; break; }
        }
        if (cart.empty()) cart = base / (rom.short_name + ".7z");
        mame_game_arg = pick_sms_machine_for_title(rom.short_name) + " -cart \"" + cart.string() + "\"";
    } else if (rom.system == "nes") {
        fs::path base = fs::absolute(fs::path(rom.source_dir));
        fs::path cart;
        for (const char* ext : {".zip", ".7z", ".nes", ".unf", ".unif"}) {
            fs::path candidate = base / (rom.short_name + ext);
            if (fs::exists(candidate)) { cart = candidate; break; }
        }
        if (cart.empty()) cart = base / (rom.short_name + ".7z");
        mame_game_arg = "nes -cart \"" + cart.string() + "\"";
    } else if (rom.system == "gb") {
        fs::path base = fs::absolute(fs::path(rom.source_dir));
        fs::path cart;
        for (const char* ext : {".zip", ".7z", ".gb"}) {
            fs::path candidate = base / (rom.short_name + ext);
            if (fs::exists(candidate)) { cart = candidate; break; }
        }
        if (cart.empty()) cart = base / (rom.short_name + ".7z");
        mame_game_arg = "gameboy -cart \"" + cart.string() + "\"";
    } else if (rom.system == "gbc") {
        fs::path base = fs::absolute(fs::path(rom.source_dir));
        fs::path cart;
        for (const char* ext : {".zip", ".7z", ".gbc", ".cgb", ".gb"}) {
            fs::path candidate = base / (rom.short_name + ext);
            if (fs::exists(candidate)) { cart = candidate; break; }
        }
        if (cart.empty()) cart = base / (rom.short_name + ".7z");
        mame_game_arg = "gbcolor -cart \"" + cart.string() + "\"";
    } else if (rom.system == "snes") {
        // Pass the full cart path so MAME finds the file regardless of rompath.
        fs::path base = fs::absolute(fs::path(rom.source_dir));
        fs::path cart;
        for (const char* ext : {".zip", ".7z", ".sfc", ".smc"}) {
            fs::path candidate = base / (rom.short_name + ext);
            if (fs::exists(candidate)) { cart = candidate; break; }
        }
        if (cart.empty()) cart = base / (rom.short_name + ".7z");
        mame_game_arg = "snes -cart \"" + cart.string() + "\"";
    } else if (rom.system == "genesis") {
        // Resolve cart: prefer compressed carts first, then loose ROM dumps.
        fs::path base = fs::absolute(fs::path(rom.source_dir));
        fs::path cart;
        for (const char* ext : {".zip", ".7z", ".bin", ".md", ".gen"}) {
            fs::path candidate = base / (rom.short_name + ext);
            if (fs::exists(candidate)) { cart = candidate; break; }
        }
        if (cart.empty()) cart = base / (rom.short_name + ".7z"); // fallback
        mame_game_arg = "genesis -cart \"" + cart.string() + "\"";
    } else {
        mame_game_arg = rom.short_name;
    }
    std::string cmd = "\"" + settings.mame_exe + "\" " + mame_game_arg;
    std::string rompath = settings.bios_path;
    if (!rom.source_dir.empty()) {
        if (!rompath.empty()) rompath += ";";
        rompath += fs::absolute(rom.source_dir).string();
    }
    if (!rompath.empty())
        cmd += " -rompath \"" + rompath + "\"";
    if (!settings.mame_args.empty())
        cmd += " " + settings.mame_args;
    cmd += " -sound none";
    cmd += " -nothrottle";
    cmd += " -seconds_to_run " + std::to_string(kPreviewSkipSeconds + (kPreviewFrameCount * kPreviewFrameSeconds) + 5);
    cmd += " -snapshot_directory \"" + temp_dir.string() + "\"";
    cmd += " -autoboot_script \"" + script_path.string() + "\"";

    std::cout << "[preview] start rom=" << rom.short_name << "\n";
    std::cout << "[preview] cmd=" << cmd << "\n";

    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result.summary = "Failed to launch preview capture";
        result.audit_summary = result.summary;
        std::cout << "[preview] rom=" << rom.short_name << " failed_to_launch\n";
        return result;
    }

    CloseHandle(write_pipe);
    queue_startup_return_keypress(GetProcessId(pi.hProcess));

    std::string text;
    char buffer[4096];
    DWORD read = 0;
    bool cancelled = false;
    ULONGLONG start_ms = GetTickCount64();
    DWORD wait_result = WAIT_TIMEOUT;
    while (true) {
        DWORD available = 0;
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD chunk = (std::min<DWORD>)(available, (DWORD)sizeof(buffer));
            if (!ReadFile(read_pipe, buffer, chunk, &read, nullptr) || read == 0)
                break;
            text.append(buffer, buffer + read);
            available -= read;
        }

        if (preview_cancelled(cancel_event)) {
            cancelled = true;
            TerminateProcess(pi.hProcess, 1);
            wait_result = WaitForSingleObject(pi.hProcess, 5000);
            break;
        }

        wait_result = WaitForSingleObject(pi.hProcess, 50);
        if (wait_result == WAIT_OBJECT_0)
            break;

        if ((GetTickCount64() - start_ms) > 60000ULL) {
            wait_result = WAIT_TIMEOUT;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }

    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &read, nullptr) && read > 0) {
        DWORD chunk = (std::min<DWORD>)(read, (DWORD)sizeof(buffer));
        DWORD got = 0;
        if (!ReadFile(read_pipe, buffer, chunk, &got, nullptr) || got == 0)
            break;
        text.append(buffer, buffer + got);
    }

    if (wait_result == WAIT_TIMEOUT && !cancelled)
        TerminateProcess(pi.hProcess, 1);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);

    std::vector<fs::path> pngs;
    if (fs::exists(temp_dir) && fs::is_directory(temp_dir)) {
        for (const auto& entry : fs::directory_iterator(temp_dir, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            if (to_lower_ascii(entry.path().extension().string()) == ".png")
                pngs.push_back(entry.path());
        }
    }
    std::sort(pngs.begin(), pngs.end());

    int frame_count = 0;
    for (size_t i = 0; i < pngs.size() && !preview_cancelled(cancel_event); ++i) {
        char frame_name[32];
        sprintf_s(frame_name, "frame_%02u.jpg", (unsigned)i);
        if (save_lowres_jpeg(pngs[i], target_dir / frame_name))
            ++frame_count;
    }

    clear_directory_files(temp_dir);
    fs::remove_all(temp_dir, ec);
    fs::remove(script_path, ec);

    bool fatal_missing = text.find("NOT FOUND") != std::string::npos ||
        text.find("Required files are missing") != std::string::npos ||
        text.find("Fatal error") != std::string::npos;
    bool audit_ok = !cancelled && (wait_result != WAIT_TIMEOUT) && (exit_code == 0) && !fatal_missing;
    result.success = audit_ok && frame_count > 0;
    result.frame_count = frame_count;
    if (cancelled)
        result.summary = "Cancelled";
    else if (result.success)
        result.summary = std::to_string(frame_count) + " previews";
    else if (wait_result == WAIT_TIMEOUT)
        result.summary = "Preview capture timed out";
    else if (audit_ok)
        result.summary = "ROM OK, no previews";
    else if (frame_count > 0)
        result.summary = "Captured " + std::to_string(frame_count) + " previews";
    else
        result.summary = summarize_audit_failure(text);
    result.audit_status = audit_ok ? AUDIT_OK : AUDIT_BROKEN;
    result.audit_summary = audit_ok ? "OK" : summarize_audit_failure(text);

    std::cout << "[preview] rom=" << rom.short_name
              << " exit=" << exit_code
              << " success=" << (result.success ? "yes" : "no")
              << " frames=" << frame_count
              << " summary=" << result.summary << "\n";
    if (!text.empty())
        std::cout << "[preview] output:\n" << text << "\n";
    return result;
}

DWORD WINAPI preview_worker_proc(LPVOID param) {
    std::unique_ptr<PreviewWorkerPayload> payload(reinterpret_cast<PreviewWorkerPayload*>(param));
    if (!payload) return 0;

    for (const auto& rom : payload->roms) {
        if (preview_cancelled(payload->cancel_event))
            break;
        auto* msg = new PreviewResultMessage(generate_preview_set(payload->settings, payload->exe_dir, rom, payload->cancel_event));
        msg->generation = payload->generation;
        if (!preview_cancelled(payload->cancel_event))
            PostMessageW(payload->hwnd, WM_PREVIEW_RESULT, 0, reinterpret_cast<LPARAM>(msg));
        else
            delete msg;
    }

    auto* done = new PreviewResultMessage();
    done->generation = payload->generation;
    if (payload->hwnd)
        PostMessageW(payload->hwnd, WM_PREVIEW_DONE, 0, reinterpret_cast<LPARAM>(done));
    else
        delete done;
    return 0;
}

void start_preview_generation(LauncherState* st) {
    if (!st) return;
    if (st->preview_running) {
        set_status(st, "Preview generation is already running.");
        return;
    }
    if (st->roms.empty()) {
        set_status(st, "No ROMs found in folders.");
        return;
    }

    std::vector<RomEntry> queue;
    queue.reserve(st->roms.size());
    for (const auto& rom : st->roms) {
        if (rom.audit_status == AUDIT_PENDING)
            queue.push_back(rom);
    }
    if (queue.empty()) {
        set_status(st, "All ROMs are already processed.");
        return;
    }

    ++st->preview_generation;
    st->preview_total = (int)queue.size();
    st->preview_completed = 0;
    st->preview_success = 0;
    st->preview_running = true;
    for (auto& rom : st->roms) {
        if (rom.audit_status == AUDIT_PENDING) {
            rom.audit_summary = "Generating previews...";
            rom.hidden = false;
        }
    }
    apply_visibility_filter(st);

    auto payload = std::make_unique<PreviewWorkerPayload>();
    payload->hwnd = GetParent(st->cover_grid);
    payload->generation = st->preview_generation;
    payload->cancel_event = nullptr;
    payload->settings = st->settings;
    payload->exe_dir = st->exe_dir;
    payload->roms = std::move(queue);

    if (st->preview_thread) {
        CloseHandle(st->preview_thread);
        st->preview_thread = nullptr;
    }
    if (st->preview_cancel_event) {
        CloseHandle(st->preview_cancel_event);
        st->preview_cancel_event = nullptr;
    }
    st->preview_cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    payload->cancel_event = st->preview_cancel_event;

    HANDLE thread = CreateThread(nullptr, 0, preview_worker_proc, payload.release(), 0, nullptr);
    if (thread) {
        st->preview_thread = thread;
        set_status(st, "Generating previews... 0/" + std::to_string(st->preview_total));
    } else {
        st->preview_running = false;
        if (st->preview_cancel_event) {
            CloseHandle(st->preview_cancel_event);
            st->preview_cancel_event = nullptr;
        }
        set_status(st, "Failed to start preview generation.");
    }
}

void apply_preview_result(LauncherState* st, const PreviewResultMessage& msg) {
    if (!st || msg.generation != st->preview_generation) return;
    for (auto& rom : st->roms) {
        if (rom.storage_key != msg.storage_key) continue;
        st->preview_image_cache.clear();
        rom.preview_frames = load_preview_frames(st->exe_dir, rom.storage_key);
        rom.audit_status = msg.audit_status;
        rom.audit_summary = msg.audit_summary;
        save_preview_status(st->exe_dir, rom.storage_key, rom.audit_status, rom.audit_summary);
        ++st->preview_completed;
        if (msg.success)
            ++st->preview_success;
        set_status(st, "Generating previews... " + std::to_string(st->preview_completed) +
            "/" + std::to_string(st->preview_total) +
            " (" + std::to_string(st->preview_success) + " ready)");
        apply_visibility_filter(st);
        if (st->cover_grid)
            InvalidateRect(st->cover_grid, nullptr, FALSE);
        break;
    }
}

void select_index(LauncherState* st, int index) {
    if (!st) return;
    if (index < 0 || index >= (int)st->roms.size()) return;
    st->selected_index = index;
    if (st->cover_grid) InvalidateRect(st->cover_grid, nullptr, FALSE);
    invalidate_action_buttons(st);
}

int cover_index_from_point(LauncherState* st, int x, int y) {
    if (!st || !st->cover_grid) return -1;
    RECT rc{};
    GetClientRect(st->cover_grid, &rc);
    int cols = (int)((rc.right - kCoverPad) / (kCoverTileW + kCoverPad));
    if (cols < 1) cols = 1;
    y += st->scroll_y;
    int col = (x - kCoverPad) / (kCoverTileW + kCoverPad);
    int row = (y - kCoverPad) / (kCoverTileH + kCoverPad);
    if (x < kCoverPad || y < kCoverPad || col < 0 || row < 0 || col >= cols) return -1;
    int tile_x = kCoverPad + col * (kCoverTileW + kCoverPad);
    int tile_y = kCoverPad + row * (kCoverTileH + kCoverPad);
    if (x > tile_x + kCoverTileW || y > tile_y + kCoverTileH) return -1;
    auto visible = visible_rom_indices(st);
    int idx = row * cols + col;
    return idx < (int)visible.size() ? visible[idx] : -1;
}

void draw_cover_grid(LauncherState* st, HDC hdc) {
    RECT rc{};
    GetClientRect(st->cover_grid, &rc);
    RECT clip_rc{};
    GetClipBox(hdc, &clip_rc);
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeNone);
    g.SetInterpolationMode(InterpolationModeBilinear);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    SolidBrush bg(Color(255, 18, 20, 24));
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    int cols = (int)((rc.right - kCoverPad) / (kCoverTileW + kCoverPad));
    if (cols < 1) cols = 1;

    FontFamily ff(L"Segoe UI");
    Font title_font(&ff, 13.0f, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentNear);

    auto visible = visible_rom_indices(st);
    for (int vis_i = 0; vis_i < (int)visible.size(); ++vis_i) {
        int i = visible[vis_i];
        int row = vis_i / cols;
        int col = vis_i % cols;
        int x = kCoverPad + col * (kCoverTileW + kCoverPad);
        int y = kCoverPad + row * (kCoverTileH + kCoverPad) - st->scroll_y;
        if (y + kCoverTileH < 0 || y > rc.bottom) continue;
        RECT tile_rc{ x, y, x + kCoverTileW, y + kCoverTileH };
        RECT ignored{};
        if (!IntersectRect(&ignored, &tile_rc, &clip_rc))
            continue;

        bool sel = (i == st->selected_index);
        bool broken = st->roms[i].audit_status == AUDIT_BROKEN;
        Color frame = broken
            ? (sel ? Color(255, 255, 130, 110) : Color(255, 190, 72, 64))
            : (sel ? Color(255, 255, 196, 64) : Color(255, 64, 70, 80));
        Color fill = broken
            ? (sel ? Color(255, 72, 30, 28) : Color(255, 52, 22, 22))
            : (sel ? Color(255, 44, 50, 58) : Color(255, 30, 34, 40));
        SolidBrush fill_br(fill);
        Pen pen(frame, sel ? 3.0f : 1.0f);
        g.FillRectangle(&fill_br, x, y, kCoverTileW, kCoverTileH);
        g.DrawRectangle(&pen, x, y, kCoverTileW, kCoverTileH);

        Rect img_rect(x + 8, y + 8, kCoverTileW - 16, kCoverImageH);
        if (const fs::path* visual_path = current_visual_path(st, i)) {
            if (Image* img = cached_preview_image(st, *visual_path))
                draw_scaled_image(g, *img, img_rect);
        } else {
            SolidBrush ph(Color(255, 52, 58, 66));
            Pen php(Color(255, 88, 96, 108), 1.0f);
            g.FillRectangle(&ph, img_rect);
            g.DrawRectangle(&php, img_rect);
            Font ph_font(&ff, 22.0f, FontStyleBold, UnitPixel);
            SolidBrush txt_br(Color(255, 220, 220, 220));
            std::wstring label = widen(st->roms[i].short_name);
            RectF tf((REAL)img_rect.X, (REAL)img_rect.Y + 48.0f, (REAL)img_rect.Width, 50.0f);
            g.DrawString(label.c_str(), -1, &ph_font, tf, &sf, &txt_br);
        }

        SolidBrush txt_br(broken ? Color(255, 255, 210, 210) : Color(255, 240, 240, 240));
        std::wstring title = widen(st->roms[i].title);
        RectF tr((REAL)(x + 10), (REAL)(y + kCoverImageH + 12), (REAL)(kCoverTileW - 20), 34.0f);
        g.DrawString(title.c_str(), -1, &title_font, tr, &sf, &txt_br);
    }
}

HFONT create_launcher_title_font(HWND hwnd) {
    UINT dpi = 96;
    if (hwnd)
        dpi = GetDpiForWindow(hwnd);
    return CreateFontW(
        -MulDiv(30, dpi, 72),
        0, 0, 0,
        FW_HEAVY,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        VARIABLE_PITCH,
        L"Segoe UI");
}

void draw_launch_vr_button(LauncherState* st, const DRAWITEMSTRUCT* dis) {
    if (!dis) return;

    const bool has_selection = st && st->selected_index >= 0 &&
        st->selected_index < (int)st->roms.size() && !st->roms[st->selected_index].hidden;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    COLORREF bg = has_selection ? RGB(180, 238, 180) : RGB(90, 90, 90);
    COLORREF border = has_selection ? RGB(92, 160, 92) : RGB(60, 60, 60);
    COLORREF text = has_selection ? RGB(24, 56, 24) : RGB(225, 225, 225);
    if (pressed)
        bg = has_selection ? RGB(156, 220, 156) : RGB(74, 74, 74);

    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_pen = SelectObject(dis->hDC, pen);
    HGDIOBJ old_brush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, old_brush);
    SelectObject(dis->hDC, old_pen);
    DeleteObject(pen);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    RECT text_rc = dis->rcItem;
    DrawTextW(dis->hDC, L"Launch In VR", -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (focused) {
        RECT focus = dis->rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(dis->hDC, &focus);
    }
}

void paint_launcher_background(HWND hwnd, HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH brush = CreateSolidBrush(RGB(18, 20, 24));
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);
}

LRESULT CALLBACK cover_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (st) draw_cover_grid(st, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (st) {
            update_cover_scrollbar(st);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (st) {
            int mx = (short)LOWORD(lp);
            int my = (short)HIWORD(lp);
            int idx = cover_index_from_point(st, mx, my);
            if (idx >= 0) select_index(st, idx);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (st) {
            int mx = (short)LOWORD(lp);
            int my = (short)HIWORD(lp);
            int idx = cover_index_from_point(st, mx, my);
            if (idx >= 0) {
                select_index(st, idx);
                bool dyn = st->dynamic_chk   && SendMessageW(st->dynamic_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool bet = st->beta_chk      && SendMessageW(st->beta_chk,      BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool den = st->density_chk   && SendMessageW(st->density_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool mot = st->motion_chk    && SendMessageW(st->motion_chk,    BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool hid = st->hide_mame_chk && SendMessageW(st->hide_mame_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (launch_mode(st, false, false, false, dyn, bet, den, mot, hid)) PostQuitMessage(0);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (st) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int cols = (int)((rc.right - kCoverPad) / (kCoverTileW + kCoverPad));
            if (cols < 1) cols = 1;
            auto visible = visible_rom_indices(st);
            int rows = (int)((visible.size() + cols - 1) / cols);
            int content_h = kCoverPad + rows * (kCoverTileH + kCoverPad);
            int max_scroll = content_h > rc.bottom ? (content_h - rc.bottom) : 0;
            int delta = ((short)HIWORD(wp) > 0) ? -80 : 80;
            st->scroll_y += delta;
            if (st->scroll_y < 0) st->scroll_y = 0;
            if (st->scroll_y > max_scroll) st->scroll_y = max_scroll;
            SetScrollPos(hwnd, SB_VERT, st->scroll_y, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_VSCROLL:
        if (st) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int cols = (int)((rc.right - kCoverPad) / (kCoverTileW + kCoverPad));
            if (cols < 1) cols = 1;
            auto visible = visible_rom_indices(st);
            int rows = (int)((visible.size() + cols - 1) / cols);
            int content_h = kCoverPad + rows * (kCoverTileH + kCoverPad);
            int max_scroll = content_h > rc.bottom ? (content_h - rc.bottom) : 0;
            switch (LOWORD(wp)) {
            case SB_LINEUP: st->scroll_y -= 40; break;
            case SB_LINEDOWN: st->scroll_y += 40; break;
            case SB_PAGEUP: st->scroll_y -= rc.bottom / 2; break;
            case SB_PAGEDOWN: st->scroll_y += rc.bottom / 2; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: st->scroll_y = HIWORD(wp); break;
            default: break;
            }
            if (st->scroll_y < 0) st->scroll_y = 0;
            if (st->scroll_y > max_scroll) st->scroll_y = max_scroll;
            SetScrollPos(hwnd, SB_VERT, st->scroll_y, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void refresh_roms(LauncherState* st) {
    st->roms.clear();
    st->preview_image_cache.clear();
    auto rom_dirs = split_rom_paths(st->settings.roms_path);
    bool has_valid_rom_dir = false;
    for (const auto& rom_dir : rom_dirs) {
        if (fs::exists(rom_dir) && fs::is_directory(rom_dir)) {
            has_valid_rom_dir = true;
            break;
        }
    }
    if (!has_valid_rom_dir) {
        set_status(st, "ROM folder not found.");
        return;
    }

    auto covers = index_covers_multi(rom_dirs);
    fs::path repo_root = find_repo_root(st->exe_path);
    auto cps_set     = load_cps_rom_sets(repo_root);
    auto konami_set  = load_konami_rom_set(repo_root);
    std::unordered_map<std::string, bool> seen_roms;
    std::unordered_map<std::string, FolderScanStats> folder_stats;

    // NeoGeo ROM names are those found in the neogeo title_map from neogeo.cpp
    auto neogeo_map = load_title_map(repo_root);
    auto stats_for_dir = [&](const fs::path& source_dir) -> FolderScanStats& {
        const std::string key = normalized_folder_key(source_dir);
        auto& stats = folder_stats[key];
        if (stats.label.empty())
            stats.label = folder_filter_label_for_path(rom_dirs, source_dir);
        return stats;
    };

    for (const auto& rom_dir : rom_dirs) {
        if (!fs::exists(rom_dir) || !fs::is_directory(rom_dir)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(rom_dir, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto& dir_stats = stats_for_dir(entry.path().parent_path());
            dir_stats.files_found++;
            auto ext = path_utf8(entry.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            bool is_sms     = (ext == ".sms");
            bool is_nes     = (ext == ".nes" || ext == ".unf" || ext == ".unif");
            bool is_gb      = (ext == ".gb");
            bool is_gbc     = (ext == ".gbc" || ext == ".cgb");
            bool is_snes    = (ext == ".sfc" || ext == ".smc");
            bool is_genesis = (ext == ".bin" || ext == ".md"  || ext == ".gen");
            bool is_archive = (ext == ".zip" || ext == ".7z");
            if (!is_sms && !is_nes && !is_gb && !is_gbc && !is_snes && !is_genesis && !is_archive) continue;
            dir_stats.rom_files_detected++;
            std::string short_name = path_utf8(entry.path().stem());
            if (short_name == "neogeo") {
                dir_stats.roms_discarded_special++;
                continue;
            }

            RomEntry rom;
            rom.short_name = short_name;
            rom.source_dir = entry.path().parent_path();
            rom.folder_key = normalized_folder_key(rom.source_dir);
            rom.folder_label = folder_filter_label_for_path(rom_dirs, rom.source_dir);
            auto it = st->title_map.find(short_name);
            rom.title = (it != st->title_map.end()) ? it->second : short_name;
            auto ci = covers.find(short_name);
            if (ci != covers.end()) rom.cover_path = ci->second;
            // Classify by folder name first — no database lookup needed for named-system folders.
            // Walk path components looking for a recognized system folder name.
            if (is_sms && is_system_enabled("sms")) {
                rom.system = "sms";
            } else if (is_nes && is_system_enabled("nes")) {
                rom.system = "nes";
            } else if (is_gbc && is_system_enabled("gbc")) {
                rom.system = "gbc";
            } else if (is_gb && is_system_enabled("gb")) {
                rom.system = "gb";
            } else if (is_snes && is_system_enabled("snes")) {
                rom.system = "snes";
            } else if (is_genesis && is_system_enabled("genesis")) {
                rom.system = "genesis";
            } else {
                for (auto pit = entry.path().begin(); pit != entry.path().end(); ++pit) {
                    auto comp = pit->string();
                    std::transform(comp.begin(), comp.end(), comp.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                    if ((comp == "sms" || comp == "mastersystem" || comp == "master system") && is_system_enabled("sms")) {
                        rom.system = "sms"; break;
                    }
                    if ((comp == "nes" || comp == "famicom" || comp == "nintendo entertainment system") && is_system_enabled("nes")) {
                        rom.system = "nes"; break;
                    }
                    if ((comp == "gbc" || comp == "gbcolor" || comp == "gameboy color" || comp == "game boy color")
                        && is_system_enabled("gbc")) {
                        rom.system = "gbc"; break;
                    }
                    if ((comp == "gb" || comp == "gameboy" || comp == "game boy")
                        && is_system_enabled("gb")) {
                        rom.system = "gb"; break;
                    }
                    if (comp == "snes" && is_system_enabled("snes"))   { rom.system = "snes";   break; }
                    if ((comp == "genesis" || comp == "megadrive" || comp == "mega drive" || comp == "sega genesis")
                        && is_system_enabled("genesis")) {
                        rom.system = "genesis"; break;
                    }
                    if (comp == "konami") { rom.system = "konami"; break; }
                    if (comp == "cps" || comp == "cps1" || comp == "cps2") {
                        // refine cps1 vs cps2 from database if possible
                        auto cps_it = cps_set.find(short_name);
                        rom.system = (cps_it != cps_set.end() && cps_it->second == 1) ? "cps1" : "cps2";
                        break;
                    }
                    if (comp == "neogeo" || comp == "neo geo" || comp == "neo-geo") {
                        rom.system = "neogeo"; break;
                    }
                }
                // Folder didn't match — fall back to database lookup.
                if (rom.system.empty()) {
                    auto cps_it = cps_set.find(short_name);
                    if (cps_it != cps_set.end())
                        rom.system = (cps_it->second == 2) ? "cps2" : "cps1";
                    else if (konami_set.count(short_name))
                        rom.system = "konami";
                    else if (neogeo_map.count(short_name))
                        rom.system = "neogeo";
                    else {
                        dir_stats.roms_discarded_unknown++;
                        continue; // unknown system, skip
                    }
                }
            }
            rom.storage_key = make_storage_key(rom.system, rom.short_name);
            if (seen_roms.count(rom.storage_key)) {
                dir_stats.roms_discarded_duplicate++;
                continue;
            }

            rom.preview_frames = load_preview_frames(st->exe_dir, rom.storage_key);
            if (!rom.preview_frames.empty()) {
                rom.audit_status = AUDIT_OK;
                rom.audit_summary = "OK";
            } else {
                AuditStatus cached_status = AUDIT_PENDING;
                std::string cached_summary;
                if (load_preview_status(st->exe_dir, rom.storage_key, cached_status, cached_summary)) {
                    if (cached_status == AUDIT_BROKEN && is_cart_system(rom.system) && cached_summary.empty()) {
                        cached_status = AUDIT_PENDING;
                        cached_summary = "Preview generation pending";
                    }
                    rom.audit_status = cached_status;
                    rom.audit_summary = cached_summary;
                }
            }

            seen_roms[rom.storage_key] = true;
            dir_stats.roms_accepted++;
            st->roms.push_back(std::move(rom));
        }
    }

    {
        std::vector<const FolderScanStats*> stats_rows;
        stats_rows.reserve(folder_stats.size());
        int total_files = 0;
        int total_rom_files = 0;
        int total_accepted = 0;
        int total_discarded_unknown = 0;
        int total_discarded_duplicate = 0;
        int total_discarded_special = 0;
        for (const auto& kv : folder_stats) {
            const FolderScanStats& stats = kv.second;
            if (stats.rom_files_detected <= 0 && stats.roms_accepted <= 0 &&
                stats.roms_discarded_unknown <= 0 && stats.roms_discarded_duplicate <= 0 &&
                stats.roms_discarded_special <= 0)
                continue;
            stats_rows.push_back(&stats);
            total_files += stats.files_found;
            total_rom_files += stats.rom_files_detected;
            total_accepted += stats.roms_accepted;
            total_discarded_unknown += stats.roms_discarded_unknown;
            total_discarded_duplicate += stats.roms_discarded_duplicate;
            total_discarded_special += stats.roms_discarded_special;
        }
        std::sort(stats_rows.begin(), stats_rows.end(), [](const FolderScanStats* a, const FolderScanStats* b) {
            return _stricmp(a->label.c_str(), b->label.c_str()) < 0;
        });
        std::cout << "[launcher] folder scan summary (" << stats_rows.size() << " folders)\n";
        for (const FolderScanStats* stats : stats_rows) {
            const int discarded = stats->roms_discarded_unknown + stats->roms_discarded_duplicate + stats->roms_discarded_special;
            std::cout << "[launcher]   " << stats->label
                      << " | files=" << stats->files_found
                      << " | rom-files=" << stats->rom_files_detected
                      << " | accepted=" << stats->roms_accepted
                      << " | discarded=" << discarded;
            if (discarded > 0) {
                std::cout << " (unknown=" << stats->roms_discarded_unknown
                          << ", duplicate=" << stats->roms_discarded_duplicate
                          << ", special=" << stats->roms_discarded_special << ")";
            }
            std::cout << "\n";
        }
        std::cout << "[launcher] totals"
                  << " | files=" << total_files
                  << " | rom-files=" << total_rom_files
                  << " | accepted=" << total_accepted
                  << " | discarded=" << (total_discarded_unknown + total_discarded_duplicate + total_discarded_special)
                  << " (unknown=" << total_discarded_unknown
                  << ", duplicate=" << total_discarded_duplicate
                  << ", special=" << total_discarded_special << ")\n";
    }

    std::sort(st->roms.begin(), st->roms.end(), [](const RomEntry& a, const RomEntry& b) {
        return _stricmp(a.title.c_str(), b.title.c_str()) < 0;
    });

    rebuild_folder_filter_combo(st);
    st->scroll_y = 0;
    update_cover_scrollbar(st);
    if (!st->roms.empty()) select_index(st, 0);
    apply_visibility_filter(st);
    if (st->roms.empty()) {
        set_status(st, "No ROMs found in folders.");
    } else {
        int pending = 0;
        for (const auto& rom : st->roms)
            if (rom.audit_status == AUDIT_PENDING) ++pending;
        set_status(st, pending > 0
            ? (std::to_string(pending) + " ROMs pending preview generation.")
            : "All ROMs are already processed.");
        if (generate_previews_enabled(st))
            start_preview_generation(st);
    }
}

void sync_path_labels(LauncherState* st) {
    set_text(st->roms_path, "ROMs: " + summarize_rom_paths(split_rom_paths(st->settings.roms_path)));
    set_text(st->bios_path, "BIOS: " + st->settings.bios_path);
}

bool launch_mode(LauncherState* st, bool preview, bool spectator, bool inspect, bool dynamic, bool beta, bool density, bool motion, bool hide_mame) {
    int sel = st->selected_index;
    if (sel < 0 || sel >= (int)st->roms.size()) {
        set_status(st, "Pick a ROM first.");
        return false;
    }

    // Prepare bios/{gamename}/ with loose board ROM files so MAME finds them
    if (!st->settings.bios_path.empty()) {
        fs::path bios_dir = st->settings.bios_path;
        fs::path game_dir = bios_dir / st->roms[sel].short_name;
        if (!fs::exists(game_dir)) {
            bool has_loose = false;
            for (const auto& e : fs::directory_iterator(bios_dir,
                    fs::directory_options::skip_permission_denied)) {
                if (!e.is_regular_file()) continue;
                auto ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".zip") { has_loose = true; break; }
            }
            if (has_loose) {
                fs::create_directories(game_dir);
                for (const auto& e : fs::directory_iterator(bios_dir,
                        fs::directory_options::skip_permission_denied)) {
                    if (!e.is_regular_file()) continue;
                    auto ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".zip") continue;
                    fs::copy_file(e.path(), game_dir / e.path().filename(),
                                  fs::copy_options::skip_existing);
                }
            }
        }
    }

    fs::path config_path = st->exe_dir / "configs" / (st->roms[sel].storage_key + ".json");
    std::string cmd = "\"" + st->exe_path.string() + "\" --game \"" + st->roms[sel].short_name + "\"";
    cmd += " --config \"" + config_path.string() + "\"";
    if (!st->roms[sel].system.empty())
        cmd += " --system " + st->roms[sel].system;
    if (!st->roms[sel].source_dir.empty())
        cmd += " --romdir \"" + fs::absolute(st->roms[sel].source_dir).string() + "\"";
    if (preview) cmd += " --preview";
    if (spectator) cmd += " --spectator";
    if (inspect) cmd += " --inspect";
    if (dynamic) cmd += " --dynamic";
    if (beta)      cmd += " --beta-depth";
    if (density)   cmd += " --density";
    if (motion)    cmd += " --motion";
    if (hide_mame) cmd += " --hide-mame";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, st->exe_dir.string().c_str(), &si, &pi)) {
        set_status(st, "Failed to launch RetroDepth child process.");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct ReadmeWndData { HFONT font; HBRUSH bg_brush; };

static LRESULT CALLBACK readme_wnd_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<ReadmeWndData*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE: {
        HWND edit = GetDlgItem(hw, 1);
        if (edit) MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        if (d) {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(0, 0, 0));
            return (LRESULT)d->bg_brush;
        }
        break;
    case WM_DESTROY:
        if (d) {
            if (d->font)     DeleteObject(d->font);
            if (d->bg_brush) DeleteObject(d->bg_brush);
            delete d;
        }
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void show_readme_window(HWND parent, const fs::path& txt_path) {
    std::ifstream f(txt_path);
    if (!f.is_open()) {
        MessageBoxW(parent, L"readme.txt not found next to the executable.", L"README", MB_OK | MB_ICONWARNING);
        return;
    }
    // Read and normalise line endings to \r\n for the EDIT control
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string content;
    content.reserve(raw.size() + 256);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') continue;          // strip bare \r
        if (raw[i] == '\n') content += '\r';   // ensure \r before every \n
        content += raw[i];
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, nullptr, 0);
    std::wstring wtext(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, wtext.data(), wlen);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = readme_wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RetroDepthReadme";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    RegisterClassExW(&wc);  // no-op if already registered

    HWND hw = CreateWindowExW(0, L"RetroDepthReadme", L"README",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 560,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    auto* d      = new ReadmeWndData{};
    d->font      = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Consolas");
    d->bg_brush  = CreateSolidBrush(RGB(0, 0, 0));
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)d);

    HWND edit = CreateWindowExW(0, L"EDIT", wtext.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 720, 560, hw, (HMENU)1, GetModuleHandleW(nullptr), nullptr);

    SendMessageW(edit, WM_SETFONT, (WPARAM)d->font, TRUE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<LauncherState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        st->title_bg_brush = CreateSolidBrush(RGB(0, 0, 0));

        st->title = CreateWindowW(L"STATIC", L"RetroDepth Launcher", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 560, kTitleH, hwnd, nullptr, nullptr, nullptr);
        st->title_font = create_launcher_title_font(hwnd);
        if (st->title && st->title_font)
            SendMessageW(st->title, WM_SETFONT, (WPARAM)st->title_font, TRUE);
        st->settings_btn = CreateWindowW(L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 110, kLaunchH, hwnd, (HMENU)IDC_SETTINGS, nullptr, nullptr);
        st->roms_path = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            16, 66, 700, 20, hwnd, (HMENU)IDC_ROMS_PATH, nullptr, nullptr);
        st->bios_path = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            16, 90, 700, 20, hwnd, (HMENU)IDC_BIOS_PATH, nullptr, nullptr);
        st->folder_filter_label = CreateWindowW(L"STATIC", L"Folder:", WS_CHILD | WS_VISIBLE,
            16, 114, 52, 22, hwnd, nullptr, nullptr, nullptr);
        st->folder_filter_combo = CreateWindowW(L"BUTTON", L"All folders  v", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            72, 110, 640, kButtonH, hwnd, (HMENU)IDC_FOLDER_FILTER, nullptr, nullptr);
        st->pick_roms_btn = CreateWindowW(L"BUTTON", L"Select ROM Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            730, 62, 170, 28, hwnd, (HMENU)IDC_PICK_ROMS, nullptr, nullptr);
        st->pick_bios_btn = CreateWindowW(L"BUTTON", L"Select BIOS Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            730, 86, 170, 28, hwnd, (HMENU)IDC_PICK_BIOS, nullptr, nullptr);
        st->refresh_btn = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            730, 120, 140, 28, hwnd, (HMENU)IDC_REFRESH, nullptr, nullptr);
        st->readme_btn = CreateWindowW(L"BUTTON", L"README", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            730, 150, 140, 28, hwnd, (HMENU)IDC_README, nullptr, nullptr);

        st->cover_grid = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_BORDER | SS_NOTIFY | WS_VSCROLL,
            16, 126, 700, 460, hwnd, (HMENU)IDC_COVER_GRID, nullptr, nullptr);
        SetWindowLongPtrW(st->cover_grid, GWLP_USERDATA, (LONG_PTR)st);
        SetWindowLongPtrW(st->cover_grid, GWLP_WNDPROC, (LONG_PTR)cover_wnd_proc);
        st->launch_prev_btn = CreateWindowW(L"BUTTON", L"2D Preview", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            730, 160, 180, 28, hwnd, (HMENU)IDC_LAUNCH_PREV, nullptr, nullptr);
        st->launch_vr_btn = CreateWindowW(L"BUTTON", L"Launch In VR", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
            730, 220, 146, kLaunchH, hwnd, (HMENU)IDC_LAUNCH_VR, nullptr, nullptr);
        st->launch_vr_view_btn = CreateWindowW(L"BUTTON", L"Launch VR + View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            730, 280, 170, kLaunchH, hwnd, (HMENU)IDC_LAUNCH_VR_VIEW, nullptr, nullptr);
        st->dynamic_chk = CreateWindowW(L"BUTTON", L"Dynamic Depth", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 376, 150, 22, hwnd, (HMENU)IDC_DYNAMIC_DEPTH, nullptr, nullptr);
        st->beta_chk = CreateWindowW(L"BUTTON", L"Beta Depth", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 400, 150, 22, hwnd, (HMENU)IDC_BETA_DEPTH, nullptr, nullptr);
        st->density_chk = CreateWindowW(L"BUTTON", L"  Density scoring", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 424, 150, 22, hwnd, nullptr, nullptr, nullptr);
        st->motion_chk = CreateWindowW(L"BUTTON", L"  Motion scoring", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 448, 150, 22, hwnd, nullptr, nullptr, nullptr);
        st->hide_mame_chk = CreateWindowW(L"BUTTON", L"Hide MAME window", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 472, 150, 22, hwnd, nullptr, nullptr, nullptr);
        st->hide_incorrect_chk = CreateWindowW(L"BUTTON", L"Hide incorrect ROMs", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 496, 150, 22, hwnd, (HMENU)IDC_HIDE_INCORRECT, nullptr, nullptr);
        st->generate_previews_chk = CreateWindowW(L"BUTTON", L"Generate previews", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            730, 520, 150, 22, hwnd, (HMENU)IDC_GENERATE_PREVIEWS, nullptr, nullptr);
        st->folder_filter_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_CLIPSIBLINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_FOLDER_FILTER_LIST, nullptr, nullptr);
        ShowWindow(st->folder_filter_list, SW_HIDE);
        SendMessageW(st->hide_mame_chk, BM_SETCHECK, BST_CHECKED, 0); // on by default
        SendMessageW(st->hide_incorrect_chk, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(st->generate_previews_chk, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(st->dynamic_chk, BM_SETCHECK, BST_CHECKED, 0);
        st->status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            16, 572, 860, 20, hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);

        sync_path_labels(st);
        refresh_roms(st);
        layout_launcher(st, hwnd);
        SetTimer(hwnd, kPreviewTimerId, kPreviewTimerMs, nullptr);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
        if (st) {
            HDC hdc = (HDC)wp;
            HWND ctl = (HWND)lp;
            if (ctl == st->title) {
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkColor(hdc, RGB(0, 0, 0));
                return (INT_PTR)(st->title_bg_brush ? st->title_bg_brush : GetStockObject(BLACK_BRUSH));
            }
            SetTextColor(hdc, RGB(228, 232, 238));
            SetBkMode(hdc, TRANSPARENT);
            return (INT_PTR)GetStockObject(NULL_BRUSH);
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_launcher_background(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (st) layout_launcher(st, hwnd);
        return 0;
    case WM_PREVIEW_RESULT: {
        std::unique_ptr<PreviewResultMessage> msg(reinterpret_cast<PreviewResultMessage*>(lp));
        if (st && msg)
            apply_preview_result(st, *msg);
        return 0;
    }
    case WM_PREVIEW_DONE: {
        std::unique_ptr<PreviewResultMessage> msg(reinterpret_cast<PreviewResultMessage*>(lp));
        if (st && msg && msg->generation == st->preview_generation) {
            st->preview_running = false;
            set_status(st, "Preview generation complete: " + std::to_string(st->preview_success) +
                " ready of " + std::to_string(st->preview_total) + " ROMs.");
            if (st->preview_thread) {
                CloseHandle(st->preview_thread);
                st->preview_thread = nullptr;
            }
            if (st->preview_cancel_event) {
                CloseHandle(st->preview_cancel_event);
                st->preview_cancel_event = nullptr;
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (st && wp == kPreviewTimerId && st->cover_grid) {
            invalidate_preview_tiles(st);
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (st && dis && dis->CtlID == IDC_LAUNCH_VR) {
            draw_launch_vr_button(st, dis);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case IDC_PICK_ROMS: {
            auto path = choose_folder(hwnd, L"Select ROM folder");
            if (!path.empty()) {
                auto rom_dirs = split_rom_paths(st->settings.roms_path);
                fs::path selected = narrow(path);
                if (std::find(rom_dirs.begin(), rom_dirs.end(), selected) == rom_dirs.end())
                    rom_dirs.push_back(std::move(selected));
                st->settings.roms_path = join_rom_paths(rom_dirs);
                save_settings(st->exe_dir, st->settings);
                sync_path_labels(st);
                refresh_roms(st);
            }
            return 0;
        }
        case IDC_PICK_BIOS: {
            auto path = choose_folder(hwnd, L"Select BIOS folder");
            if (!path.empty()) {
                st->settings.bios_path = narrow(path);
                save_settings(st->exe_dir, st->settings);
                sync_path_labels(st);
            }
            return 0;
        }
        case IDC_REFRESH:
            refresh_roms(st);
            return 0;
        case IDC_README:
            show_readme_window(hwnd, st->exe_dir / "readme.txt");
            return 0;
        case IDC_SETTINGS:
            st->settings_open = !st->settings_open;
            layout_launcher(st, hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case IDC_FOLDER_FILTER:
            if (HIWORD(wp) == BN_CLICKED) {
                if (st->folder_filter_list && IsWindowVisible(st->folder_filter_list))
                    hide_folder_filter_list(st);
                else
                    show_folder_filter_list(st, hwnd);
            }
            return 0;
        case IDC_FOLDER_FILTER_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(st->folder_filter_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)st->folder_filter_keys.size())
                    st->active_folder_filter_key = st->folder_filter_keys[sel];
                else
                    st->active_folder_filter_key.clear();
                sync_folder_filter_button(st);
                apply_visibility_filter(st);
                hide_folder_filter_list(st);
            } else if (HIWORD(wp) == LBN_KILLFOCUS) {
                hide_folder_filter_list(st);
            }
            return 0;
        case IDC_HIDE_INCORRECT:
            apply_visibility_filter(st);
            return 0;
        case IDC_GENERATE_PREVIEWS:
            if (generate_previews_enabled(st))
                start_preview_generation(st);
            else if (!st->preview_running)
                set_status(st, "Auto preview generation is disabled.");
            return 0;
        case IDC_LAUNCH_PREV: {
            bool dyn = st->dynamic_chk   && SendMessageW(st->dynamic_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool bet = st->beta_chk      && SendMessageW(st->beta_chk,      BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool den = st->density_chk   && SendMessageW(st->density_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool mot = st->motion_chk    && SendMessageW(st->motion_chk,    BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool hid = st->hide_mame_chk && SendMessageW(st->hide_mame_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (launch_mode(st, true, false, false, dyn, bet, den, mot, hid)) PostQuitMessage(0);
            return 0;
        }
        case IDC_LAUNCH_VR: {
            bool dyn = st->dynamic_chk   && SendMessageW(st->dynamic_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool bet = st->beta_chk      && SendMessageW(st->beta_chk,      BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool den = st->density_chk   && SendMessageW(st->density_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool mot = st->motion_chk    && SendMessageW(st->motion_chk,    BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool hid = st->hide_mame_chk && SendMessageW(st->hide_mame_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (launch_mode(st, false, false, false, dyn, bet, den, mot, hid)) PostQuitMessage(0);
            return 0;
        }
        case IDC_LAUNCH_VR_VIEW: {
            bool dyn = st->dynamic_chk   && SendMessageW(st->dynamic_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool bet = st->beta_chk      && SendMessageW(st->beta_chk,      BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool den = st->density_chk   && SendMessageW(st->density_chk,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool mot = st->motion_chk    && SendMessageW(st->motion_chk,    BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool hid = st->hide_mame_chk && SendMessageW(st->hide_mame_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (launch_mode(st, false, true, false, dyn, bet, den, mot, hid)) PostQuitMessage(0);
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        KillTimer(hwnd, kPreviewTimerId);
        if (st && st->preview_cancel_event)
            SetEvent(st->preview_cancel_event);
        if (st && st->preview_thread) {
            WaitForSingleObject(st->preview_thread, 15000);
            CloseHandle(st->preview_thread);
            st->preview_thread = nullptr;
        }
        if (st && st->preview_cancel_event) {
            CloseHandle(st->preview_cancel_event);
            st->preview_cancel_event = nullptr;
        }
        if (st)
            st->preview_running = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->title_font) {
            DeleteObject(st->title_font);
            st->title_font = nullptr;
        }
        if (st && st->title_bg_brush) {
            DeleteObject(st->title_bg_brush);
            st->title_bg_brush = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int run_launcher(const fs::path& exe_path) {
    GdiplusStartupInput gdiplus_input;
    GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        LauncherState st;
        st.exe_path = exe_path;
        st.exe_dir = exe_path.parent_path();
        st.settings = load_settings(st.exe_dir);
        std::cout << "[launcher] settings.mame_exe=" << st.settings.mame_exe << "\n";
        std::cout << "[launcher] settings.bios_path=" << st.settings.bios_path << "\n";
        std::cout << "[launcher] settings.roms_path(raw)=" << st.settings.roms_path << "\n";
        {
            auto rom_dirs = split_rom_paths(st.settings.roms_path);
            std::cout << "[launcher] rom roots parsed=" << rom_dirs.size() << "\n";
            for (size_t i = 0; i < rom_dirs.size(); ++i) {
                std::cout << "[launcher] rom root[" << i << "]=" << rom_dirs[i].string()
                          << " exists=" << (fs::exists(rom_dirs[i]) ? "yes" : "no")
                          << " dir=" << (fs::is_directory(rom_dirs[i]) ? "yes" : "no") << "\n";
            }
        }
        st.title_map = load_title_map(find_repo_root(exe_path));
        std::cout << "[launcher] source title map entries: " << st.title_map.size() << "\n";
        if (st.title_map.empty())
            st.title_map = load_title_map_from_mame(st.settings.mame_exe);
        else {
            auto mame_map = load_title_map_from_mame(st.settings.mame_exe);
            std::cout << "[launcher] mame title map entries: " << mame_map.size() << "\n";
            st.title_map.insert(mame_map.begin(), mame_map.end());
        }
        auto dump_key = [&](const char* key) {
            auto it = st.title_map.find(key);
            std::cout << "[launcher] " << key << " => "
                      << (it != st.title_map.end() ? it->second : "<missing>") << "\n";
        };
        dump_key("kof98");
        dump_key("mslug");
        dump_key("retrodepth");

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RetroDepthLauncher";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon   = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                         IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        RegisterClassExW(&wc);

        RECT window_rc{ 0, 0, kDefaultLauncherClientW, kDefaultLauncherClientH };
        AdjustWindowRect(&window_rc, WS_OVERLAPPEDWINDOW, FALSE);

        HWND hwnd = CreateWindowW(
            wc.lpszClassName, L"RetroDepth Launcher",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            window_rc.right - window_rc.left,
            window_rc.bottom - window_rc.top,
            nullptr, nullptr, wc.hInstance, &st);

        if (!hwnd) {
            CoUninitialize();
            if (g_gdiplus_token) {
                GdiplusShutdown(g_gdiplus_token);
                g_gdiplus_token = 0;
            }
            return 1;
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    if (g_gdiplus_token) {
        GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_token = 0;
    }
    return 0;
}
