#include "preview_window.h"
#include "settings_io.h"
#include "system_features.h"
#include <dxgi.h>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <DirectXMath.h>
#include <iostream>

using namespace DirectX;
namespace fs = std::filesystem;

static void hr_check(HRESULT hr, const char* msg) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(msg) + " hr=" + std::to_string(hr));
}

static std::mt19937& preview_random_engine() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

static float randf(float min_v, float max_v) {
    std::uniform_real_distribution<float> dist(min_v, max_v);
    return dist(preview_random_engine());
}

static int randi(int min_v, int max_v) {
    std::uniform_int_distribution<int> dist(min_v, max_v);
    return dist(preview_random_engine());
}

static bool rand_bool(float true_probability) {
    std::bernoulli_distribution dist((double)true_probability);
    return dist(preview_random_engine());
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

PreviewWindow::PreviewWindow(GameConfig config)
    : m_config(std::move(config))
{
    create_window();
    create_d3d11();
    create_swapchain();
    m_renderer = std::make_unique<D3D11Renderer>(m_device.Get(), m_ctx.Get());
    apply_renderer_state();
    refresh_status_ui();
}

PreviewWindow::~PreviewWindow() {
    m_renderer.reset();
    if (m_hwnd) {
        SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void PreviewWindow::set_dynamic_mode(bool v) {
    m_launch_dynamic = v;
    if (v) m_dynamic_router = std::make_unique<DynamicRouter>(m_config);
    else   m_dynamic_router.reset();
    if (m_dynamic_router) {
        m_dynamic_router->set_beta_depth(m_launch_beta_depth);
        m_dynamic_router->set_density_scoring(m_launch_density);
        m_dynamic_router->set_motion_scoring(m_launch_motion);
    }
    m_info_win.set_router(m_dynamic_router.get());
}

void PreviewWindow::set_beta_depth(bool v) {
    m_launch_beta_depth = v;
    if (m_dynamic_router)
        m_dynamic_router->set_beta_depth(v);
}

void PreviewWindow::set_density_scoring(bool v) {
    m_launch_density = v;
    if (m_dynamic_router)
        m_dynamic_router->set_density_scoring(v);
}

void PreviewWindow::set_motion_scoring(bool v) {
    m_launch_motion = v;
    if (m_dynamic_router)
        m_dynamic_router->set_motion_scoring(v);
}

void PreviewWindow::set_3d_layers_enabled(bool v) {
    m_3d_layers_enabled = v;
    if (m_renderer)
        m_renderer->set_3d_layers_enabled(v);
    m_status_ui_dirty = true;
}

void PreviewWindow::set_depthmap_enabled(bool v) {
    m_depthmap_enabled = v;
    if (m_renderer)
        m_renderer->set_depthmap_enabled(v);
    m_status_ui_dirty = true;
}

void PreviewWindow::set_depthmap_mirror_enabled(bool v) {
    m_depthmap_mirror_enabled = v;
    if (m_renderer)
        m_renderer->set_depthmap_mirror_enabled(v);
    m_status_ui_dirty = true;
}

void PreviewWindow::set_upscale_enabled(bool v) {
    m_upscale_enabled = v;
    if (m_renderer)
        m_renderer->set_upscale_enabled(v);
    m_status_ui_dirty = true;
}

void PreviewWindow::set_shadows_enabled(bool v) {
    m_shadows_enabled = v;
    if (m_renderer)
        m_renderer->set_shadows_enabled(v);
    m_status_ui_dirty = true;
}

void PreviewWindow::set_ambilight_enabled(bool v) {
    m_ambilight_enabled = v;
    if (m_renderer)
        m_renderer->set_ambilight_enabled(v);
    m_status_ui_dirty = true;
}

void PreviewWindow::set_screen_curve(float v) {
    m_screen_curve = (std::max)(-0.50f, (std::min)(0.50f, v));
    if (m_renderer)
        m_renderer->set_screen_curve(m_screen_curve);
    m_status_ui_dirty = true;
}

void PreviewWindow::apply_renderer_state() {
    if (!m_renderer)
        return;
    m_renderer->set_3d_layers_enabled(m_3d_layers_enabled);
    m_renderer->set_depthmap_enabled(m_depthmap_enabled);
    m_renderer->set_depthmap_mirror_enabled(m_depthmap_mirror_enabled);
    m_renderer->set_upscale_enabled(m_upscale_enabled);
    m_renderer->set_shadows_enabled(m_shadows_enabled);
    m_renderer->set_ambilight_enabled(m_ambilight_enabled);
    m_renderer->set_rotate_screen(m_rotate_screen);
    m_renderer->set_contrast(m_contrast);
    m_renderer->set_saturation(m_saturation);
    m_renderer->set_gamma(m_gamma);
    m_renderer->set_roundness(m_roundness);
    m_renderer->set_screen_curve(m_screen_curve);
}

void PreviewWindow::refresh_last_frames_from_config() {
    if (m_last_frames.empty())
        return;
    for (auto& frame : m_last_frames) {
        for (const auto& layer : m_config.layers) {
            if (layer.id != frame.id)
                continue;
            frame.depth_meters = layer.depth_meters;
            frame.quad_width_meters = layer.quad_width_meters;
            frame.copies = layer.copies;
            break;
        }
    }
}

void PreviewWindow::randomize_preview_state() {
    m_gamma = randf(0.95f, 1.35f);
    m_contrast = randf(0.82f, 1.20f);
    m_saturation = randf(0.60f, 1.15f);

    m_3d_layers_enabled = rand_bool(0.68f);
    m_depthmap_enabled = rand_bool(0.18f);
    m_depthmap_mirror_enabled = m_depthmap_enabled && rand_bool(0.30f);
    m_upscale_enabled = rand_bool(0.35f);
    m_shadows_enabled = rand_bool(0.58f);
    m_ambilight_enabled = rand_bool(0.62f);
    m_rotate_screen = false;
    m_screen_curve = randf(-0.12f, 0.18f);
    m_roundness = randf(-0.18f, 0.22f);
    m_bg_color = random_dark_bg_color();

    if (!m_config.layers.empty()) {
        const int n = (int)m_config.layers.size();
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
            layer.copies.resize((size_t)copy_count);
            for (int c = 0; c < copy_count; ++c)
                layer.copies[(size_t)c] = (float)(c + 1) * spacing;
        }
    }

    apply_renderer_state();
    refresh_last_frames_from_config();
    m_dist_init = false;
    m_status_ui_dirty = true;
    std::cout << "[preview] randomized state\n";
}

bool PreviewWindow::launch_random_validated_game() {
    wchar_t exe_buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    fs::path exe_path = exe_buf;
    fs::path exe_dir = exe_path.parent_path();

    auto candidates = collect_random_launch_entries(exe_path);
    if (candidates.empty()) {
        std::cout << "[preview] no launcher-validated games found\n";
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
        std::cout << "[preview] no different validated game available\n";
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
    cmd += " --preview";
    if (m_launch_dynamic)
        cmd += " --dynamic";
    if (m_launch_beta_depth)
        cmd += " --beta-depth";
    if (m_launch_density)
        cmd += " --density";
    if (m_launch_motion)
        cmd += " --motion";
    if (m_auto_exit_ms > 0)
        cmd += " --auto-exit-ms " + std::to_string(m_auto_exit_ms);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    std::string exe_dir_str = exe_dir.string();
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        exe_dir_str.c_str(), &si, &pi)) {
        std::cout << "[preview] failed to launch child process (" << GetLastError() << ")\n";
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    m_running = false;
    std::cout << "[preview] launching " << pick.storage_key << "\n";
    return true;
}

void PreviewWindow::refresh_status_ui() {
    if (!m_renderer)
        return;

    std::string overlay =
        "PREVIEW\n"
        "\n"
        "[L] 3D LAYERS: ";
    overlay += m_3d_layers_enabled ? "ON\n" : "OFF\n";
    overlay += "[D] DEPTHMAP: ";
    overlay += m_depthmap_enabled ? "ON\n" : "OFF\n";
    overlay += "[M] MIRROR  : ";
    overlay += m_depthmap_mirror_enabled ? "ON\n" : "OFF\n";
    overlay += "[U] UPSCALE : ";
    overlay += m_upscale_enabled ? "ON\n" : "OFF\n";
    overlay += "[S] SHADOWS : ";
    overlay += m_shadows_enabled ? "ON\n" : "OFF\n";
    overlay += "[H] AMBILIGHT: ";
    overlay += m_ambilight_enabled ? "ON\n" : "OFF\n";
    overlay += "[P] RANDOM SETTINGS\n";
    overlay += "[O] RANDOM GAME\n";
    overlay += "CURVE: ";
    overlay += std::to_string((int)std::lround(m_screen_curve * 100.0f));
    overlay += "%\n";
    overlay += "[,] CURVE -\n";
    overlay += "[.] CURVE +\n";
    overlay += "\n"
               "[ESC] EXIT\n";
    m_renderer->update_overlay(overlay);

    std::string title = "RetroDepth Preview";
    title += m_3d_layers_enabled ? " | 3DLayers ON" : " | 3DLayers OFF";
    title += m_depthmap_enabled ? " | Depthmap ON" : " | Depthmap OFF";
    title += m_depthmap_mirror_enabled ? " | Mirror ON" : " | Mirror OFF";
    title += m_upscale_enabled ? " | Upscale ON" : " | Upscale OFF";
    title += m_shadows_enabled ? " | Shadows ON" : " | Shadows OFF";
    title += m_ambilight_enabled ? " | Ambilight ON" : " | Ambilight OFF";
    title += " | Curve " + std::to_string((int)std::lround(m_screen_curve * 100.0f)) + "%";
    title += " | L/D/M/U/S/H/P/O ,/. | ESC exit";
    if (m_hwnd)
        SetWindowTextA(m_hwnd, title.c_str());

    m_status_ui_dirty = false;
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK PreviewWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
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
    case WM_KEYDOWN: {
        if (!self) return 0;
        if (wp == VK_ESCAPE) { self->m_running = false; return 0; }
        if (wp == 'L') { self->set_3d_layers_enabled(!self->m_3d_layers_enabled); return 0; }
        if (wp == 'D') { self->set_depthmap_enabled(!self->m_depthmap_enabled); return 0; }
        if (wp == 'M') { self->set_depthmap_mirror_enabled(!self->m_depthmap_mirror_enabled); return 0; }
        if (wp == 'U') { self->set_upscale_enabled(!self->m_upscale_enabled); return 0; }
        if (wp == 'S') { self->set_shadows_enabled(!self->m_shadows_enabled); return 0; }
        if (wp == 'H') { self->set_ambilight_enabled(!self->m_ambilight_enabled); return 0; }
        if (wp == 'P') { self->randomize_preview_state(); return 0; }
        if (wp == 'O') { self->launch_random_validated_game(); return 0; }
        if (wp == VK_OEM_COMMA) { self->set_screen_curve(self->m_screen_curve - 0.01f); return 0; }
        if (wp == VK_OEM_PERIOD) { self->set_screen_curve(self->m_screen_curve + 0.01f); return 0; }
        if (self->m_mame_hwnd)
            PostMessageA(self->m_mame_hwnd, WM_KEYDOWN, wp, lp);
        return 0;
    }
    case WM_KEYUP:
        if (self && self->m_mame_hwnd)
            PostMessageA(self->m_mame_hwnd, WM_KEYUP, wp, lp);
        return 0;
    case WM_LBUTTONDOWN:
        if (self) {
            self->m_dragging = true;
            self->m_last_mx  = LOWORD(lp);
            self->m_last_my  = HIWORD(lp);
            SetCapture(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (self) { self->m_dragging = false; ReleaseCapture(); }
        return 0;
    case WM_MOUSEMOVE:
        if (self && self->m_dragging) {
            int mx = LOWORD(lp), my = HIWORD(lp);
            int dx = mx - self->m_last_mx;
            int dy = my - self->m_last_my;
            self->m_last_mx = mx;
            self->m_last_my = my;
            self->m_yaw   += dx * 0.005f;
            self->m_pitch += dy * 0.005f;
            // Clamp pitch so we don't flip over
            if (self->m_pitch >  1.4f) self->m_pitch =  1.4f;
            if (self->m_pitch < -1.4f) self->m_pitch = -1.4f;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (self) {
            int delta = (short)HIWORD(wp);
            float factor = (delta > 0) ? 0.9f : 1.1f;
            self->m_distance *= factor;
            if (self->m_distance < 0.1f)  self->m_distance = 0.1f;
            if (self->m_distance > 50.0f) self->m_distance = 50.0f;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void PreviewWindow::create_window() {
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RetroDepthPreview";
    RegisterClassExA(&wc);

    RECT rc = {0, 0, m_width, m_height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExA(0, "RetroDepthPreview",
        "RetroDepth Preview — ESC to exit",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

    SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hwnd, SW_SHOW);
}

// ---------------------------------------------------------------------------
// D3D11
// ---------------------------------------------------------------------------

void PreviewWindow::create_d3d11() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    hr_check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               0, &fl, 1, D3D11_SDK_VERSION,
                               m_device.GetAddressOf(), nullptr,
                               m_ctx.GetAddressOf()), "D3D11CreateDevice");
}

void PreviewWindow::create_swapchain() {
    ComPtr<IDXGIFactory> factory;
    hr_check(CreateDXGIFactory(IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory");

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount          = 2;
    sd.BufferDesc.Width     = m_width;
    sd.BufferDesc.Height    = m_height;
    sd.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow         = m_hwnd;
    sd.SampleDesc.Count     = 1;
    sd.Windowed             = TRUE;
    sd.SwapEffect           = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr_check(factory->CreateSwapChain(m_device.Get(), &sd,
             m_swapchain.GetAddressOf()), "CreateSwapChain");

    resize(m_width, m_height);
}

void PreviewWindow::resize(int w, int h) {
    m_width = w; m_height = h;

    m_rtv.Reset();
    m_dsv.Reset();
    m_depth_tex.Reset();

    m_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

    ComPtr<ID3D11Texture2D> back;
    m_swapchain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()));
    m_device->CreateRenderTargetView(back.Get(), nullptr, m_rtv.GetAddressOf());

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format           = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
    m_device->CreateTexture2D(&td, nullptr, m_depth_tex.GetAddressOf());

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
    dsvd.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depth_tex.Get(), &dsvd, m_dsv.GetAddressOf());
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void PreviewWindow::run() {
    bool printed_connected = false;
    bool printed_frames    = false;
    int  frame_count       = 0;
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

        // Diagnostic: report shmem and frame arrival
        if (!printed_connected && m_shmem.is_connected()) {
            std::cout << "[Preview] MAME shared memory connected.\n";
            printed_connected = true;
        }
        if (!printed_frames && !m_last_frames.empty()) {
            std::cout << "[Preview] First frame received (" << m_last_frames.size() << " layers).\n";
            printed_frames = true;
        }
        ++frame_count;

        render_frame();
    }
}

void PreviewWindow::render_frame() {
    if (m_status_ui_dirty)
        refresh_status_ui();

    m_shmem.set_thumb_needed(m_editor.is_active());
    auto new_frames = m_shmem.poll(m_config);
    if (!new_frames.empty()) {
        if ((int)new_frames.size() != m_last_layer_count) {
            m_renderer->resize_layers((int)new_frames.size());
            m_last_layer_count = (int)new_frames.size();
        }
        for (int i = 0; i < (int)new_frames.size(); ++i)
            m_renderer->update_layer(i, new_frames[i]);
        m_last_frames = std::move(new_frames);
        // Forward live palette colours to editor window for swatch display
        const uint32_t (*pals)[16] = m_shmem.get_palettes();
        if (pals) m_info_win.set_palette_colors(pals);
        const uint32_t (*thumbs)[32 * 32] = m_shmem.get_thumbs();
        if (thumbs) m_info_win.set_palette_thumbs(thumbs);
    }
    if (m_dynamic_router && !m_last_frames.empty())
        m_dynamic_router->on_frame(m_last_frames);
    const std::vector<LayerFrame>& frames = m_last_frames;

    // Clear (dark navy — visible if D3D11 works but no frames arrive)
    ID3D11RenderTargetView* rtvs[] = { m_rtv.Get() };
    m_ctx->OMSetRenderTargets(1, rtvs, m_dsv.Get());
    float clear[4] = {m_bg_color.x, m_bg_color.y, m_bg_color.z, m_bg_color.w};
    m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);
    m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    m_editor.poll_keys();
    if (m_editor.is_active())
        m_info_win.show_and_sync();
    else
        m_info_win.hide();
    // Translate config-layer index → sorted-frame index so the blink highlights
    // the correct quad even after depths are reordered.
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

    // Orbit camera: anchor at the midpoint of all layer depths.
    // Left-drag to rotate, scroll wheel to zoom.
    float anchor_z = 2.5f; // fallback
    float anchor_y = m_config.quad_y_meters;
    if (!frames.empty()) {
        float min_d = frames[0].depth_meters;
        float max_d = frames[0].depth_meters;
        for (const auto& frame : frames) {
            if (frame.depth_meters < min_d) min_d = frame.depth_meters;
            if (frame.depth_meters > max_d) max_d = frame.depth_meters;
        }
        anchor_z = (min_d + max_d) * 0.5f;
    } else if (!m_config.layers.empty()) {
        float min_d = m_config.layers[0].depth_meters;
        float max_d = m_config.layers[0].depth_meters;
        for (const auto& lc : m_config.layers) {
            if (lc.depth_meters < min_d) min_d = lc.depth_meters;
            if (lc.depth_meters > max_d) max_d = lc.depth_meters;
        }
        anchor_z = (min_d + max_d) * 0.5f;
    }
    // On the first frame: place camera at world origin (z=0), looking straight
    // at the active layer midpoint — identical to wearing the VR headset.
    if (!m_dist_init) {
        m_distance = anchor_z;
        m_dist_init = true;
    }
    XMVECTOR anchor = XMVectorSet(0.0f, anchor_y, -anchor_z, 0.0f);

    // Camera position: orbit around anchor by yaw and pitch
    XMVECTOR dir = XMVectorSet(
        sinf(m_yaw) * cosf(m_pitch),
        sinf(m_pitch),
        cosf(m_yaw) * cosf(m_pitch),
        0.0f);
    XMVECTOR eye_pos = XMVectorAdd(anchor, XMVectorScale(dir, m_distance));

    XMMATRIX view = XMMatrixLookAtRH(eye_pos, anchor, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

    float aspect = (float)m_width / m_height;
    XMMATRIX proj = XMMatrixPerspectiveFovRH(
        XMConvertToRadians(80.0f), aspect, 0.05f, 100.0f);

    EyeParams ep;
    ep.view          = view;
    ep.proj          = proj;
    ep.viewport      = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    ep.quad_y_meters = m_config.quad_y_meters;

    if (!frames.empty()) {
        if (m_editor.is_active() && m_editor.is_solo()
            && sel_sorted >= 0 && sel_sorted < (int)frames.size()) {
            std::vector<LayerFrame> solo = { frames[sel_sorted] };
            m_renderer->set_editor_state(true, -1, false);
            m_renderer->render_frame(solo, ep);
        } else {
            m_renderer->render_frame(frames, ep);
        }
    }

    m_swapchain->Present(1, 0);
}
