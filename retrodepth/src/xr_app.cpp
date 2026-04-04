#include "xr_app.h"
#include <dxgi1_2.h>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cmath>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <random>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include "settings_io.h"
#include "system_features.h"

using namespace DirectX;
using json = nlohmann::json;
namespace fs = std::filesystem;

// Tilt pivot depth (metres): screen rotates around its own centre at this depth
static constexpr float k_tilt_pivot = 2.0f;
static constexpr int k_vr_preset_count = 5;
static constexpr int k_preset_load_btn_base = 100;
static constexpr int k_preset_save_btn_base = 110;
static constexpr float k_status_panel_h = 3.0f;
static constexpr float k_status_panel_w = 3.0f * (1536.0f / 2304.0f);
static constexpr float k_status_panel_x = -2.25f;
static constexpr float k_status_panel_z = 0.0f;
static constexpr float k_preset_panel_h = 0.72f;
static constexpr float k_preset_panel_gap = 0.10f;
static constexpr float k_random_panel_h = 3.0f;
static constexpr float k_random_panel_w = 3.0f;
static constexpr float k_random_panel_x = 2.25f;
static constexpr float k_random_panel_z = 0.0f;
static constexpr int k_random_game_btn = 200;
static constexpr int k_preset_overlay_tex_w = 2560;
static constexpr int k_preset_overlay_tex_h = 384;
static constexpr int k_preset_overlay_font_scale = 4;
static constexpr int k_preset_overlay_margin_px = k_preset_overlay_font_scale;
static constexpr int k_preset_overlay_glyph_w_px = 8 * k_preset_overlay_font_scale;
static constexpr int k_preset_overlay_glyph_h_px = 8 * k_preset_overlay_font_scale;
static constexpr int k_preset_overlay_line_step_px = k_preset_overlay_glyph_h_px + 1;
static constexpr int k_preset_overlay_cell_chars = 15;
static constexpr int k_preset_overlay_cell_w_px =
    k_preset_overlay_cell_chars * k_preset_overlay_glyph_w_px;

static int preset_col_hit(float pu) {
    const float left =
        (float)k_preset_overlay_margin_px / (float)k_preset_overlay_tex_w;
    const float width =
        (float)(k_preset_overlay_cell_w_px * k_vr_preset_count) /
        (float)k_preset_overlay_tex_w;
    const float right = left + width;
    if (pu < left || pu >= right)
        return -1;
    float u = (pu - left) / width;
    return (std::max)(0, (std::min)(k_vr_preset_count - 1,
        (int)(u * (float)k_vr_preset_count)));
}

static bool preset_row_hit(float pv, int row_idx) {
    constexpr float k_hit_pad_px = 8.0f;
    const float top =
        ((float)k_preset_overlay_margin_px +
         (float)row_idx * (float)k_preset_overlay_line_step_px - k_hit_pad_px) /
        (float)k_preset_overlay_tex_h;
    const float bottom =
        ((float)k_preset_overlay_margin_px +
         (float)row_idx * (float)k_preset_overlay_line_step_px +
         (float)k_preset_overlay_glyph_h_px + k_hit_pad_px) /
        (float)k_preset_overlay_tex_h;
    return pv >= top && pv < bottom;
}

static std::mt19937& vr_random_engine() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

static float randf(float min_v, float max_v) {
    std::uniform_real_distribution<float> dist(min_v, max_v);
    return dist(vr_random_engine());
}

static int randi(int min_v, int max_v) {
    std::uniform_int_distribution<int> dist(min_v, max_v);
    return dist(vr_random_engine());
}

static bool rand_bool(float true_probability) {
    std::bernoulli_distribution dist((double)true_probability);
    return dist(vr_random_engine());
}

static float clampf(float v, float lo, float hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

static XMFLOAT4 random_dark_bg_color() {
    float h = randf(0.0f, 1.0f);
    float s = randf(0.18f, 0.72f);
    float v = randf(0.04f, 0.18f);

    float hp = h * 6.0f;
    int sector = ((int)std::floor(hp)) % 6;
    float f = hp - std::floor(hp);
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);

    float r = v, g = t, b = p;
    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return { r, g, b, 1.0f };
}

static VrPresetLayerState lerp_layer_state(const VrPresetLayerState& a, const VrPresetLayerState& b, float t) {
    VrPresetLayerState out;
    out.depth_meters = a.depth_meters + (b.depth_meters - a.depth_meters) * t;
    out.quad_width_meters = a.quad_width_meters + (b.quad_width_meters - a.quad_width_meters) * t;
    out.copies = (t < 0.5f) ? a.copies : b.copies;
    return out;
}

static VrPresetLayerState sample_preset_layer(const std::vector<VrPresetLayerState>& layers, int dst_index, int dst_count) {
    if (layers.empty())
        return {};
    if ((int)layers.size() == 1 || dst_count <= 1)
        return layers.front();

    float pos = (float)dst_index / (float)(dst_count - 1);
    float srcf = pos * (float)(layers.size() - 1);
    int lo = (int)std::floor(srcf);
    int hi = (int)std::ceil(srcf);
    lo = (std::max)(0, (std::min)(lo, (int)layers.size() - 1));
    hi = (std::max)(0, (std::min)(hi, (int)layers.size() - 1));
    if (lo == hi)
        return layers[lo];
    return lerp_layer_state(layers[lo], layers[hi], srcf - (float)lo);
}

static void shift_layers_depth(std::vector<VrPresetLayerState>& layers, float delta) {
    for (auto& layer : layers)
        layer.depth_meters = (std::max)(0.10f, layer.depth_meters + delta);
}

static void scale_layers_depth_spread(std::vector<VrPresetLayerState>& layers, float scale) {
    if (layers.empty())
        return;
    float near_d = layers.front().depth_meters;
    for (const auto& layer : layers)
        near_d = (std::min)(near_d, layer.depth_meters);
    for (auto& layer : layers)
        layer.depth_meters = (std::max)(0.10f, near_d + (layer.depth_meters - near_d) * scale);
}

static void shift_layers_width(std::vector<VrPresetLayerState>& layers, float delta) {
    for (auto& layer : layers)
        layer.quad_width_meters = (std::max)(0.50f, layer.quad_width_meters + delta);
}

static void scale_layer_copies(std::vector<VrPresetLayerState>& layers, float scale) {
    for (auto& layer : layers)
        for (auto& copy : layer.copies)
            copy = (std::max)(0.001f, copy * scale);
}

static std::string center_cell(const std::string& text, int width) {
    if ((int)text.size() >= width)
        return text.substr(0, width);
    int pad_left = (width - (int)text.size()) / 2;
    int pad_right = width - (int)text.size() - pad_left;
    return std::string((size_t)pad_left, ' ') + text + std::string((size_t)pad_right, ' ');
}

static std::string make_preset_overlay(int active_preset, int hovered_btn) {
    constexpr int k_cell_w = 15;
    std::string row_load, row_name, row_save;
    for (int i = 0; i < k_vr_preset_count; ++i) {
        std::string load = (hovered_btn == k_preset_load_btn_base + i) ? ">LOAD<" : "[LOAD]";
        std::string name = (active_preset == i)
            ? ("*P" + std::to_string(i + 1) + "*")
            : (" P" + std::to_string(i + 1) + " ");
        std::string save = (hovered_btn == k_preset_save_btn_base + i) ? ">SAVE<" : "[SAVE]";
        row_load += center_cell(load, k_cell_w);
        row_name += center_cell(name, k_cell_w);
        row_save += center_cell(save, k_cell_w);
    }
    return row_load + "\n" + row_name + "\n" + row_save;
}

static std::string make_random_overlay(bool hovered) {
    std::ostringstream out;
    out << "\n\n\n\n\n";
    out << (hovered ? ">[ RANDOM ]\n" : "[ RANDOM ]\n");
    out << "[  GAME  ]\n";
    out << "\nVALIDATED ONLY";
    return out.str();
}

struct RandomLaunchEntry {
    std::string short_name;
    std::string storage_key;
    std::string system;
    fs::path source_dir;
};

static std::string to_lower_ascii_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string make_storage_key_for_random(const std::string& system,
                                               const std::string& short_name) {
    return make_storage_key_for_system(system, short_name);
}

static std::string sanitize_preview_component_random(const std::string& s) {
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

static fs::path preview_rom_dir_random(const fs::path& exe_dir,
                                       const std::string& storage_key) {
    std::string safe = sanitize_preview_component_random(storage_key);
    std::string bucket = safe.substr(0, (std::min<size_t>)(2, safe.size()));
    if (bucket.size() < 2)
        bucket.append(2 - bucket.size(), '_');
    return exe_dir / "previews" / bucket / safe;
}

static bool has_preview_images_random(const fs::path& exe_dir,
                                      const std::string& storage_key) {
    fs::path dir = preview_rom_dir_random(exe_dir, storage_key);
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return false;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = to_lower_ascii_copy(entry.path().extension().string());
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp")
            return true;
    }
    return false;
}

static bool load_preview_ok_status_random(const fs::path& exe_dir,
                                          const std::string& storage_key) {
    fs::path status_path = preview_rom_dir_random(exe_dir, storage_key) / "status.txt";
    std::ifstream in(status_path, std::ios::binary);
    if (!in.is_open())
        return false;
    auto trim_local = [](std::string s) {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string{};
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };
    std::string line1;
    std::getline(in, line1);
    return to_lower_ascii_copy(trim_local(line1)) == "ok";
}

static bool is_validated_launcher_rom(const fs::path& exe_dir,
                                      const std::string& storage_key) {
    if (has_preview_images_random(exe_dir, storage_key))
        return true;
    return load_preview_ok_status_random(exe_dir, storage_key);
}

static fs::path find_repo_root_for_random(const fs::path& exe_path) {
    fs::path cur = exe_path.parent_path();
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(cur / "mame-src/src/mame/neogeo/neogeo.cpp"))
            return cur;
        if (!cur.has_parent_path())
            break;
        cur = cur.parent_path();
    }
    return exe_path.parent_path();
}

static std::unordered_set<std::string> load_neogeo_rom_set_for_random(const fs::path& repo_root) {
    std::unordered_set<std::string> out;
    std::ifstream f(repo_root / "mame-src/src/mame/neogeo/neogeo.cpp");
    if (!f.is_open())
        return out;
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find("GAME(");
        if (p == std::string::npos)
            continue;
        size_t c1 = line.find(',', p);
        if (c1 == std::string::npos)
            continue;
        size_t s0 = line.find_first_not_of(" \t", c1 + 1);
        if (s0 == std::string::npos)
            continue;
        size_t s1 = line.find_first_of(", \t", s0);
        if (s1 == std::string::npos)
            continue;
        std::string name = line.substr(s0, s1 - s0);
        if (!name.empty())
            out.insert(name);
    }
    return out;
}

static std::unordered_map<std::string, int> load_cps_rom_sets_for_random(const fs::path& repo_root) {
    std::unordered_map<std::string, int> out;
    for (int sys = 1; sys <= 2; ++sys) {
        std::string fname = (sys == 1)
            ? "mame-src/src/mame/capcom/cps1.cpp"
            : "mame-src/src/mame/capcom/cps2.cpp";
        std::ifstream f(repo_root / fname);
        if (!f.is_open())
            continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t p = line.find("GAME(");
            if (p == std::string::npos)
                p = line.find("GAME_DRIVER(");
            if (p == std::string::npos)
                continue;
            size_t c1 = line.find(',', p);
            if (c1 == std::string::npos)
                continue;
            size_t s0 = line.find_first_not_of(" \t", c1 + 1);
            if (s0 == std::string::npos)
                continue;
            size_t s1 = line.find_first_of(", \t", s0);
            if (s1 == std::string::npos)
                continue;
            std::string name = line.substr(s0, s1 - s0);
            if (!name.empty())
                out[name] = sys;
        }
    }
    return out;
}

static std::unordered_set<std::string> load_konami_rom_set_for_random(const fs::path& repo_root) {
    std::unordered_set<std::string> out;
    static const char* k_files[] = {
        "mame-src/src/mame/konami/tmnt.cpp",
        "mame-src/src/mame/konami/simpsons.cpp",
    };
    for (const auto* fname : k_files) {
        std::ifstream f(repo_root / fname);
        if (!f.is_open())
            continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t p = line.find("GAME(");
            if (p == std::string::npos)
                continue;
            size_t c1 = line.find(',', p);
            if (c1 == std::string::npos)
                continue;
            size_t s0 = line.find_first_not_of(" \t", c1 + 1);
            if (s0 == std::string::npos)
                continue;
            size_t s1 = line.find_first_of(", \t", s0);
            if (s1 == std::string::npos)
                continue;
            std::string name = line.substr(s0, s1 - s0);
            if (!name.empty())
                out.insert(name);
        }
    }
    return out;
}

static std::string current_storage_key_for_random(const GameConfig& config) {
    if (!config.config_path.empty())
        return fs::path(config.config_path).stem().string();
    if (is_sms_cart_rom_name(config.rom_name))
        return make_storage_key_for_random("sms", config.game);
    if (is_nes_cart_rom_name(config.rom_name))
        return make_storage_key_for_random("nes", config.game);
    if (is_gb_cart_rom_name(config.rom_name))
        return make_storage_key_for_random("gb", config.game);
    if (is_gbc_cart_rom_name(config.rom_name))
        return make_storage_key_for_random("gbc", config.game);
    if (is_snes_cart_rom_name(config.rom_name))
        return make_storage_key_for_random("snes", config.game);
    if (is_genesis_cart_rom_name(config.rom_name))
        return make_storage_key_for_random("genesis", config.game);
    return config.game;
}

static std::vector<RandomLaunchEntry> collect_random_launch_entries(const fs::path& exe_path) {
    std::vector<RandomLaunchEntry> out;
    fs::path exe_dir = exe_path.parent_path();
    Settings settings = load_settings(exe_dir);
    auto rom_dirs = split_rom_paths(settings.roms_path);
    fs::path repo_root = find_repo_root_for_random(exe_path);
    auto cps_set = load_cps_rom_sets_for_random(repo_root);
    auto konami_set = load_konami_rom_set_for_random(repo_root);
    auto neogeo_set = load_neogeo_rom_set_for_random(repo_root);
    std::unordered_set<std::string> seen;

    for (const auto& rom_dir : rom_dirs) {
        if (!fs::exists(rom_dir) || !fs::is_directory(rom_dir))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(rom_dir, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file())
                continue;

            std::string ext = to_lower_ascii_copy(entry.path().extension().string());
            bool is_sms = (ext == ".sms");
            bool is_nes = (ext == ".nes" || ext == ".unf" || ext == ".unif");
            bool is_gb = (ext == ".gb");
            bool is_gbc = (ext == ".gbc" || ext == ".cgb");
            bool is_snes = (ext == ".sfc" || ext == ".smc");
            bool is_genesis = (ext == ".bin" || ext == ".md" || ext == ".gen");
            if (!is_sms && !is_nes && !is_gb && !is_gbc && !is_snes && !is_genesis && ext != ".zip" && ext != ".7z")
                continue;

            RandomLaunchEntry rom;
            rom.short_name = entry.path().stem().string();
            if (rom.short_name.empty() || rom.short_name == "neogeo")
                continue;
            rom.source_dir = entry.path().parent_path();

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
                for (const auto& part : entry.path()) {
                    std::string comp = to_lower_ascii_copy(part.string());
                    if ((comp == "sms" || comp == "mastersystem" || comp == "master system") && is_system_enabled("sms")) {
                        rom.system = "sms";
                        break;
                    }
                    if ((comp == "nes" || comp == "famicom" || comp == "nintendo entertainment system") && is_system_enabled("nes")) {
                        rom.system = "nes";
                        break;
                    }
                    if ((comp == "gbc" || comp == "gbcolor" || comp == "gameboy color" || comp == "game boy color")
                        && is_system_enabled("gbc")) {
                        rom.system = "gbc";
                        break;
                    }
                    if ((comp == "gb" || comp == "gameboy" || comp == "game boy")
                        && is_system_enabled("gb")) {
                        rom.system = "gb";
                        break;
                    }
                    if (comp == "snes" && is_system_enabled("snes")) {
                        rom.system = "snes";
                        break;
                    }
                    if ((comp == "genesis" || comp == "megadrive" || comp == "mega drive" || comp == "sega genesis")
                        && is_system_enabled("genesis")) {
                        rom.system = "genesis";
                        break;
                    }
                    if (comp == "konami") {
                        rom.system = "konami";
                        break;
                    }
                    if (comp == "cps" || comp == "cps1" || comp == "cps2") {
                        auto cps_it = cps_set.find(rom.short_name);
                        rom.system = (cps_it != cps_set.end() && cps_it->second == 1) ? "cps1" : "cps2";
                        break;
                    }
                    if (comp == "neogeo" || comp == "neo geo" || comp == "neo-geo") {
                        rom.system = "neogeo";
                        break;
                    }
                }
                if (rom.system.empty()) {
                    auto cps_it = cps_set.find(rom.short_name);
                    if (cps_it != cps_set.end())
                        rom.system = (cps_it->second == 2) ? "cps2" : "cps1";
                    else if (konami_set.count(rom.short_name))
                        rom.system = "konami";
                    else if (neogeo_set.count(rom.short_name))
                        rom.system = "neogeo";
                    else
                        continue;
                }
            }

            rom.storage_key = make_storage_key_for_random(rom.system, rom.short_name);
            if (seen.count(rom.storage_key))
                continue;
            if (!is_validated_launcher_rom(exe_dir, rom.storage_key))
                continue;

            seen.insert(rom.storage_key);
            out.push_back(std::move(rom));
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void xr_check(XrResult result, const char* msg) {
    if (XR_FAILED(result)) {
        std::string err = std::string(msg) + " (XrResult=" + std::to_string(result) + ")";
        if (result == -4)
            err += "\n  --> API version unsupported by runtime (XR_ERROR_API_VERSION_UNSUPPORTED).";
        else if (result == -6 || result == -2)
            err += "\n  --> Is SteamVR running? Start SteamVR before launching retrodepth.";
        throw std::runtime_error(err);
    }
}

static void hr_check(HRESULT hr, const char* msg) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(msg) + " (HRESULT=" + std::to_string(hr) + ")");
}

static XMMATRIX xr_pose_to_matrix(const XrPosef& pose) {
    XMVECTOR rot = XMVectorSet(pose.orientation.x, pose.orientation.y,
                                pose.orientation.z, pose.orientation.w);
    XMVECTOR pos = XMVectorSet(pose.position.x, pose.position.y,
                                pose.position.z, 0.0f);
    XMMATRIX rot_m = XMMatrixRotationQuaternion(rot);
    XMMATRIX trans = XMMatrixTranslationFromVector(pos);
    return XMMatrixMultiply(rot_m, trans);
}

static float yaw_from_pose(const XrPosef& pose) {
    XMVECTOR q = XMVectorSet(pose.orientation.x, pose.orientation.y,
                             pose.orientation.z, pose.orientation.w);
    XMMATRIX rot = XMMatrixRotationQuaternion(q);
    XMVECTOR fwd = XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), rot);
    return std::atan2(XMVectorGetX(fwd), -XMVectorGetZ(fwd));
}

static XMMATRIX make_projection(const XrFovf& fov, float near_z, float far_z) {
    float l = tanf(fov.angleLeft);
    float r = tanf(fov.angleRight);
    float u = tanf(fov.angleUp);
    float d = tanf(fov.angleDown);
    float w = r - l, h = u - d;
    XMMATRIX m = {};
    m.r[0] = XMVectorSet(2.0f / w,        0,                     0,  0);
    m.r[1] = XMVectorSet(0,               2.0f / h,              0,  0);
    m.r[2] = XMVectorSet((r+l)/w,         (u+d)/h,  far_z/(near_z-far_z), -1);
    m.r[3] = XMVectorSet(0,               0, (near_z*far_z)/(near_z-far_z), 0);
    return m;
}

static std::string make_status_overlay(const GameConfig& config,
                                        const std::vector<LayerFrame>* live_frames = nullptr,
                                        int hovered_palette = -1,
                                        int hovered_group   = 0,
                                        int hovered_px      = -1,
                                        int hovered_py      = -1,
                                        int hovered_w       = 0,
                                        int hovered_h       = 0,
                                        const std::string& hovered_matches = {},
                                        int panel_btn       = -1,
                                        bool hover_flash    = false,
                                        bool editor_mode    = false,
                                        float vr_gamma      = 1.15f,
                                        float vr_contrast   = 0.90f,
                                        float vr_saturation = 0.80f,
                                        bool layers_3d_enabled = false,
                                        bool depthmap_enabled = false,
                                        bool depthmap_mirror_enabled = false,
                                        bool upscale_enabled = false,
                                        bool shadows_enabled = false,
                                        bool ambilight_enabled = false,
                                        float screen_curve   = 0.0f,
                                        bool rotate_screen  = false,
                                        float tilt_x        = 0.0f,
                                        float tilt_y        = 0.0f,
                                        float roundness     = 0.0f)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    int dupes = 0;
    float width = 0.0f;
    float first_layer = 0.0f;
    float gap = 0.0f;

    if (live_frames && !live_frames->empty()) {
        dupes = live_frames->front().copies.empty()
            ? D3D11Renderer::k_max_depth_copies
            : (int)(std::min)(live_frames->front().copies.size(),
                              (size_t)D3D11Renderer::k_max_depth_copies);
        width = live_frames->front().quad_width_meters;

        std::vector<float> depths;
        depths.reserve(live_frames->size());
        for (const auto& frame : *live_frames)
            depths.push_back(frame.depth_meters);
        std::sort(depths.begin(), depths.end());
        first_layer = depths.front();
        if (depths.size() >= 2) {
            float total = 0.0f;
            for (size_t i = 1; i < depths.size(); ++i)
                total += depths[i] - depths[i - 1];
            gap = total / (float)(depths.size() - 1);
        }
    } else if (!config.layers.empty()) {
        dupes = config.layers.front().copies.empty()
            ? D3D11Renderer::k_max_depth_copies
            : (int)(std::min)(config.layers.front().copies.size(),
                              (size_t)D3D11Renderer::k_max_depth_copies);
        width = config.layers.front().quad_width_meters;

        std::vector<float> depths;
        depths.reserve(config.layers.size());
        for (const auto& lc : config.layers)
            depths.push_back(lc.depth_meters);
        std::sort(depths.begin(), depths.end());
        first_layer = depths.front();
        if (depths.size() >= 2) {
            float total = 0.0f;
            for (size_t i = 1; i < depths.size(); ++i)
                total += depths[i] - depths[i - 1];
            gap = total / (float)(depths.size() - 1);
        }
    }

    out << "LIVE SETTINGS\n";
    out << "Dupes: " << dupes << "\n";
    out << "First layer: " << first_layer << " m\n";
    out << "Layer gap: " << gap << " m\n";
    out << "Width: " << width << " m\n";

    // Laser hover info
    out << "\n";
    if (hovered_palette >= 0) {
        out << (hover_flash ? ">" : "")
            << "PAL:" << std::setw(2) << std::setfill('0') << hovered_palette
            << " GRP:" << hovered_group << "\n";
    } else {
        out << "PAL:-- GRP:-\n";
    }
    if (hovered_px >= 0 && hovered_py >= 0 && hovered_w > 0 && hovered_h > 0) {
        out << "XY:" << hovered_px << "/" << hovered_w
            << "  " << hovered_py << "/" << hovered_h << "\n";
    } else {
        out << "XY:--/--  --/--\n";
    }
    out << "MATCH:" << (hovered_matches.empty() ? "-" : hovered_matches) << "\n";

    // Virtual buttons (highlighted with '>' when laser is hovering)
    out << "\n";
    if (panel_btn == 0)
        out << ">[LOAD DEFAULT]\n";
    else
        out << "[LOAD DEFAULT]\n";

    if (panel_btn == 1)
        out << ">[LOAD SAVED]\n";
    else
        out << "[LOAD SAVED]\n";

    if (panel_btn == 2)
        out << ">" << (editor_mode ? "[EDITOR: ON]\n" : "[EDITOR: OFF]\n");
    else
        out << (editor_mode ? "[EDITOR: ON]\n" : "[EDITOR: OFF]\n");

    if (panel_btn == 16)
        out << ">" << (layers_3d_enabled ? "[3DLAYERS: ON]\n" : "[3DLAYERS: OFF]\n");
    else
        out <<        (layers_3d_enabled ? "[3DLAYERS: ON]\n" : "[3DLAYERS: OFF]\n");

    if (panel_btn == 17)
        out << ">" << (depthmap_enabled ? "[DEPTHMAP: ON]\n" : "[DEPTHMAP: OFF]\n");
    else
        out <<        (depthmap_enabled ? "[DEPTHMAP: ON]\n" : "[DEPTHMAP: OFF]\n");

    if (panel_btn == 18)
        out << ">" << (depthmap_mirror_enabled ? "[MIRROR: ON]\n" : "[MIRROR: OFF]\n");
    else
        out <<        (depthmap_mirror_enabled ? "[MIRROR: ON]\n" : "[MIRROR: OFF]\n");

    if (panel_btn == 19)
        out << ">" << (upscale_enabled ? "[UPSCALE: ON]\n" : "[UPSCALE: OFF]\n");
    else
        out <<        (upscale_enabled ? "[UPSCALE: ON]\n" : "[UPSCALE: OFF]\n");

    if (panel_btn == 20)
        out << ">" << (shadows_enabled ? "[SHADOWS: ON]\n" : "[SHADOWS: OFF]\n");
    else
        out <<        (shadows_enabled ? "[SHADOWS: ON]\n" : "[SHADOWS: OFF]\n");

    if (panel_btn == 21)
        out << ">" << (ambilight_enabled ? "[AMBILIGHT: ON]\n" : "[AMBILIGHT: OFF]\n");
    else
        out <<        (ambilight_enabled ? "[AMBILIGHT: ON]\n" : "[AMBILIGHT: OFF]\n");

    if (panel_btn == 22)
        out << ">[EVEN SPREAD]\n";
    else
        out << "[EVEN SPREAD]\n";

    // Color grading — depthmap/shadows/even-spread controls sit above this block.
    // btn 3/4 = gamma -/+, btn 5/6 = contrast -/+, btn 7/8 = saturation -/+
    out << "COLOR GRADING\n";
    out << "Gamma:      " << vr_gamma      << "\n";
    out << (panel_btn == 3 ? ">[- ] Gamma"   : " [-] Gamma")   << "\n";
    out << (panel_btn == 4 ? ">[+ ] Gamma"   : " [+] Gamma")   << "\n";
    out << "Contrast:   " << vr_contrast   << "\n";
    out << (panel_btn == 5 ? ">[- ] Contrast"  : " [-] Contrast")  << "\n";
    out << (panel_btn == 6 ? ">[+ ] Contrast"  : " [+] Contrast")  << "\n";
    out << "Saturation: " << vr_saturation << "\n";
    out << (panel_btn == 7 ? ">[- ] Saturation" : " [-] Saturation") << "\n";
    out << (panel_btn == 8 ? ">[+ ] Saturation" : " [+] Saturation") << "\n";

    // Screen rotation
    if (panel_btn == 9)
        out << ">" << (rotate_screen ? "[ROTATE: ON]\n" : "[ROTATE: OFF]\n");
    else
        out <<        (rotate_screen ? "[ROTATE: ON]\n" : "[ROTATE: OFF]\n");

    // Screen tilt
    // tilt_x = up/down (X axis), tilt_y = left/right (Y axis), displayed in degrees
    static constexpr float k_r2d = 57.2958f;
    out << "\n";
    out << "SCREEN TILT\n";
    out << "L/R angle: " << (tilt_y * k_r2d) << " deg\n";
    out << (panel_btn == 10 ? ">[- ] Tilt L/R" : " [-] Tilt L/R") << "\n";
    out << (panel_btn == 11 ? ">[+ ] Tilt L/R" : " [+] Tilt L/R") << "\n";
    out << "U/D angle: " << (tilt_x * k_r2d) << " deg\n";
    out << (panel_btn == 12 ? ">[- ] Tilt U/D" : " [-] Tilt U/D") << "\n";
    out << (panel_btn == 13 ? ">[+ ] Tilt U/D" : " [+] Tilt U/D") << "\n";

    out << "Sphere: " << (int)std::round(roundness * 100.f) << " %\n";
    out << (panel_btn == 14 ? ">[- ] Sphere" : " [-] Sphere") << "\n";
    out << (panel_btn == 15 ? ">[+ ] Sphere" : " [+] Sphere") << "\n";
    out << "Curve: " << (int)std::round(screen_curve * 100.f) << " %\n";
    out << (panel_btn == 23 ? ">[- ] Curve" : " [-] Curve") << "\n";
    out << (panel_btn == 24 ? ">[+ ] Curve" : " [+] Curve") << "\n";

    return out.str();
}

// ---------------------------------------------------------------------------
// XrApp
// ---------------------------------------------------------------------------

XrApp::XrApp(GameConfig config)
    : m_config(std::move(config))
{
    m_factory_default_config = m_config;
    build_default_vr_presets();
    m_vr_presets = m_default_vr_presets;

    create_instance();
    create_system();
    create_d3d11_device();
    create_session();
    create_swapchains();
    create_depth_buffers();
    create_spaces();
    init_actions();

    m_renderer = std::make_unique<D3D11Renderer>(m_d3d_device.Get(), m_d3d_ctx.Get());
    m_renderer->set_brightness(0.65f);
    load_vr_presets();
    load_vr_grading();
    apply_runtime_vr_state();
}

XrApp::~XrApp() {
    destroy();
}

void XrApp::set_dynamic_mode(bool v) {
    m_launch_dynamic = v;
    if (v) m_dynamic_router = std::make_unique<DynamicRouter>(m_config);
    else   m_dynamic_router.reset();
}

bool XrApp::launch_random_validated_game() {
    wchar_t exe_buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    fs::path exe_path = exe_buf;
    fs::path exe_dir = exe_path.parent_path();

    auto candidates = collect_random_launch_entries(exe_path);
    if (candidates.empty()) {
        std::cout << "[random] no launcher-validated games found\n";
        return false;
    }

    std::string current_storage_key = current_storage_key_for_random(m_config);
    std::vector<const RandomLaunchEntry*> filtered;
    filtered.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.storage_key != current_storage_key)
            filtered.push_back(&candidate);
    }

    if (filtered.empty()) {
        std::cout << "[random] no different validated game available\n";
        return false;
    }

    const RandomLaunchEntry& pick = *filtered[(size_t)randi(0, (int)filtered.size() - 1)];
    fs::path config_path = exe_dir / "configs" / (pick.storage_key + ".json");

    std::string cmd = "\"" + exe_path.string() + "\" --game \"" + pick.short_name + "\"";
    cmd += " --config \"" + config_path.string() + "\"";
    if (!pick.system.empty())
        cmd += " --system " + pick.system;
    if (!pick.source_dir.empty())
        cmd += " --romdir \"" + fs::absolute(pick.source_dir).string() + "\"";
    if (m_spectator_enabled)
        cmd += " --spectator";
    if (m_launch_dynamic)
        cmd += " --dynamic";
    if (m_launch_beta_depth)
        cmd += " --beta-depth";
    if (m_launch_density)
        cmd += " --density";
    if (m_launch_motion)
        cmd += " --motion";
    if (m_launch_hide_mame)
        cmd += " --hide-mame";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    std::string exe_dir_str = exe_dir.string();
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        exe_dir_str.c_str(), &si, &pi)) {
        std::cout << "[random] failed to launch child process (" << GetLastError() << ")\n";
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    m_request_exit = true;
    std::cout << "[random] launching " << pick.storage_key << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Instance & system
// ---------------------------------------------------------------------------

void XrApp::create_instance() {
    const char* extensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy_s(info.applicationInfo.applicationName, "retrodepth", XR_MAX_APPLICATION_NAME_SIZE);
    info.applicationInfo.apiVersion     = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount          = 1;
    info.enabledExtensionNames          = extensions;

    xr_check(xrCreateInstance(&info, &m_instance), "xrCreateInstance");

    // Resolve D3D11 requirement extension function
    xr_check(xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&m_pfn_get_d3d11_reqs)),
        "xrGetInstanceProcAddr xrGetD3D11GraphicsRequirementsKHR");
}

void XrApp::create_system() {
    XrSystemGetInfo sys_info = { XR_TYPE_SYSTEM_GET_INFO };
    sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    xr_check(xrGetSystem(m_instance, &sys_info, &m_system_id), "xrGetSystem");
}

// ---------------------------------------------------------------------------
// D3D11 device creation (must use the adapter OpenXR specifies)
// ---------------------------------------------------------------------------

void XrApp::create_d3d11_device() {
    XrGraphicsRequirementsD3D11KHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    xr_check(m_pfn_get_d3d11_reqs(m_instance, m_system_id, &reqs),
             "xrGetD3D11GraphicsRequirementsKHR");

    // Find the adapter matching the LUID OpenXR wants
    ComPtr<IDXGIFactory1> factory;
    hr_check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory1");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (memcmp(&desc.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)) == 0)
            break;
        adapter.Reset();
    }
    if (!adapter)
        throw std::runtime_error("Could not find D3D11 adapter matching OpenXR LUID");

    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    hr_check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               0, feature_levels, 2,
                               D3D11_SDK_VERSION,
                               m_d3d_device.GetAddressOf(), nullptr,
                               m_d3d_ctx.GetAddressOf()),
             "D3D11CreateDevice");
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void XrApp::create_session() {
    XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = m_d3d_device.Get();

    XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
    info.systemId  = m_system_id;
    info.next      = &binding;

    xr_check(xrCreateSession(m_instance, &info, &m_session), "xrCreateSession");
}

// ---------------------------------------------------------------------------
// Swapchains
// ---------------------------------------------------------------------------

void XrApp::create_swapchains() {
    // Query supported formats and pick BGRA sRGB (most compatible)
    uint32_t fmt_count = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &fmt_count, nullptr);
    std::vector<int64_t> formats(fmt_count);
    xrEnumerateSwapchainFormats(m_session, fmt_count, &fmt_count, formats.data());

    // Preferred order: BGRA_UNORM, RGBA_UNORM, BGRA_SRGB
    const int64_t preferred[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };
    m_color_format = formats[0];
    for (int64_t pref : preferred) {
        if (std::find(formats.begin(), formats.end(), pref) != formats.end()) {
            m_color_format = pref;
            break;
        }
    }

    // Query view configs
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> view_configs(view_count,
        { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(m_instance, m_system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        view_count, &view_count, view_configs.data());

    for (uint32_t eye = 0; eye < 2 && eye < view_count; ++eye) {
        const auto& vc = view_configs[eye];

        XrSwapchainCreateInfo sc_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc_info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                              XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc_info.format      = m_color_format;
        sc_info.sampleCount = 1;
        sc_info.width       = vc.recommendedImageRectWidth;
        sc_info.height      = vc.recommendedImageRectHeight;
        sc_info.faceCount   = 1;
        sc_info.arraySize   = 1;
        sc_info.mipCount    = 1;

        xr_check(xrCreateSwapchain(m_session, &sc_info,
                 &m_eye_swapchain[eye].handle), "xrCreateSwapchain");
        m_eye_swapchain[eye].width  = sc_info.width;
        m_eye_swapchain[eye].height = sc_info.height;

        uint32_t img_count = 0;
        xrEnumerateSwapchainImages(m_eye_swapchain[eye].handle, 0, &img_count, nullptr);
        m_eye_images[eye].resize(img_count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(m_eye_swapchain[eye].handle, img_count, &img_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(m_eye_images[eye].data()));

        // Cache raw texture pointers
        m_eye_swapchain[eye].images.resize(img_count);
        for (uint32_t i = 0; i < img_count; ++i)
            m_eye_swapchain[eye].images[i] = m_eye_images[eye][i].texture;
    }
}

// ---------------------------------------------------------------------------
// Depth buffers (we own these, not OpenXR)
// ---------------------------------------------------------------------------

void XrApp::create_depth_buffers() {
    for (int eye = 0; eye < 2; ++eye) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = m_eye_swapchain[eye].width;
        td.Height           = m_eye_swapchain[eye].height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
        hr_check(m_d3d_device->CreateTexture2D(&td, nullptr,
                 m_depth_tex[eye].GetAddressOf()), "Depth texture");

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
        dsvd.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr_check(m_d3d_device->CreateDepthStencilView(
                 m_depth_tex[eye].Get(), &dsvd,
                 m_depth_dsv[eye].GetAddressOf()), "Depth DSV");
    }
}

// ---------------------------------------------------------------------------
// Reference space
// ---------------------------------------------------------------------------

void XrApp::create_spaces() {
    XrReferenceSpaceCreateInfo info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    info.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };
    // Try STAGE first; fall back to LOCAL if not supported
    if (XR_FAILED(xrCreateReferenceSpace(m_session, &info, &m_stage_space))) {
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        xr_check(xrCreateReferenceSpace(m_session, &info, &m_stage_space),
                 "xrCreateReferenceSpace LOCAL");
    }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void XrApp::destroy() {
    m_session_running = false;
    m_spectator.reset();
    m_renderer.reset();

    for (int i = 0; i < 2; ++i) {
        if (m_aim_space[i] != XR_NULL_HANDLE) {
            xrDestroySpace(m_aim_space[i]);
            m_aim_space[i] = XR_NULL_HANDLE;
        }
        if (m_ctrl_space[i] != XR_NULL_HANDLE) {
            xrDestroySpace(m_ctrl_space[i]);
            m_ctrl_space[i] = XR_NULL_HANDLE;
        }
    }

    m_depth_dsv[0].Reset();
    m_depth_dsv[1].Reset();
    m_depth_tex[0].Reset();
    m_depth_tex[1].Reset();

    m_eye_images[0].clear();
    m_eye_images[1].clear();
    m_eye_swapchain[0].images.clear();
    m_eye_swapchain[1].images.clear();

    for (int i = 0; i < 2; ++i) {
        if (m_eye_swapchain[i].handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(m_eye_swapchain[i].handle);
            m_eye_swapchain[i].handle = XR_NULL_HANDLE;
        }
    }
    if (m_action_set != XR_NULL_HANDLE) {
        XrAction actions[] = {
            m_act_move, m_act_a, m_act_b, m_act_trig, m_act_grip, m_act_start, m_act_coin,
            m_act_depth, m_act_lstick, m_act_rstick, m_act_pause, m_act_widen, m_act_narrow,
            m_act_pose[0], m_act_pose[1], m_act_aim[0], m_act_aim[1]
        };
        for (XrAction& act : actions) {
            if (act != XR_NULL_HANDLE) {
                xrDestroyAction(act);
                act = XR_NULL_HANDLE;
            }
        }
        xrDestroyActionSet(m_action_set);
        m_action_set = XR_NULL_HANDLE;
    }
    if (m_stage_space != XR_NULL_HANDLE) { xrDestroySpace(m_stage_space); m_stage_space = XR_NULL_HANDLE; }
    if (m_session     != XR_NULL_HANDLE) { xrDestroySession(m_session);   m_session     = XR_NULL_HANDLE; }
    if (m_instance    != XR_NULL_HANDLE) { xrDestroyInstance(m_instance); m_instance    = XR_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void XrApp::run() {
    if (m_spectator_enabled) {
        m_spectator = std::make_unique<SpectatorWindow>(m_d3d_device.Get(), m_d3d_ctx.Get());
        if (m_spectator)
            apply_runtime_vr_state();
    }

    while (true) {
        bool should_exit = false;
        poll_events(should_exit);
        if (m_spectator) m_spectator->pump_messages();
        if (m_spectator && !m_spectator->is_open()) m_spectator.reset();
        bool esc_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (esc_down && !m_escape_prev)
            should_exit = true;
        m_escape_prev = esc_down;
        if (should_exit || m_request_exit) break;

        if (m_session_running) {
            render_frame();
            if (m_request_exit)
                break;
        }
    }
}

void XrApp::poll_events(bool& should_exit) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            should_exit = true;
            return;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            handle_session_state_changed(
                reinterpret_cast<XrEventDataSessionStateChanged*>(&ev),
                should_exit);
            break;
        default:
            break;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

void XrApp::handle_session_state_changed(
    XrEventDataSessionStateChanged* ev, bool& should_exit)
{
    m_session_state = ev->state;

    switch (m_session_state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        xr_check(xrBeginSession(m_session, &begin), "xrBeginSession");
        m_session_running = true;
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        xrEndSession(m_session);
        m_session_running = false;
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        should_exit = true;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Controller input
// ---------------------------------------------------------------------------

// Helper: send a key event to the MAME window (works even when MAME is unfocused).
static void mame_key(HWND hwnd, UINT vk, bool down) {
    if (!hwnd) return;
    UINT scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    // Arrow keys and other navigation keys are "extended" — bit 24 of lParam must be set
    // or MAME's win32 keyboard provider ignores them.
    static const UINT extended_vks[] = {
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
        VK_RCONTROL, VK_RMENU, VK_RSHIFT,
    };
    bool extended = false;
    for (UINT ev : extended_vks) if (vk == ev) { extended = true; break; }
    LPARAM ext = extended ? (1 << 24) : 0;
    if (down)
        PostMessage(hwnd, WM_KEYDOWN, vk, ext | (scan << 16) | 1);
    else
        PostMessage(hwnd, WM_KEYUP,   vk, ext | (scan << 16) | 0xC0000001);
}

void XrApp::init_actions() {
    // --- Action set ---
    XrActionSetCreateInfo as_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(as_info.actionSetName,          "retrodepth");
    strcpy_s(as_info.localizedActionSetName, "RetroDepth");
    xr_check(xrCreateActionSet(m_instance, &as_info, &m_action_set), "CreateActionSet");

    auto make_action = [&](XrAction& out, const char* name, XrActionType type) {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = type;
        strcpy_s(info.actionName,          name);
        strcpy_s(info.localizedActionName, name);
        xr_check(xrCreateAction(m_action_set, &info, &out), name);
    };

    make_action(m_act_move,  "move",       XR_ACTION_TYPE_VECTOR2F_INPUT);
    make_action(m_act_depth, "depth",      XR_ACTION_TYPE_VECTOR2F_INPUT);
    make_action(m_act_lstick,"lstick",     XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_rstick,"rstick",     XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_pause, "pause",      XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_a,     "fire1",      XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_b,     "fire2",      XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_trig,  "fire3",      XR_ACTION_TYPE_FLOAT_INPUT);
    make_action(m_act_grip,  "fire4",      XR_ACTION_TYPE_FLOAT_INPUT);
    make_action(m_act_start,  "start",      XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_coin,   "coin",       XR_ACTION_TYPE_BOOLEAN_INPUT);
    make_action(m_act_widen,  "widen",      XR_ACTION_TYPE_FLOAT_INPUT);
    make_action(m_act_narrow, "narrow",     XR_ACTION_TYPE_FLOAT_INPUT);
    make_action(m_act_pose[0], "grip_l",   XR_ACTION_TYPE_POSE_INPUT);
    make_action(m_act_pose[1], "grip_r",   XR_ACTION_TYPE_POSE_INPUT);
    make_action(m_act_aim[0],  "aim_l",    XR_ACTION_TYPE_POSE_INPUT);
    make_action(m_act_aim[1],  "aim_r",    XR_ACTION_TYPE_POSE_INPUT);

    // --- Suggested bindings: Oculus Touch ---
    auto path = [&](const char* p) {
        XrPath xp; xrStringToPath(m_instance, p, &xp); return xp;
    };

    XrInteractionProfileSuggestedBinding oculus = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &oculus.interactionProfile);
    XrActionSuggestedBinding oculus_binds[] = {
        { m_act_move,     path("/user/hand/left/input/thumbstick")        },
        { m_act_depth,    path("/user/hand/right/input/thumbstick")       },
        { m_act_lstick,   path("/user/hand/left/input/thumbstick/click")  },
        { m_act_rstick,   path("/user/hand/right/input/thumbstick/click") },
        { m_act_pause,    path("/user/hand/right/input/system/click")     },
        { m_act_a,        path("/user/hand/right/input/a/click")          },
        { m_act_b,        path("/user/hand/right/input/b/click")          },
        { m_act_trig,     path("/user/hand/right/input/trigger/value")    },
        { m_act_grip,     path("/user/hand/right/input/squeeze/value")    },
        { m_act_start,    path("/user/hand/left/input/x/click")           },
        { m_act_coin,     path("/user/hand/left/input/y/click")           },
        { m_act_widen,    path("/user/hand/left/input/trigger/value")     },
        { m_act_narrow,   path("/user/hand/left/input/squeeze/value")     },
        { m_act_pose[0],  path("/user/hand/left/input/grip/pose")         },
        { m_act_pose[1],  path("/user/hand/right/input/grip/pose")        },
        { m_act_aim[0],   path("/user/hand/left/input/aim/pose")          },
        { m_act_aim[1],   path("/user/hand/right/input/aim/pose")         },
    };
    oculus.suggestedBindings    = oculus_binds;
    oculus.countSuggestedBindings = (uint32_t)std::size(oculus_binds);
    xrSuggestInteractionProfileBindings(m_instance, &oculus);

    // --- Suggested bindings: Valve Index (fallback) ---
    XrInteractionProfileSuggestedBinding index_profile = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    xrStringToPath(m_instance, "/interaction_profiles/valve/index_controller", &index_profile.interactionProfile);
    XrActionSuggestedBinding index_binds[] = {
        { m_act_move,     path("/user/hand/left/input/thumbstick")        },
        { m_act_depth,    path("/user/hand/right/input/thumbstick")       },
        { m_act_lstick,   path("/user/hand/left/input/thumbstick/click")  },
        { m_act_rstick,   path("/user/hand/right/input/thumbstick/click") },
        { m_act_pause,    path("/user/hand/right/input/system/click")     },
        { m_act_a,        path("/user/hand/right/input/a/click")          },
        { m_act_b,        path("/user/hand/right/input/b/click")          },
        { m_act_trig,     path("/user/hand/right/input/trigger/click")    },
        { m_act_grip,     path("/user/hand/right/input/squeeze/click")    },
        { m_act_start,    path("/user/hand/left/input/a/click")           },
        { m_act_coin,     path("/user/hand/left/input/b/click")           },
        { m_act_widen,    path("/user/hand/left/input/trigger/value")     },
        { m_act_narrow,   path("/user/hand/left/input/squeeze/value")     },
        { m_act_pose[0],  path("/user/hand/left/input/grip/pose")         },
        { m_act_pose[1],  path("/user/hand/right/input/grip/pose")        },
        { m_act_aim[0],   path("/user/hand/left/input/aim/pose")          },
        { m_act_aim[1],   path("/user/hand/right/input/aim/pose")         },
    };
    index_profile.suggestedBindings       = index_binds;
    index_profile.countSuggestedBindings  = (uint32_t)std::size(index_binds);
    xrSuggestInteractionProfileBindings(m_instance, &index_profile);

    // --- Attach action set to session ---
    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.actionSets      = &m_action_set;
    attach.countActionSets = 1;
    xr_check(xrAttachSessionActionSets(m_session, &attach), "AttachActionSets");

    // --- Create controller spaces ---
    XrActionSpaceCreateInfo space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
    space_info.poseInActionSpace = { {0,0,0,1}, {0,0,0} }; // identity
    for (int h = 0; h < 2; ++h) {
        space_info.action = m_act_pose[h];
        xr_check(xrCreateActionSpace(m_session, &space_info, &m_ctrl_space[h]), "CreateActionSpace");
    }

    // --- Create aim spaces ---
    for (int h = 0; h < 2; ++h) {
        space_info.action = m_act_aim[h];
        xr_check(xrCreateActionSpace(m_session, &space_info, &m_aim_space[h]), "CreateAimSpace");
    }
}

// ---------------------------------------------------------------------------
// VR color grading persistence
// ---------------------------------------------------------------------------

void XrApp::build_default_vr_presets() {
    auto make_from_config = [&](const GameConfig& cfg, const char* name) {
        VrPresetState preset;
        preset.name = name;
        preset.gamma = 1.15f;
        preset.contrast = 0.90f;
        preset.saturation = 0.80f;
        preset.layers3d = false;
        preset.depthmap = false;
        preset.depthmap_mirror = false;
        preset.upscale = false;
        preset.shadows = false;
        preset.rotate_screen = false;
        preset.tilt_x = 0.0f;
        preset.tilt_y = 0.0f;
        preset.roundness = 0.0f;
        preset.quad_y_meters = cfg.quad_y_meters;
        preset.bg_color = {0.02f, 0.02f, 0.05f, 1.0f};
        for (int i = 0; i < 256; ++i)
            preset.palette_route[(size_t)i] = cfg.palette_route[i];
        preset.layers.reserve(cfg.layers.size());
        for (const auto& layer : cfg.layers) {
            VrPresetLayerState out;
            out.depth_meters = layer.depth_meters;
            out.quad_width_meters = layer.quad_width_meters;
            out.copies = layer.copies;
            preset.layers.push_back(std::move(out));
        }
        return preset;
    };

    m_default_vr_presets[0] = make_from_config(m_factory_default_config, "Preset 1");

    m_default_vr_presets[1] = m_default_vr_presets[0];
    m_default_vr_presets[1].name = "Preset 2";
    shift_layers_depth(m_default_vr_presets[1].layers, -0.10f);
    scale_layers_depth_spread(m_default_vr_presets[1].layers, 0.88f);
    shift_layers_width(m_default_vr_presets[1].layers, 0.10f);
    m_default_vr_presets[1].gamma = 1.10f;
    m_default_vr_presets[1].contrast = 0.95f;

    m_default_vr_presets[2] = m_default_vr_presets[0];
    m_default_vr_presets[2].name = "Preset 3";
    scale_layers_depth_spread(m_default_vr_presets[2].layers, 1.18f);
    shift_layers_width(m_default_vr_presets[2].layers, 0.04f);
    m_default_vr_presets[2].shadows = true;
    m_default_vr_presets[2].saturation = 0.90f;

    m_default_vr_presets[3] = m_default_vr_presets[0];
    m_default_vr_presets[3].name = "Preset 4";
    shift_layers_width(m_default_vr_presets[3].layers, 0.08f);
    scale_layer_copies(m_default_vr_presets[3].layers, 1.20f);
    m_default_vr_presets[3].layers3d = true;
    m_default_vr_presets[3].shadows = true;
    m_default_vr_presets[3].roundness = 0.10f;

    m_default_vr_presets[4] = m_default_vr_presets[0];
    m_default_vr_presets[4].name = "Preset 5";
    shift_layers_depth(m_default_vr_presets[4].layers, -0.04f);
    scale_layers_depth_spread(m_default_vr_presets[4].layers, 1.04f);
    shift_layers_width(m_default_vr_presets[4].layers, 0.16f);
    m_default_vr_presets[4].upscale = true;
    m_default_vr_presets[4].contrast = 1.00f;
    m_default_vr_presets[4].saturation = 0.92f;
}

VrPresetState XrApp::capture_current_vr_state() const {
    VrPresetState preset;
    preset.name = "Preset " + std::to_string(m_active_vr_preset + 1);
    preset.gamma = m_vr_gamma;
    preset.contrast = m_vr_contrast;
    preset.saturation = m_vr_saturation;
    preset.layers3d = m_vr_3d_layers;
    preset.depthmap = m_vr_depthmap;
    preset.depthmap_mirror = m_vr_depthmap_mirror;
    preset.upscale = m_vr_upscale;
    preset.shadows = m_vr_shadows;
    preset.ambilight = m_vr_ambilight;
    preset.screen_curve = m_vr_screen_curve;
    preset.rotate_screen = m_rotate_screen;
    preset.tilt_x = m_screen_tilt_x;
    preset.tilt_y = m_screen_tilt_y;
    preset.roundness = m_vr_roundness;
    preset.quad_y_meters = m_config.quad_y_meters;
    preset.bg_color = {m_bg_color.x, m_bg_color.y, m_bg_color.z, m_bg_color.w};
    for (int i = 0; i < 256; ++i)
        preset.palette_route[(size_t)i] = m_config.palette_route[i];
    preset.layers.reserve(m_config.layers.size());
    for (const auto& layer : m_config.layers) {
        VrPresetLayerState out;
        out.depth_meters = layer.depth_meters;
        out.quad_width_meters = layer.quad_width_meters;
        out.copies = layer.copies;
        preset.layers.push_back(std::move(out));
    }
    return preset;
}

void XrApp::apply_runtime_vr_state() {
    m_bg_color = {m_bg_color.x, m_bg_color.y, m_bg_color.z, 1.0f};
    m_renderer->set_gamma(m_vr_gamma);
    m_renderer->set_contrast(m_vr_contrast);
    m_renderer->set_saturation(m_vr_saturation);
    m_renderer->set_rotate_screen(m_rotate_screen);
    m_renderer->set_3d_layers_enabled(m_vr_3d_layers);
    m_renderer->set_depthmap_enabled(m_vr_depthmap);
    m_renderer->set_depthmap_mirror_enabled(m_vr_depthmap_mirror);
    m_renderer->set_upscale_enabled(m_vr_upscale);
    m_renderer->set_shadows_enabled(m_vr_shadows);
    m_renderer->set_ambilight_enabled(m_vr_ambilight);
    m_renderer->set_screen_curve(m_vr_screen_curve);
    m_renderer->set_roundness(m_vr_roundness);
    if (m_spectator) {
        m_spectator->set_gamma(m_vr_gamma);
        m_spectator->set_contrast(m_vr_contrast);
        m_spectator->set_saturation(m_vr_saturation);
        m_spectator->set_rotate_screen(m_rotate_screen);
        m_spectator->set_3d_layers_enabled(m_vr_3d_layers);
        m_spectator->set_depthmap_enabled(m_vr_depthmap);
        m_spectator->set_depthmap_mirror_enabled(m_vr_depthmap_mirror);
        m_spectator->set_upscale_enabled(m_vr_upscale);
        m_spectator->set_shadows_enabled(m_vr_shadows);
        m_spectator->set_ambilight_enabled(m_vr_ambilight);
        m_spectator->set_screen_curve(m_vr_screen_curve);
        m_spectator->set_roundness(m_vr_roundness);
        m_spectator->set_background_color(m_bg_color);
    }
    m_editor.push_ctrl_update();
}

void XrApp::apply_vr_preset_state(const VrPresetState& state) {
    m_vr_gamma = state.gamma;
    m_vr_contrast = state.contrast;
    m_vr_saturation = state.saturation;
    m_vr_3d_layers = state.layers3d;
    m_vr_depthmap = state.depthmap;
    m_vr_depthmap_mirror = state.depthmap_mirror;
    m_vr_upscale = state.upscale;
    m_vr_shadows = state.shadows;
    m_vr_ambilight = state.ambilight;
    m_vr_screen_curve = state.screen_curve;
    m_rotate_screen = state.rotate_screen;
    m_screen_tilt_x = state.tilt_x;
    m_screen_tilt_y = state.tilt_y;
    m_vr_roundness = state.roundness;
    m_config.quad_y_meters = state.quad_y_meters;
    m_bg_color = {state.bg_color[0], state.bg_color[1], state.bg_color[2], state.bg_color[3]};

    for (int i = 0; i < 256; ++i)
        m_config.palette_route[i] = state.palette_route[(size_t)i];

    if (!state.layers.empty() && !m_config.layers.empty()) {
        const int dst_count = (int)m_config.layers.size();
        for (int i = 0; i < dst_count; ++i) {
            VrPresetLayerState src = sample_preset_layer(state.layers, i, dst_count);
            m_config.layers[i].depth_meters = (std::max)(0.10f, src.depth_meters);
            m_config.layers[i].quad_width_meters = (std::max)(0.50f, src.quad_width_meters);
            m_config.layers[i].copies = src.copies;
        }
    }

    apply_runtime_vr_state();
}

void XrApp::load_vr_preset(int idx) {
    if (idx < 0 || idx >= k_vr_preset_count)
        return;
    m_active_vr_preset = idx;
    apply_vr_preset_state(m_vr_presets[(size_t)idx]);
    save_vr_grading();
    std::cout << "[vr] loaded preset " << (idx + 1) << "\n";
}

void XrApp::save_current_to_vr_preset(int idx) {
    if (idx < 0 || idx >= k_vr_preset_count)
        return;
    VrPresetState state = capture_current_vr_state();
    state.name = "Preset " + std::to_string(idx + 1);
    m_vr_presets[(size_t)idx] = std::move(state);
    m_active_vr_preset = idx;
    save_vr_presets();
    save_vr_grading();
    std::cout << "[vr] saved preset " << (idx + 1) << "\n";
}

void XrApp::randomize_vr_state() {
    // Keep randomization inside curated ranges so each click tends to produce
    // a plausible setup rather than an unusable mess.
    m_vr_gamma = randf(0.95f, 1.35f);
    m_vr_contrast = randf(0.82f, 1.20f);
    m_vr_saturation = randf(0.60f, 1.15f);

    m_vr_3d_layers = rand_bool(0.68f);
    m_vr_depthmap = rand_bool(0.18f);
    m_vr_depthmap_mirror = m_vr_depthmap && rand_bool(0.30f);
    m_vr_upscale = rand_bool(0.35f);
    m_vr_shadows = rand_bool(0.58f);
    m_vr_ambilight = rand_bool(0.62f);

    // Rotate stays disabled here; it only makes sense for a narrow slice of games.
    m_rotate_screen = false;

    m_vr_screen_curve = randf(-0.12f, 0.18f);
    m_vr_roundness = randf(-0.18f, 0.22f);
    // Keep random viewing angles fairly subtle; large tilts quickly feel wrong.
    m_screen_tilt_x = randf(-0.075f, 0.075f);
    m_screen_tilt_y = randf(-0.11f, 0.11f);
    m_bg_color = random_dark_bg_color();

    if (!m_config.layers.empty()) {
        const int n = (int)m_config.layers.size();
        // Push random presets into a wider distance range, including layouts that
        // sit noticeably farther away from the visor while staying visually usable.
        const float far_depth = randf(1.55f, 4.20f);
        const float spread = randf(0.24f, 0.88f);
        const float near_depth = (std::max)(0.80f, far_depth - spread);
        const float apparent_scale = randf(0.68f, 1.18f);
        const float base_width = clampf(far_depth * apparent_scale, 1.35f, 3.30f);
        const float width_slope = randf(-0.10f, 0.14f);
        const int copy_count = randi(6, 18);
        const float copy_spacing_base = randf(0.0100f, 0.0200f);

        for (int i = 0; i < n; ++i) {
            float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
            LayerConfig& layer = m_config.layers[(size_t)i];
            layer.depth_meters = far_depth + t * (near_depth - far_depth);
            layer.quad_width_meters = clampf(
                base_width + (t - 0.5f) * width_slope * 2.0f,
                1.40f, 4.00f);

            float spacing = copy_spacing_base * randf(0.90f, 1.12f);
            layer.copies.resize(copy_count);
            for (int c = 0; c < copy_count; ++c)
                layer.copies[(size_t)c] = (float)(c + 1) * spacing;
        }
    }

    apply_runtime_vr_state();
    save_vr_grading();
    std::cout << "[vr] randomized state\n";
}

void XrApp::save_vr_presets() const {
    json root;
    root["version"] = 1;
    root["presets"] = json::array();
    for (const auto& preset : m_vr_presets) {
        json jp;
        jp["name"] = preset.name;
        jp["gamma"] = preset.gamma;
        jp["contrast"] = preset.contrast;
        jp["saturation"] = preset.saturation;
        jp["layers3d"] = preset.layers3d;
        jp["depthmap"] = preset.depthmap;
        jp["depthmap_mirror"] = preset.depthmap_mirror;
        jp["upscale"] = preset.upscale;
        jp["shadows"] = preset.shadows;
        jp["ambilight"] = preset.ambilight;
        jp["screen_curve"] = preset.screen_curve;
        jp["rotate_screen"] = preset.rotate_screen;
        jp["tilt_x"] = preset.tilt_x;
        jp["tilt_y"] = preset.tilt_y;
        jp["roundness"] = preset.roundness;
        jp["quad_y_meters"] = preset.quad_y_meters;
        jp["bg_color"] = { preset.bg_color[0], preset.bg_color[1], preset.bg_color[2], preset.bg_color[3] };

        json route = json::array();
        for (uint8_t v : preset.palette_route)
            route.push_back((int)v);
        jp["palette_route"] = std::move(route);

        json layers = json::array();
        for (const auto& layer : preset.layers) {
            layers.push_back({
                {"depth_meters", layer.depth_meters},
                {"quad_width_meters", layer.quad_width_meters},
                {"copies", layer.copies}
            });
        }
        jp["layers"] = std::move(layers);
        root["presets"].push_back(std::move(jp));
    }

    std::ofstream f("configs/vr_presets.json");
    if (f)
        f << root.dump(2);
}

void XrApp::load_vr_presets() {
    std::ifstream f("configs/vr_presets.json");
    if (!f) {
        save_vr_presets();
        return;
    }

    try {
        json root;
        f >> root;
        const auto& presets = root.value("presets", json::array());
        for (int i = 0; i < k_vr_preset_count && i < (int)presets.size(); ++i) {
            const auto& jp = presets[(size_t)i];
            VrPresetState preset = m_default_vr_presets[(size_t)i];
            preset.name = jp.value("name", preset.name);
            preset.gamma = jp.value("gamma", preset.gamma);
            preset.contrast = jp.value("contrast", preset.contrast);
            preset.saturation = jp.value("saturation", preset.saturation);
            preset.layers3d = jp.value("layers3d", preset.layers3d);
            preset.depthmap = jp.value("depthmap", preset.depthmap);
            preset.depthmap_mirror = jp.value("depthmap_mirror", preset.depthmap_mirror);
            preset.upscale = jp.value("upscale", preset.upscale);
            preset.shadows = jp.value("shadows", preset.shadows);
            preset.ambilight = jp.value("ambilight", preset.ambilight);
            preset.screen_curve = jp.value("screen_curve", preset.screen_curve);
            preset.rotate_screen = jp.value("rotate_screen", preset.rotate_screen);
            preset.tilt_x = jp.value("tilt_x", preset.tilt_x);
            preset.tilt_y = jp.value("tilt_y", preset.tilt_y);
            preset.roundness = jp.value("roundness", preset.roundness);
            preset.quad_y_meters = jp.value("quad_y_meters", preset.quad_y_meters);

            if (jp.contains("bg_color") && jp["bg_color"].is_array() && jp["bg_color"].size() >= 4) {
                for (int c = 0; c < 4; ++c)
                    preset.bg_color[(size_t)c] = jp["bg_color"][(size_t)c].get<float>();
            }
            if (jp.contains("palette_route") && jp["palette_route"].is_array()) {
                const auto& route = jp["palette_route"];
                for (int p = 0; p < 256 && p < (int)route.size(); ++p)
                    preset.palette_route[(size_t)p] = (uint8_t)route[(size_t)p].get<int>();
            }
            if (jp.contains("layers") && jp["layers"].is_array()) {
                preset.layers.clear();
                for (const auto& jl : jp["layers"]) {
                    VrPresetLayerState layer;
                    layer.depth_meters = jl.value("depth_meters", 1.0f);
                    layer.quad_width_meters = jl.value("quad_width_meters", 2.0f);
                    if (jl.contains("copies"))
                        layer.copies = jl["copies"].get<std::vector<float>>();
                    preset.layers.push_back(std::move(layer));
                }
            }
            m_vr_presets[(size_t)i] = std::move(preset);
        }
    } catch (...) {
        m_vr_presets = m_default_vr_presets;
        save_vr_presets();
    }
}

void XrApp::reset_vr_presets_to_defaults() {
    build_default_vr_presets();
    m_vr_presets = m_default_vr_presets;
    m_active_vr_preset = 0;
    save_vr_presets();
}

void XrApp::save_vr_grading() {
    json root;
    root["version"] = 1;
    root["active_preset"] = m_active_vr_preset;
    VrPresetState current = capture_current_vr_state();
    current.name = "Current";

    json jc;
    jc["name"] = current.name;
    jc["gamma"] = current.gamma;
    jc["contrast"] = current.contrast;
    jc["saturation"] = current.saturation;
    jc["layers3d"] = current.layers3d;
    jc["depthmap"] = current.depthmap;
    jc["depthmap_mirror"] = current.depthmap_mirror;
    jc["upscale"] = current.upscale;
    jc["shadows"] = current.shadows;
    jc["ambilight"] = current.ambilight;
    jc["screen_curve"] = current.screen_curve;
    jc["rotate_screen"] = current.rotate_screen;
    jc["tilt_x"] = current.tilt_x;
    jc["tilt_y"] = current.tilt_y;
    jc["roundness"] = current.roundness;
    jc["quad_y_meters"] = current.quad_y_meters;
    jc["bg_color"] = { current.bg_color[0], current.bg_color[1], current.bg_color[2], current.bg_color[3] };
    json route = json::array();
    for (uint8_t v : current.palette_route)
        route.push_back((int)v);
    jc["palette_route"] = std::move(route);
    json layers = json::array();
    for (const auto& layer : current.layers) {
        layers.push_back({
            {"depth_meters", layer.depth_meters},
            {"quad_width_meters", layer.quad_width_meters},
            {"copies", layer.copies}
        });
    }
    jc["layers"] = std::move(layers);
    root["current"] = std::move(jc);

    std::ofstream fj("configs/vr_settings.json");
    if (fj)
        fj << root.dump(2);

    std::ofstream ft("configs/vr_settings.txt");
    if (ft) {
        ft << "gamma="      << m_vr_gamma      << "\n";
        ft << "contrast="   << m_vr_contrast   << "\n";
        ft << "saturation=" << m_vr_saturation << "\n";
        ft << "layers3d="   << (m_vr_3d_layers ? 1 : 0) << "\n";
        ft << "depthmap="   << (m_vr_depthmap ? 1 : 0) << "\n";
        ft << "depthmap_mirror=" << (m_vr_depthmap_mirror ? 1 : 0) << "\n";
        ft << "upscale="    << (m_vr_upscale ? 1 : 0) << "\n";
        ft << "shadows="    << (m_vr_shadows ? 1 : 0) << "\n";
        ft << "ambilight="  << (m_vr_ambilight ? 1 : 0) << "\n";
        ft << "screen_curve=" << m_vr_screen_curve << "\n";
        ft << "tilt_x="     << m_screen_tilt_x << "\n";
        ft << "tilt_y="     << m_screen_tilt_y << "\n";
        ft << "roundness="  << m_vr_roundness  << "\n";
        ft << "active_preset=" << m_active_vr_preset << "\n";
    }
}

void XrApp::load_vr_grading() {
    std::ifstream fj("configs/vr_settings.json");
    if (fj) {
        try {
            json root;
            fj >> root;
            m_active_vr_preset = root.value("active_preset", 0);
            if (m_active_vr_preset < 0 || m_active_vr_preset >= k_vr_preset_count)
                m_active_vr_preset = 0;
            if (root.contains("current") && root["current"].is_object()) {
                VrPresetState state = capture_current_vr_state();
                const auto& jc = root["current"];
                state.name = jc.value("name", state.name);
                state.gamma = jc.value("gamma", state.gamma);
                state.contrast = jc.value("contrast", state.contrast);
                state.saturation = jc.value("saturation", state.saturation);
                state.layers3d = jc.value("layers3d", state.layers3d);
                state.depthmap = jc.value("depthmap", state.depthmap);
                state.depthmap_mirror = jc.value("depthmap_mirror", state.depthmap_mirror);
                state.upscale = jc.value("upscale", state.upscale);
                state.shadows = jc.value("shadows", state.shadows);
                state.ambilight = jc.value("ambilight", state.ambilight);
                state.screen_curve = jc.value("screen_curve", state.screen_curve);
                state.rotate_screen = jc.value("rotate_screen", state.rotate_screen);
                state.tilt_x = jc.value("tilt_x", state.tilt_x);
                state.tilt_y = jc.value("tilt_y", state.tilt_y);
                state.roundness = jc.value("roundness", state.roundness);
                state.quad_y_meters = jc.value("quad_y_meters", state.quad_y_meters);
                if (jc.contains("bg_color") && jc["bg_color"].is_array() && jc["bg_color"].size() >= 4) {
                    for (int c = 0; c < 4; ++c)
                        state.bg_color[(size_t)c] = jc["bg_color"][(size_t)c].get<float>();
                }
                if (jc.contains("palette_route") && jc["palette_route"].is_array()) {
                    const auto& route = jc["palette_route"];
                    for (int p = 0; p < 256 && p < (int)route.size(); ++p)
                        state.palette_route[(size_t)p] = (uint8_t)route[(size_t)p].get<int>();
                }
                if (jc.contains("layers") && jc["layers"].is_array()) {
                    state.layers.clear();
                    for (const auto& jl : jc["layers"]) {
                        VrPresetLayerState layer;
                        layer.depth_meters = jl.value("depth_meters", 1.0f);
                        layer.quad_width_meters = jl.value("quad_width_meters", 2.0f);
                        if (jl.contains("copies"))
                            layer.copies = jl["copies"].get<std::vector<float>>();
                        state.layers.push_back(std::move(layer));
                    }
                }
                apply_vr_preset_state(state);
                return;
            }
        } catch (...) {
        }
    }

    std::ifstream f("configs/vr_settings.txt");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        try {
            float val = std::stof(line.substr(eq + 1));
            if      (key == "gamma")      m_vr_gamma      = val;
            else if (key == "contrast")   m_vr_contrast   = val;
            else if (key == "saturation") m_vr_saturation = val;
            else if (key == "layers3d")   m_vr_3d_layers  = (val >= 0.5f);
            else if (key == "depthmap")   m_vr_depthmap   = (val >= 0.5f);
            else if (key == "depthmap_mirror") m_vr_depthmap_mirror = (val >= 0.5f);
            else if (key == "upscale")    m_vr_upscale    = (val >= 0.5f);
            else if (key == "shadows")    m_vr_shadows    = (val >= 0.5f);
            else if (key == "ambilight")  m_vr_ambilight  = (val >= 0.5f);
            else if (key == "screen_curve") m_vr_screen_curve = val;
            else if (key == "tilt_x")     m_screen_tilt_x = val;
            else if (key == "tilt_y")     m_screen_tilt_y = val;
            else if (key == "roundness")  m_vr_roundness  = val;
            else if (key == "active_preset") m_active_vr_preset = (int)val;
        } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// Laser pointer: ray-cast aim poses against layer quads and left panel
// ---------------------------------------------------------------------------

void XrApp::update_laser_hits(XrTime time, float eye_y) {
    m_renderer->clear_hover_mask();

    // Scene world matrix: layer world space → stage space
    // Tilts pivot around z=-2m (approximate screen center depth) so the screen
    // rotates in place rather than swinging around the world origin.
    // tilt_x positive = pinball (top of screen goes farther away)
    // tilt_y positive = watching from right side
    XMMATRIX sw     = XMMatrixTranslation(0.f, 0.f,  k_tilt_pivot) *
                      XMMatrixRotationX(-m_screen_tilt_x) *  // negate: + = top away = pinball
                      XMMatrixRotationY( m_screen_tilt_y) *
                      XMMatrixTranslation(0.f, 0.f, -k_tilt_pivot) *
                      XMMatrixRotationY(m_scene_yaw) *
                      XMMatrixTranslation(m_scene_x, 0.f, m_scene_z);
    XMMATRIX sw_inv = XMMatrixInverse(nullptr, sw);

    const float panel_x_lw = k_status_panel_x;
    const float panel_z_lw = k_status_panel_z;
    const float panel_h    = k_status_panel_h;
    const float panel_w    = k_status_panel_w;
    const float preset_panel_y = eye_y - (panel_h * 0.5f + k_preset_panel_h * 0.5f + k_preset_panel_gap);

    m_hovered_palette    = -1;
    m_hovered_group      = 0;
    m_hovered_layer      = -1;
    m_hovered_px         = -1;
    m_hovered_py         = -1;
    m_hovered_w          = 0;
    m_hovered_h          = 0;
    m_hovered_matches.clear();
    m_on_colorpal[0]     = false;
    m_on_colorpal[1]     = false;
    m_colorpal_hover[0]  = -1;
    m_colorpal_hover[1]  = -1;

    for (int h = 0; h < 2; ++h) {
        m_laser_on_screen[h] = false;
        m_laser_on_panel[h]  = false;
        m_laser_panel_btn[h] = -1;

        // The left controller currently has no click path for UI interaction,
        // so hide its laser entirely to avoid implying that it can activate UI.
        if (h == 0) {
            m_laser_origin[h] = {0.f, 0.f, 0.f};
            m_laser_tip[h]    = {0.f, 0.f, 0.f};
            continue;
        }

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(m_aim_space[h], m_stage_space, time, &loc);
        XrSpaceLocation grip_loc = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(m_ctrl_space[h], m_stage_space, time, &grip_loc);
        const bool aim_valid =
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        const bool grip_pos_valid =
            (grip_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;

        if (!aim_valid || !grip_pos_valid) {
            // Fallback: use grip pose and shoot straight forward
            if (grip_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                m_laser_origin[h] = { grip_loc.pose.position.x,
                                      grip_loc.pose.position.y,
                                      grip_loc.pose.position.z };
                m_laser_tip[h]    = { m_laser_origin[h].x,
                                      m_laser_origin[h].y,
                                      m_laser_origin[h].z - 5.f };
            } else {
                m_laser_origin[h] = { (h == 0) ? -0.3f : 0.3f, 1.0f, -0.5f };
                m_laser_tip[h]    = { m_laser_origin[h].x,
                                      m_laser_origin[h].y,
                                      m_laser_origin[h].z - 5.f };
            }
            continue;
        }

        XMFLOAT3 origin_stage = { grip_loc.pose.position.x,
                                  grip_loc.pose.position.y,
                                  grip_loc.pose.position.z };

        // Forward direction (-Z in controller space) rotated by aim orientation
        XMVECTOR qv  = XMVectorSet(loc.pose.orientation.x, loc.pose.orientation.y,
                                    loc.pose.orientation.z, loc.pose.orientation.w);
        XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0.f, 0.f, -1.f, 0.f), qv);

        // Stage-space ray (used for panel/colorpal tests — panels are fixed in stage space)
        XMFLOAT3 ro_s = origin_stage;
        XMFLOAT3 rd_s;
        XMStoreFloat3(&rd_s, XMVector3Normalize(fwd));

        // Transform ray into layer world space for layer intersection tests
        XMVECTOR ray_o = XMVector3Transform(XMLoadFloat3(&origin_stage), sw_inv);
        XMVECTOR ray_d = XMVector3Normalize(XMVector3TransformNormal(fwd, sw_inv));
        XMFLOAT3 ro, rd;
        XMStoreFloat3(&ro, ray_o);
        XMStoreFloat3(&rd, ray_d);

        float best_t     = 8.0f;
        int   best_layer = -1;
        float best_u     = 0.f, best_v = 0.f;
        bool  best_in_stage = false; // true = best_t was measured in stage space

        if (m_editor_mode) {
            // Test each layer quad (planar slab at z = -depth_meters in layer world space)
            for (int li = 0; li < (int)m_last_frames.size(); ++li) {
                const auto& f = m_last_frames[li];
                if (std::abs(rd.z) < 1e-6f) continue;
                float t = (-f.depth_meters - ro.z) / rd.z;
                if (t < 0.05f || t >= best_t) continue;
                float hx = ro.x + t * rd.x;
                float hy = ro.y + t * rd.y;
                float qw = f.quad_width_meters;
                float qh = (f.height > 0) ? qw * (float)f.height / (float)f.width : qw;
                float u = hx / qw + 0.5f;
                float v = (hy - eye_y) / (-qh) + 0.5f; // top of screen = v=0
                if (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f) {
                    best_t     = t;
                    best_layer = li;
                    best_u     = u;
                    best_v     = v;
                }
            }
        }

        m_laser_on_screen[h] = (best_layer >= 0);

        // Test status panel quad (vertical plane at x = panel_x_lw, facing +X) — stage space
        if (std::abs(rd_s.x) > 1e-6f) {
            float t_panel = (panel_x_lw - ro_s.x) / rd_s.x;
            if (t_panel > 0.05f && t_panel < best_t) {
                float hy = ro_s.y + t_panel * rd_s.y;
                float hz = ro_s.z + t_panel * rd_s.z;
                float pu = (hz - (panel_z_lw - panel_w * 0.5f)) / panel_w;
                float pv = 1.0f - ((hy - (eye_y - panel_h * 0.5f)) / panel_h);
                bool near_panel = (pu >= -0.25f && pu <= 1.25f && pv >= -0.12f && pv <= 1.12f);
                bool on_panel = (pu >= 0.f && pu <= 1.f && pv >= 0.f && pv <= 1.f);
                if (near_panel) {
                    best_t = t_panel;
                    best_in_stage = true;
                    m_laser_on_panel[h]  = true;
                    m_laser_panel_pu[h]  = pu;
                    if (on_panel)
                        m_laser_on_screen[h] = false; // real panel hit takes priority
                    // pv=0 is top, pv=1 is bottom
                    // Ranges computed from: (margin + line*step) / overlay_h
                    // overlay_h=2304, glyph_h=48, step=49, margin=6
                    if (on_panel) {
                        auto line_hit = [&](int line) {
                            constexpr float k_margin = 6.0f;
                            constexpr float k_step = 49.0f;
                            constexpr float k_glyph_h = 48.0f;
                            constexpr float k_overlay_h = 2304.0f;
                            float top = (k_margin + (float)line * k_step) / k_overlay_h;
                            float bottom = (k_margin + (float)line * k_step + k_glyph_h) / k_overlay_h;
                            return pv >= top && pv < bottom;
                        };
                        if      (line_hit(10)) m_laser_panel_btn[h] = 0;  // Load Default
                        else if (line_hit(11)) m_laser_panel_btn[h] = 1;  // Load Saved
                        else if (line_hit(12)) m_laser_panel_btn[h] = 2;  // Editor toggle
                        else if (line_hit(13)) m_laser_panel_btn[h] = 16; // 3DLayers toggle
                        else if (line_hit(14)) m_laser_panel_btn[h] = 17; // Depthmap toggle
                        else if (line_hit(15)) m_laser_panel_btn[h] = 18; // Mirror toggle
                        else if (line_hit(16)) m_laser_panel_btn[h] = 19; // Upscale toggle
                        else if (line_hit(17)) m_laser_panel_btn[h] = 20; // Shadows toggle
                        else if (line_hit(18)) m_laser_panel_btn[h] = 21; // Ambilight toggle
                        else if (line_hit(19)) m_laser_panel_btn[h] = 22; // Even Spread
                        else if (line_hit(22)) m_laser_panel_btn[h] = 3;  // Gamma -
                        else if (line_hit(23)) m_laser_panel_btn[h] = 4;  // Gamma +
                        else if (line_hit(25)) m_laser_panel_btn[h] = 5;  // Contrast -
                        else if (line_hit(26)) m_laser_panel_btn[h] = 6;  // Contrast +
                        else if (line_hit(28)) m_laser_panel_btn[h] = 7;  // Saturation -
                        else if (line_hit(29)) m_laser_panel_btn[h] = 8;  // Saturation +
                        else if (line_hit(30)) m_laser_panel_btn[h] = 9;  // Rotate
                        else if (line_hit(34)) m_laser_panel_btn[h] = 10; // Tilt L/R -
                        else if (line_hit(35)) m_laser_panel_btn[h] = 11; // Tilt L/R +
                        else if (line_hit(37)) m_laser_panel_btn[h] = 12; // Tilt U/D -
                        else if (line_hit(38)) m_laser_panel_btn[h] = 13; // Tilt U/D +
                        else if (line_hit(40)) m_laser_panel_btn[h] = 14; // Sphere -
                        else if (line_hit(41)) m_laser_panel_btn[h] = 15; // Sphere +
                        else if (line_hit(43)) m_laser_panel_btn[h] = 23; // Curve -
                        else if (line_hit(44)) m_laser_panel_btn[h] = 24; // Curve +
                        else                                     m_laser_panel_btn[h] = -1;
                    }
                }
            }
        }

        // Test preset strip (same wall plane, below the status panel)
        if (std::abs(rd_s.x) > 1e-6f) {
            float t_panel = (panel_x_lw - ro_s.x) / rd_s.x;
            if (t_panel > 0.05f && t_panel <= best_t + 1e-4f) {
                float hy = ro_s.y + t_panel * rd_s.y;
                float hz = ro_s.z + t_panel * rd_s.z;
                float pu = (hz - (panel_z_lw - panel_w * 0.5f)) / panel_w;
                float pv = 1.0f - ((hy - (preset_panel_y - k_preset_panel_h * 0.5f)) / k_preset_panel_h);
                if (pu >= 0.f && pu <= 1.f && pv >= 0.f && pv <= 1.f) {
                    best_t = t_panel;
                    best_in_stage = true;
                    m_laser_on_panel[h] = true;
                    m_laser_on_screen[h] = false;
                    int col = preset_col_hit(pu);
                    if (col >= 0 && preset_row_hit(pv, 0))
                        m_laser_panel_btn[h] = k_preset_load_btn_base + col;
                    else if (col >= 0 && preset_row_hit(pv, 2))
                        m_laser_panel_btn[h] = k_preset_save_btn_base + col;
                    else
                        m_laser_panel_btn[h] = -1;
                }
            }
        }

        // Test random-game panel on the right wall. The whole panel is one giant button.
        if (std::abs(rd_s.x) > 1e-6f) {
            float t_panel = (k_random_panel_x - ro_s.x) / rd_s.x;
            if (t_panel > 0.05f && t_panel < best_t) {
                float hy = ro_s.y + t_panel * rd_s.y;
                float hz = ro_s.z + t_panel * rd_s.z;
                float pu = (hz - (k_random_panel_z - k_random_panel_w * 0.5f)) / k_random_panel_w;
                float pv = 1.0f - ((hy - (eye_y - k_random_panel_h * 0.5f)) / k_random_panel_h);
                if (pu >= 0.f && pu <= 1.f && pv >= 0.f && pv <= 1.f) {
                    best_t = t_panel;
                    best_in_stage = true;
                    m_laser_on_panel[h] = true;
                    m_laser_on_screen[h] = false;
                    m_laser_panel_btn[h] = k_random_game_btn;
                }
            }
        }

        // Test color palette panel (adjacent left of status panel: z=1.0..2.0) — stage space
        {
            const float pal_x =  -2.20f; // 5 cm closer → t always < status panel's t
            const float pal_z =   1.25f;
            const float pal_h =   3.0f;
            const float pal_w =   0.375f;
            if (std::abs(rd_s.x) > 1e-6f) {
                float t_pal = (pal_x - ro_s.x) / rd_s.x;
                if (t_pal > 0.05f && t_pal < best_t) {
                    float hy = ro_s.y + t_pal * rd_s.y;
                    float hz = ro_s.z + t_pal * rd_s.z;
                    float pu = (hz - (pal_z - pal_w * 0.5f)) / pal_w;
                    float pv = 1.0f - ((hy - (eye_y - pal_h * 0.5f)) / pal_h);
                    if (pu >= 0.f && pu <= 1.f && pv >= 0.f && pv <= 1.f) {
                        best_t = t_pal;
                        best_in_stage = true;
                        // UV is mirrored horizontally (negative X scale in world matrix)
                        int col = (int)((1.0f - pu) * D3D11Renderer::k_colorpal_cols);
                        int row = (int)(pv * D3D11Renderer::k_colorpal_rows);
                        col = (std::max)(0, (std::min)(D3D11Renderer::k_colorpal_cols - 1, col));
                        row = (std::max)(0, (std::min)(D3D11Renderer::k_colorpal_rows - 1, row));
                        m_colorpal_hover[h] = row * D3D11Renderer::k_colorpal_cols + col;
                        m_on_colorpal[h]    = true;
                        m_laser_on_panel[h] = true;  // show laser beam
                    }
                }
            }
        }

        // Compute tip position in stage space
        XMFLOAT3 tip_stage;
        if (best_in_stage) {
            // Panel/colorpal hit — best_t already measured in stage space
            tip_stage = { ro_s.x + best_t * rd_s.x,
                          ro_s.y + best_t * rd_s.y,
                          ro_s.z + best_t * rd_s.z };
        } else {
            // Layer hit — best_t in layer-world space, transform back to stage
            XMVECTOR tip_lw = XMVectorSet(ro.x + best_t * rd.x,
                                           ro.y + best_t * rd.y,
                                           ro.z + best_t * rd.z,
                                           1.f);
            XMStoreFloat3(&tip_stage, XMVector3Transform(tip_lw, sw));
        }

        m_laser_origin[h] = origin_stage;
        m_laser_tip[h]    = tip_stage;

        // Palette lookup: only for right controller, only when hitting a layer
        if (m_editor_mode && h == 1 && best_layer >= 0) {
            m_hovered_layer = best_layer;
            const auto& f = m_last_frames[best_layer];
            if (!f.rgba.empty()) {
                int px = (int)(best_u * (float)f.width);
                int py = (int)(best_v * (float)f.height);
                px = (std::max)(0, (std::min)(px, f.width  - 1));
                py = (std::max)(0, (std::min)(py, f.height - 1));
                m_hovered_px = px;
                m_hovered_py = py;
                m_hovered_w  = f.width;
                m_hovered_h  = f.height;
                if (!f.owner_ids.empty()) {
                    uint16_t owner = f.owner_ids[py * f.width + px];
                    if (owner != LayerFrame::OWNER_NONE) {
                        m_hovered_palette = (int)owner;
                        m_hovered_group = m_editor.get_palette_group(m_hovered_palette);
                        m_hovered_matches = std::to_string(m_hovered_palette);

                        std::vector<uint8_t> mask((size_t)f.width * f.height, 0);
                        std::vector<uint8_t> visited((size_t)f.width * f.height, 0);
                        std::vector<int> stack;
                        stack.push_back(py * f.width + px);
                        while (!stack.empty()) {
                            int pos = stack.back();
                            stack.pop_back();
                            if (pos < 0 || pos >= f.width * f.height || visited[pos]) continue;
                            visited[pos] = 1;
                            int sx = pos % f.width;
                            int sy = pos / f.width;
                            if (f.owner_ids[pos] != owner) continue;
                            mask[pos] = 180;
                            if (sx > 0)            stack.push_back(pos - 1);
                            if (sx + 1 < f.width)  stack.push_back(pos + 1);
                            if (sy > 0)            stack.push_back(pos - f.width);
                            if (sy + 1 < f.height) stack.push_back(pos + f.width);
                        }
                        m_renderer->update_hover_mask(best_layer, f.width, f.height, mask.data());
                    }
                } else {
                    int pidx = (py * f.width + px) * 4;
                    uint8_t pb = f.rgba[pidx + 0];
                    uint8_t pg = f.rgba[pidx + 1];
                    uint8_t pr = f.rgba[pidx + 2];
                    uint8_t pa = f.rgba[pidx + 3];
                    if (pa > 0) {
                        const uint32_t (*pals)[16] = m_shmem.get_palettes();
                        if (pals) {
                            int match_count = 0;
                            for (int p = 0; p < 256; ++p) {
                                for (int c = 0; c < 16; ++c) {
                                    uint32_t col = pals[p][c];
                                    uint8_t cb = (uint8_t)( col        & 0xFF);
                                    uint8_t cg = (uint8_t)((col >>  8) & 0xFF);
                                    uint8_t cr = (uint8_t)((col >> 16) & 0xFF);
                                    if (cb == pb && cg == pg && cr == pr) {
                                        if (m_hovered_palette < 0)
                                            m_hovered_palette = p;
                                        if (match_count < 8) {
                                            if (!m_hovered_matches.empty()) m_hovered_matches += ",";
                                            m_hovered_matches += std::to_string(p);
                                        } else if (match_count == 8) {
                                            m_hovered_matches += ",...";
                                        }
                                        ++match_count;
                                        break;
                                    }
                                }
                            }
                            if (m_hovered_palette >= 0)
                                m_hovered_group = m_editor.get_palette_group(m_hovered_palette);
                        }
                    }
                }
            }
        }
    }
    m_eye_y_cache = eye_y;
}

void XrApp::poll_actions(XrTime predicted_display_time, const XrView* views, uint32_t view_count) {
    // Sync actions
    XrActiveActionSet active = { m_action_set, XR_NULL_PATH };
    XrActionsSyncInfo sync   = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.activeActionSets      = &active;
    sync.countActiveActionSets = 1;
    if (XR_FAILED(xrSyncActions(m_session, &sync))) return;

    // Helper: read 2D axis
    auto get_vec2 = [&](XrAction act) -> XrVector2f {
        XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
        XrActionStateGetInfo g  = { XR_TYPE_ACTION_STATE_GET_INFO };
        g.action = act;
        xrGetActionStateVector2f(m_session, &g, &s);
        return s.isActive ? s.currentState : XrVector2f{0,0};
    };

    // Helper: read boolean
    auto get_bool = [&](XrAction act) -> bool {
        XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
        XrActionStateGetInfo g = { XR_TYPE_ACTION_STATE_GET_INFO };
        g.action = act;
        xrGetActionStateBoolean(m_session, &g, &s);
        return s.isActive && s.currentState;
    };

    // Helper: read float action and threshold to bool (for trigger/grip)
    auto get_float_as_bool = [&](XrAction act, float threshold = 0.5f) -> bool {
        XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
        XrActionStateGetInfo g = { XR_TYPE_ACTION_STATE_GET_INFO };
        g.action = act;
        xrGetActionStateFloat(m_session, &g, &s);
        return s.isActive && s.currentState >= threshold;
    };

    // --- Left stick → directional keys ---
    XrVector2f move = get_vec2(m_act_move);
    static constexpr float STICK_THRESH = 0.5f;
    bool dirs[4] = {
        move.y >  STICK_THRESH,  // UP
        move.y < -STICK_THRESH,  // DOWN
        move.x < -STICK_THRESH,  // LEFT
        move.x >  STICK_THRESH,  // RIGHT
    };
    const UINT dir_vk[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    for (int d = 0; d < 4; ++d) {
        if (dirs[d] != m_dir_held[d]) {
            mame_key(m_mame_hwnd, dir_vk[d], dirs[d]);
            m_dir_held[d] = dirs[d];
        }
    }

    // --- Right buttons → fire keys (standard MAME NeoGeo defaults) ---
    // Right trigger: only forward VK_SPACE to MAME when laser is NOT on any interactive surface
    bool trig_raw = get_float_as_bool(m_act_trig);
    bool trig_for_mame = trig_raw && (!m_editor_mode || (!m_laser_on_screen[1] && !m_laser_on_panel[1]));
    bool btns[6] = {
        get_bool(m_act_b),              // fire 1 (B btn) → LCtrl
        get_bool(m_act_a),              // fire 2 (A btn) → LAlt
        trig_for_mame,                  // fire 3 (trigger) → Space (suppressed when laser active)
        get_float_as_bool(m_act_grip),  // fire 4 (grip)    → LShift
        get_bool(m_act_start),          // start  → 1
        get_bool(m_act_coin),           // coin   → 5
    };
    const UINT btn_vk[6] = { VK_LCONTROL, VK_LMENU, VK_SPACE, VK_LSHIFT, '1', '5' };
    for (int b = 0; b < 6; ++b) {
        if (btns[b] != m_btn_prev[b]) {
            mame_key(m_mame_hwnd, btn_vk[b], btns[b]);
            m_btn_prev[b] = btns[b];
        }
    }

    // --- Laser trigger: cycle palette group or press virtual panel button ---
    if (trig_raw && !m_rtrig_prev) {
        if (m_on_colorpal[1] && m_colorpal_hover[1] >= 0) {
            m_bg_color = m_renderer->get_palette_color(m_colorpal_hover[1]);
            apply_runtime_vr_state();
            save_vr_grading();
            std::cout << "[colorpal] bg color " << m_colorpal_hover[1] << "\n";
        } else if (m_editor_mode && m_laser_on_screen[1] && m_hovered_palette >= 0) {
            int grp = (m_editor.get_palette_group(m_hovered_palette) + 1) % 4;
            m_editor.set_palette_group(m_hovered_palette, grp);
            m_hovered_group = grp;
            save_vr_grading();
            std::cout << "[laser] pal " << m_hovered_palette << " -> grp " << grp << "\n";
        } else if (m_laser_on_panel[1]) {
            if (m_laser_panel_btn[1] == 0) {
                reset_vr_presets_to_defaults();
                apply_vr_preset_state(m_default_vr_presets[0]);
                save_vr_grading();
                std::cout << "[laser] loaded defaults\n";
            } else if (m_laser_panel_btn[1] == k_random_game_btn) {
                launch_random_validated_game();
            } else if (m_laser_panel_btn[1] == 1) {
                // Load Saved: reload config from file
                if (!m_config.config_path.empty()) {
                    auto reloaded = GameConfig::load(m_config.config_path);
                    for (int p = 0; p < 256; ++p) {
                        int grp = (reloaded.palette_route[p] == 0xFF) ? 0 : reloaded.palette_route[p];
                        m_editor.set_palette_group(p, grp);
                    }
                    std::cout << "[laser] loaded saved palette groups\n";
                }
            } else if (m_laser_panel_btn[1] == 2) {
                m_editor_mode = !m_editor_mode;
                m_editor.set_thumb_requested(m_editor_mode);
                m_hovered_palette = -1;
                m_hovered_group = 0;
                m_hovered_layer = -1;
                for (int h = 0; h < 2; ++h) {
                    m_laser_on_screen[h] = false;
                    m_laser_on_panel[h] = false;
                    m_laser_panel_btn[h] = -1;
                }
                std::cout << "[laser] editor mode " << (m_editor_mode ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] >= k_preset_load_btn_base &&
                       m_laser_panel_btn[1] < k_preset_load_btn_base + k_vr_preset_count) {
                load_vr_preset(m_laser_panel_btn[1] - k_preset_load_btn_base);
            } else if (m_laser_panel_btn[1] >= k_preset_save_btn_base &&
                       m_laser_panel_btn[1] < k_preset_save_btn_base + k_vr_preset_count) {
                save_current_to_vr_preset(m_laser_panel_btn[1] - k_preset_save_btn_base);
            } else if (m_laser_panel_btn[1] == 16) {
                m_vr_3d_layers = !m_vr_3d_layers;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] 3dlayers " << (m_vr_3d_layers ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 17) {
                m_vr_depthmap = !m_vr_depthmap;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] depthmap " << (m_vr_depthmap ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 18) {
                m_vr_depthmap_mirror = !m_vr_depthmap_mirror;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] mirror " << (m_vr_depthmap_mirror ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 19) {
                m_vr_upscale = !m_vr_upscale;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] upscale " << (m_vr_upscale ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 20) {
                m_vr_shadows = !m_vr_shadows;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] shadows " << (m_vr_shadows ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 21) {
                m_vr_ambilight = !m_vr_ambilight;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] ambilight " << (m_vr_ambilight ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 22) {
                even_spread_layer_depths(m_config.layers);
                save_vr_grading();
                std::cout << "[laser] even spread\n";
            } else if (m_laser_panel_btn[1] == 23) {
                m_vr_screen_curve = (std::max)(-0.50f, m_vr_screen_curve - 0.01f);
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] curve " << m_vr_screen_curve << "\n";
            } else if (m_laser_panel_btn[1] == 24) {
                m_vr_screen_curve = (std::min)(0.50f, m_vr_screen_curve + 0.01f);
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] curve " << m_vr_screen_curve << "\n";
            } else if (m_laser_panel_btn[1] == 3) {
                m_vr_gamma = (std::max)(0.5f, (std::min)(2.5f, m_vr_gamma - 0.05f));
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] gamma " << m_vr_gamma << "\n";
            } else if (m_laser_panel_btn[1] == 4) {
                m_vr_gamma = (std::max)(0.5f, (std::min)(2.5f, m_vr_gamma + 0.05f));
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] gamma " << m_vr_gamma << "\n";
            } else if (m_laser_panel_btn[1] == 5) {
                m_vr_contrast = (std::max)(0.5f, (std::min)(2.5f, m_vr_contrast - 0.05f));
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] contrast " << m_vr_contrast << "\n";
            } else if (m_laser_panel_btn[1] == 6) {
                m_vr_contrast = (std::max)(0.5f, (std::min)(2.5f, m_vr_contrast + 0.05f));
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] contrast " << m_vr_contrast << "\n";
            } else if (m_laser_panel_btn[1] == 7) {
                m_vr_saturation = (std::max)(0.0f, (std::min)(2.0f, m_vr_saturation - 0.05f));
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] saturation " << m_vr_saturation << "\n";
            } else if (m_laser_panel_btn[1] == 8) {
                m_vr_saturation = (std::max)(0.0f, (std::min)(2.0f, m_vr_saturation + 0.05f));
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] saturation " << m_vr_saturation << "\n";
            } else if (m_laser_panel_btn[1] == 9) {
                m_rotate_screen = !m_rotate_screen;
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] rotate screen " << (m_rotate_screen ? "ON" : "OFF") << "\n";
            } else if (m_laser_panel_btn[1] == 10) {
                m_screen_tilt_y = (std::max)(-XM_PIDIV2, m_screen_tilt_y - 0.0873f); // -5 deg
                save_vr_grading();
            } else if (m_laser_panel_btn[1] == 11) {
                m_screen_tilt_y = (std::min)( XM_PIDIV2, m_screen_tilt_y + 0.0873f); // +5 deg
                save_vr_grading();
            } else if (m_laser_panel_btn[1] == 12) {
                m_screen_tilt_x = (std::max)(-XM_PIDIV2, m_screen_tilt_x - 0.0873f); // -5 deg
                save_vr_grading();
            } else if (m_laser_panel_btn[1] == 13) {
                m_screen_tilt_x = (std::min)( XM_PIDIV2, m_screen_tilt_x + 0.0873f); // +5 deg
                save_vr_grading();
            } else if (m_laser_panel_btn[1] == 14) {
                m_vr_roundness = (std::max)(-1.0f, m_vr_roundness - 0.01f);
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] roundness " << m_vr_roundness << "\n";
            } else if (m_laser_panel_btn[1] == 15) {
                m_vr_roundness = (std::min)( 1.0f, m_vr_roundness + 0.01f);
                apply_runtime_vr_state();
                save_vr_grading();
                std::cout << "[laser] roundness " << m_vr_roundness << "\n";
            }
        }
    }
    m_rtrig_prev = trig_raw;

    {
        bool down = get_bool(m_act_lstick);
        if (down && !m_lstick_prev) {
            randomize_vr_state();
        }
        m_lstick_prev = down;
    }

    {
        bool down = get_bool(m_act_rstick);
        if (down && !m_rstick_prev && view_count > 0) {
            float avg_x = 0.0f;
            float avg_z = 0.0f;
            for (uint32_t i = 0; i < view_count; ++i) {
                avg_x += views[i].pose.position.x;
                avg_z += views[i].pose.position.z;
            }
            avg_x /= (float)view_count;
            avg_z /= (float)view_count;
            m_scene_x = avg_x;
            m_scene_z = avg_z;
            m_scene_yaw = -yaw_from_pose(views[0].pose);
            std::cout << "Scene recentered.\n";
        }
        m_rstick_prev = down;
    }

    {
        bool down = get_bool(m_act_pause);
        if (down != m_pause_prev) {
            mame_key(m_mame_hwnd, VK_F5, down);
            m_pause_prev = down;
        }
    }

    // Right stick:
    //   Y up/down   — shift ALL layers closer / farther (translate the whole scene)
    //   X left/right — compress / expand depth spread proportionally (scale all depths)
    if (!m_config.layers.empty()) {
        auto stick = get_vec2(m_act_depth);
        constexpr float k_dead = 0.25f;
        bool active = (stick.y > k_dead || stick.y < -k_dead || stick.x > k_dead || stick.x < -k_dead);
        ULONGLONG now = GetTickCount64();
        if (active && now >= m_stick_next_fire) {
            bool changed = false;
            // Y: translate — up = closer (subtract depth), down = farther (add depth)
            if (stick.y > k_dead || stick.y < -k_dead) {
                float delta = -stick.y * 0.05f;
                for (auto& lc : m_config.layers) {
                    float d = lc.depth_meters + delta;
                    lc.depth_meters = d < 0.1f ? 0.1f : d;
                }
                changed = true;
            }
            // X: expand/compress spread — anchor at nearest layer, scale distances proportionally
            if (stick.x > k_dead || stick.x < -k_dead) {
                float min_d = m_config.layers[0].depth_meters;
                for (auto& lc : m_config.layers)
                    if (lc.depth_meters < min_d) min_d = lc.depth_meters;
                float scale = 1.0f + stick.x * 0.01f;
                for (auto& lc : m_config.layers) {
                    float d = min_d + (lc.depth_meters - min_d) * scale;
                    lc.depth_meters = d < 0.1f ? 0.1f : d;
                }
                changed = true;
            }
            if (changed)
                save_vr_grading();
            m_stick_next_fire = now + 80;
        } else if (!active) {
            m_stick_next_fire = now + 250;
        }
    }

    // Left trigger = widen all layers, left grip = narrow all layers
    {
        constexpr float k_w_dead = 0.25f;
        float widen  = get_float_as_bool(m_act_widen)  ? 1.0f : 0.0f;
        float narrow = get_float_as_bool(m_act_narrow) ? 1.0f : 0.0f;
        bool  active = (widen > k_w_dead || narrow > k_w_dead);
        ULONGLONG now = GetTickCount64();
        if (active && now >= m_width_next_fire) {
            float delta = (widen - narrow) * 0.05f;
            for (auto& lc : m_config.layers) {
                float w = lc.quad_width_meters + delta;
                lc.quad_width_meters = w < 0.5f ? 0.5f : w;
            }
            save_vr_grading();
            m_width_next_fire = now + 80;
        } else if (!active) {
            m_width_next_fire = now + 250;
        }
    }

    // --- Locate controller spaces for rendering ---
    for (int h = 0; h < 2; ++h) {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(m_ctrl_space[h], m_stage_space, predicted_display_time, &loc);
        bool valid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                     (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        XMMATRIX world = valid ? xr_pose_to_matrix(loc.pose) : XMMatrixIdentity();
        m_renderer->set_controller_transform(h, valid, world);
    }
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void XrApp::render_frame() {
    // --- Wait for frame ---
    XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
    xr_check(xrWaitFrame(m_session, nullptr, &frame_state), "xrWaitFrame");
    xr_check(xrBeginFrame(m_session, nullptr), "xrBeginFrame");

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj_layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView proj_views[2] = {};

    if (frame_state.shouldRender) {
        // --- Locate views ---
        XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime           = frame_state.predictedDisplayTime;
        locate.space                 = m_stage_space;

        XrViewState view_state = { XR_TYPE_VIEW_STATE };
        uint32_t view_count = 0;
        XrView views[2] = {{ XR_TYPE_VIEW }, { XR_TYPE_VIEW }};
        xrLocateViews(m_session, &locate, &view_state, 2, &view_count, views);

        // --- Read new frame from shared memory ---
        {
            auto new_frames = m_shmem.poll(m_config);
            if (!new_frames.empty()) {
                m_renderer->resize_layers((int)new_frames.size());
                for (int i = 0; i < (int)new_frames.size(); ++i)
                    m_renderer->update_layer(i, new_frames[i]);
                m_last_frames = std::move(new_frames);
                m_editor.on_new_frame(m_last_frames);
                if (m_spectator) m_spectator->update_layers(m_last_frames);
            }
            if (m_dynamic_router && !m_last_frames.empty())
                m_dynamic_router->on_frame(m_last_frames);
        }

        // --- Controller input: poll actions, forward keys to MAME ---
        poll_actions(frame_state.predictedDisplayTime, views, view_count);

        // --- Layer editor: poll keys, push state to renderer ---
        m_editor.poll_keys();
        int sel_sorted = -1;
        {
            int sel_cfg = m_editor.get_selected();
            if (sel_cfg >= 0 && sel_cfg < (int)m_config.layers.size()) {
                const std::string& sel_id = m_config.layers[sel_cfg].id;
                for (int i = 0; i < (int)m_last_frames.size(); ++i) {
                    if (m_last_frames[i].id == sel_id) { sel_sorted = i; break; }
                }
            }
            m_renderer->set_editor_state(m_editor.is_active(), sel_sorted, m_editor.blink_on());
        }

        // Compute eye_y once (used for laser hit tests and quad positioning)
        {
            float raw_eye_y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            float eye_y_for_laser = m_eye_y_locked ? m_locked_eye_y : raw_eye_y;
            update_laser_hits(frame_state.predictedDisplayTime, eye_y_for_laser);
        }

        m_renderer->set_laser_state(m_laser_origin, m_laser_tip,
                                     m_laser_on_screen, m_laser_on_panel,
                                     m_laser_panel_btn,
                                     m_hovered_palette, m_hovered_group,
                                     m_hovered_layer);

        bool hover_flash = (m_hovered_palette >= 0) && ((GetTickCount64() / 125) % 2 == 0);
        std::string overlay_text = make_status_overlay(m_config,
                                                       &m_last_frames,
                                                       m_hovered_palette,
                                                       m_hovered_group,
                                                       m_hovered_px,
                                                       m_hovered_py,
                                                       m_hovered_w,
                                                       m_hovered_h,
                                                       m_hovered_matches,
                                                       m_laser_panel_btn[1],
                                                       hover_flash,
                                                       m_editor_mode,
                                                       m_vr_gamma,
                                                       m_vr_contrast,
                                                       m_vr_saturation,
                                                       m_vr_3d_layers,
                                                       m_vr_depthmap,
                                                       m_vr_depthmap_mirror,
                                                       m_vr_upscale,
                                                       m_vr_shadows,
                                                       m_vr_ambilight,
                                                       m_vr_screen_curve,
                                                       m_rotate_screen,
                                                       m_screen_tilt_x,
                                                       m_screen_tilt_y,
                                                       m_vr_roundness);
        std::string preset_overlay_text = make_preset_overlay(m_active_vr_preset, m_laser_panel_btn[1]);
        std::string random_overlay_text = make_random_overlay(m_laser_panel_btn[1] == k_random_game_btn);
        m_renderer->update_overlay(overlay_text);
        m_renderer->update_preset_overlay(preset_overlay_text);
        m_renderer->update_random_overlay(random_overlay_text);

        // --- Render each eye ---
        for (int eye = 0; eye < 2; ++eye) {
            uint32_t img_idx = 0;
            xrAcquireSwapchainImage(m_eye_swapchain[eye].handle, nullptr, &img_idx);

            XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wait.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(m_eye_swapchain[eye].handle, &wait);

            ID3D11Texture2D* color_tex = m_eye_swapchain[eye].images[img_idx];

            // Build EyeParams
            XMMATRIX view_mat = xr_pose_to_matrix(views[eye].pose);
            XMMATRIX view_inv = XMMatrixInverse(nullptr, view_mat);
            XMMATRIX proj_mat = make_projection(views[eye].fov, 0.05f, 100.0f);

            // F5 toggles eye-height lock: locked = snapped to position at key press,
            // unlocked = live tracking (quads follow head height automatically).
            bool f5_down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
            if (f5_down && !m_f5_was_down) {
                m_eye_y_locked = !m_eye_y_locked;
                if (m_eye_y_locked) {
                    m_locked_eye_y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
                    std::cout << "Eye height locked at " << m_locked_eye_y << " m\n";
                } else {
                    std::cout << "Eye height unlocked (live tracking)\n";
                }
            }
            m_f5_was_down = f5_down;

            float eye_y = m_eye_y_locked
                ? m_locked_eye_y
                : (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;

            EyeParams ep;
            ep.view          = view_inv;
            ep.proj          = proj_mat;
            ep.quad_y_meters = eye_y;
            ep.scene_world   = XMMatrixTranslation(0.f, 0.f,  k_tilt_pivot) *
                               XMMatrixRotationX(-m_screen_tilt_x) *
                               XMMatrixRotationY( m_screen_tilt_y) *
                               XMMatrixTranslation(0.f, 0.f, -k_tilt_pivot) *
                               XMMatrixRotationY(m_scene_yaw) *
                               XMMatrixTranslation(m_scene_x, 0.0f, m_scene_z);
            ep.viewport = {
                0.0f, 0.0f,
                (float)m_eye_swapchain[eye].width,
                (float)m_eye_swapchain[eye].height,
                0.0f, 1.0f
            };

            // Bind color RTV + depth DSV, clear, draw
            ComPtr<ID3D11RenderTargetView> rtv;
            D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
            rtvd.Format        = (DXGI_FORMAT)m_color_format;
            rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            m_d3d_device->CreateRenderTargetView(color_tex, &rtvd, rtv.GetAddressOf());

            float clear[4] = {
                m_bg_color.x,
                m_bg_color.y,
                m_bg_color.z,
                1.0f
            };
            m_d3d_ctx->ClearRenderTargetView(rtv.Get(), clear);
            m_d3d_ctx->ClearDepthStencilView(m_depth_dsv[eye].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

            ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
            m_d3d_ctx->OMSetRenderTargets(1, rtvs, m_depth_dsv[eye].Get());

            m_renderer->render_frame(m_last_frames, ep);

            if (m_spectator && eye == 0) {
                m_spectator->update_frame(ep,
                                          m_editor.is_active(),
                                          sel_sorted,
                                          m_editor.blink_on(),
                                          m_laser_origin,
                                          m_laser_tip,
                                          m_laser_on_screen,
                                          m_laser_on_panel,
                                          m_laser_panel_btn,
                                          m_hovered_palette,
                                          m_hovered_group,
                                          m_hovered_layer,
                                          overlay_text,
                                          preset_overlay_text,
                                          random_overlay_text);
            }

            m_d3d_ctx->OMSetRenderTargets(0, nullptr, nullptr);

            xrReleaseSwapchainImage(m_eye_swapchain[eye].handle, nullptr);

            // Fill projection view
            proj_views[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            proj_views[eye].pose    = views[eye].pose;
            proj_views[eye].fov     = views[eye].fov;
            proj_views[eye].subImage.swapchain             = m_eye_swapchain[eye].handle;
            proj_views[eye].subImage.imageRect.offset      = {0, 0};
            proj_views[eye].subImage.imageRect.extent      = {
                m_eye_swapchain[eye].width, m_eye_swapchain[eye].height };
            proj_views[eye].subImage.imageArrayIndex       = 0;
        }

        proj_layer.space     = m_stage_space;
        proj_layer.viewCount = 2;
        proj_layer.views     = proj_views;
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj_layer));
    }

    XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
    end_info.displayTime          = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount           = (uint32_t)layers.size();
    end_info.layers               = layers.data();
    xrEndFrame(m_session, &end_info);
}
