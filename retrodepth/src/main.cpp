#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "xr_app.h"
#include "preview_window.h"
#include "game_config.h"
#include "system_features.h"
#include "settings_io.h"
#include "launcher_window.h"
#include "spectator_window.h"
#include "diagnostics_recorder.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// MAME launcher
// ---------------------------------------------------------------------------

// Spawns MAME and returns its process handle (or NULL on failure).
static HANDLE launch_mame(const Settings& s, const std::string& rom_name) {
    if (s.mame_exe.empty()) return nullptr;

    // For cartridge-based console systems, resolve the cart name to a full file path so MAME can find it.
    std::string effective_rom = rom_name;
    auto choose_sms_machine = [](const std::string& cart_name) -> std::string {
        std::string lower = cart_name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (lower.find("korea") != std::string::npos)
            return "smskr";
        return "sms1";
    };

    // Helper: resolve a "-cart \"name\"" rom_name to an absolute path.
    // machine_prefix = e.g. "snes", system_folder = e.g. "snes"
    // cart_exts: ordered list of extensions to try
    auto resolve_cart = [&](const std::string& stored_prefix,
                            const std::string& machine_prefix,
                            const std::string& system_folder,
                            const std::vector<std::string>& cart_exts) {
        const std::string prefix = stored_prefix + " -cart \"";
        if (rom_name.size() <= prefix.size()) return;
        if (rom_name.substr(0, prefix.size()) != prefix) return;
        auto start = rom_name.find('"') + 1;
        auto end   = rom_name.rfind('"');
        if (end <= start) return;
        std::string cart_name = rom_name.substr(start, end - start);
        // Already a full path if it contains a directory separator.
        if (cart_name.find('\\') != std::string::npos || cart_name.find('/') != std::string::npos) return;
        std::string launch_machine = machine_prefix;
        if (stored_prefix == "sms" || stored_prefix == "sms1" || stored_prefix == "smsj" || stored_prefix == "smskr")
            launch_machine = choose_sms_machine(cart_name);
        for (const auto& dir : split_rom_paths(s.roms_path)) {
            for (const auto& ext : cart_exts) {
                fs::path candidate = dir / (cart_name + ext);
                if (fs::exists(candidate)) {
                    effective_rom = launch_machine + " -cart \"" + candidate.string() + "\"";
                    return;
                }
            }
            for (const auto& ext : cart_exts) {
                fs::path candidate = dir / system_folder / (cart_name + ext);
                if (fs::exists(candidate)) {
                    effective_rom = launch_machine + " -cart \"" + candidate.string() + "\"";
                    return;
                }
            }
        }
    };

    resolve_cart("sms",     "sms1",    "sms",     {".zip", ".7z", ".sms"});
    resolve_cart("sms1",    "sms1",    "sms",     {".zip", ".7z", ".sms"});
    resolve_cart("smsj",    "smsj",    "sms",     {".zip", ".7z", ".sms"});
    resolve_cart("smskr",   "smskr",   "sms",     {".zip", ".7z", ".sms"});
    resolve_cart("nes",     "nes",     "nes",     {".zip", ".7z", ".nes", ".unf", ".unif"});
    resolve_cart("snes",    "snes",    "snes",    {".zip", ".7z", ".sfc", ".smc"});
    resolve_cart("genesis", "genesis", "genesis", {".zip", ".7z", ".bin", ".md", ".gen"});
    resolve_cart("gb",      "gameboy", "gb",      {".zip", ".7z", ".gb"});
    resolve_cart("gameboy", "gameboy", "gb",      {".zip", ".7z", ".gb"});
    resolve_cart("gbc",     "gbcolor", "gbc",     {".zip", ".7z", ".gbc", ".cgb", ".gb"});
    resolve_cart("gbcolor", "gbcolor", "gbc",     {".zip", ".7z", ".gbc", ".cgb", ".gb"});

    std::string cmd = "\"" + s.mame_exe + "\" " + effective_rom;
    {
        // Build rompath: bios_path first (for neogeo.zip), then all configured ROM roots.
        // MAME uses semicolons to separate multiple paths.
        std::string rompath = s.bios_path;
        std::string joined_rom_paths = join_rom_paths(split_rom_paths(s.roms_path));
        if (!joined_rom_paths.empty()) {
            if (!rompath.empty()) rompath += ";";
            rompath += joined_rom_paths;
        }
        cmd += " -rompath \"" + rompath + "\"";
    }
    if (!s.mame_args.empty())
        cmd += " " + s.mame_args;

    std::cout << "Launching MAME: " << cmd << "\n";

    STARTUPINFOA si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE; // show MAME but don't steal focus
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::cerr << "Failed to launch MAME (error " << GetLastError() << ")\n";
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess; // caller owns this handle
}

// Finds the main visible window belonging to a given process.
static HWND find_process_hwnd(HANDLE proc) {
    struct D { DWORD pid; HWND hwnd; };
    D data = { GetProcessId(proc), nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* d = reinterpret_cast<D*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == d->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
            d->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

// Waits up to timeout_ms for the MAME shared memory to appear.
static bool wait_for_shmem(int timeout_ms = 20000) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\RetroDepthFB4");
        if (h) { CloseHandle(h); return true; }
        Sleep(500);
        elapsed += 500;
    }
    return false;
}

static bool config_matches_system(const GameConfig& cfg, const std::string& system_name) {
    if (config_matches_cart_system(system_name, cfg.rom_name))
        return true;
    if (system_name == "cps2" || system_name == "cps1" || system_name == "konami" || system_name == "neogeo")
        return !is_sms_cart_rom_name(cfg.rom_name)
            && !is_nes_cart_rom_name(cfg.rom_name)
            && !is_snes_cart_rom_name(cfg.rom_name)
            && !is_genesis_cart_rom_name(cfg.rom_name)
            && !is_gb_cart_rom_name(cfg.rom_name)
            && !is_gbc_cart_rom_name(cfg.rom_name);
    return false;
}

static GameConfig make_default_config_for_system(const std::string& system_name, const std::string& game_name) {
    if (system_name == "cps2")
        return GameConfig::make_default_cps2(game_name);
    if (system_name == "cps1")
        return GameConfig::make_default_cps1(game_name);
    if (system_name == "konami")
        return GameConfig::make_default_konami(game_name);
    if (system_name == "nes")
        return GameConfig::make_default_nes(game_name);
    if (system_name == "sms")
        return GameConfig::make_default_sms(game_name);
    if (system_name == "snes")
        return GameConfig::make_default_snes(game_name);
    if (system_name == "genesis")
        return GameConfig::make_default_genesis(game_name);
    if (system_name == "gb")
        return GameConfig::make_default_gb(game_name);
    if (system_name == "gbc")
        return GameConfig::make_default_gbc(game_name);
    return GameConfig::make_default_neogeo(game_name);
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr <<
        "Usage:\n"
        "  retrodepth --game <name>          Load configs/<name>.json and launch MAME\n"
        "  retrodepth --config <path>        Load explicit config file and launch MAME\n"
        "  retrodepth --window <title>       Flat mode: attach to already-running window\n"
        "\n"
        "MAME path and ROMs path are read from configs/settings.json.\n";
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    try {
        bool preview_mode = false;
        bool spectator_mode = false;
        bool inspect_mode = false;
        bool dynamic_mode   = false;
        bool beta_depth_mode = false;
        bool density_mode   = false;
        bool motion_mode    = false;
        bool hide_mame_mode = false;
        uint32_t auto_exit_ms = 0;
        std::string game_name, config_path, window_title, system_name, romdir_override;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if ((a == "--game"   || a == "-g") && i+1 < argc) game_name   = argv[++i];
            else if ((a == "--config" || a == "-c") && i+1 < argc) config_path = argv[++i];
            else if ((a == "--window" || a == "-w") && i+1 < argc) window_title = argv[++i];
            else if (a == "--preview" || a == "-p") preview_mode = true;
            else if (a == "--spectator" || a == "--mirror") spectator_mode = true;
            else if (a == "--inspect") inspect_mode = true;
            else if (a == "--dynamic") dynamic_mode = true;
            else if (a == "--beta-depth") beta_depth_mode = true;
            else if (a == "--density") density_mode = true;
            else if (a == "--motion")    motion_mode    = true;
            else if (a == "--hide-mame") hide_mame_mode = true;
            else if ((a == "--system" || a == "-s") && i+1 < argc) system_name = argv[++i];
            else if (a == "--romdir" && i+1 < argc) romdir_override = argv[++i];
            else if (a == "--auto-exit-ms" && i+1 < argc) auto_exit_ms = (uint32_t)(std::max)(0, atoi(argv[++i]));
            else if (a == "--help" || a == "-h") { print_usage(); return 0; }
        }

        wchar_t exe_buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
        fs::path exe_path = exe_buf;
        fs::path exe_dir = exe_path.parent_path();
        if (game_name.empty() && config_path.empty() && window_title.empty())
            return run_launcher(exe_path);

        Settings settings = load_settings(exe_dir);
        if (!romdir_override.empty())
            settings.roms_path = romdir_override;
        if (!system_name.empty() && !is_system_enabled(system_name))
            throw std::runtime_error("System support is disabled by code toggle: " + system_name);

        GameConfig config;
        auto create_default_config = [&](const fs::path& dst_path) {
            config = make_default_config_for_system(system_name, game_name);
            config.config_path = dst_path.string();
            fs::create_directories(dst_path.parent_path());
            config.save();
            std::cout << "Auto-created config: " << config.config_path << "\n";
        };
        auto try_migrate_legacy_config = [&](const fs::path& dst_path) -> bool {
            if (game_name.empty())
                return false;
            if (system_name != "sms" && system_name != "snes" && system_name != "genesis"
                && system_name != "gb" && system_name != "gbc")
                return false;

            const fs::path target_abs = fs::absolute(dst_path).lexically_normal();
            for (const auto& legacy : { exe_dir / "configs" / (game_name + ".json"),
                                        fs::path("configs") / (game_name + ".json") }) {
                if (!fs::exists(legacy))
                    continue;
                if (fs::absolute(legacy).lexically_normal() == target_abs)
                    continue;
                try {
                    GameConfig legacy_cfg = GameConfig::load(legacy.string());
                    if (!config_matches_system(legacy_cfg, system_name))
                        continue;
                    legacy_cfg.config_path = dst_path.string();
                    fs::create_directories(dst_path.parent_path());
                    legacy_cfg.save();
                    config = std::move(legacy_cfg);
                    std::cout << "Migrated legacy config: " << legacy.string()
                              << " -> " << dst_path.string() << "\n";
                    return true;
                } catch (...) {
                }
            }
            return false;
        };

        if (!config_path.empty()) {
            fs::path requested = config_path;
            if (fs::exists(requested))
                config = GameConfig::load(requested.string());
            else if (!game_name.empty()) {
                if (!try_migrate_legacy_config(requested))
                    create_default_config(requested);
            } else {
                throw std::runtime_error("Cannot open config: " + requested.string());
            }
        } else if (!game_name.empty()) {
            fs::path p1 = exe_dir / "configs" / (game_name + ".json");
            fs::path p2 = fs::path("configs") / (game_name + ".json");
            if      (fs::exists(p1)) config = GameConfig::load(p1.string());
            else if (fs::exists(p2)) config = GameConfig::load(p2.string());
            else create_default_config(p1);
        } else if (!window_title.empty()) {
            config = GameConfig::make_flat(window_title);
        } else {
            print_usage();
            return 1;
        }

        std::cout << "RetroDepth\n"
                  << "  Game    : " << config.game << "\n"
                  << "  ROM     : " << config.rom_name << "\n"
                  << "  Window  : " << config.source_window_title << "\n"
                  << "  Layers  : " << config.layers.size() << "\n";
        if (inspect_mode)
            std::cout << "[main] inspect mode requested\n";

        // Launch MAME unless the user just wants to attach to an existing window
        HANDLE mame_proc = nullptr;
        if (window_title.empty() && !config.rom_name.empty()) {
            // Kill any stale MAME process left from a previous crashed session.
            {
                STARTUPINFOA si = {}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};
                std::string stale_exe = settings.mame_exe.empty()
                    ? std::string("rdmame.exe")
                    : fs::path(settings.mame_exe).filename().string();
                std::string kill_cmd = "taskkill /f /im " + stale_exe;
                if (CreateProcessA(nullptr, kill_cmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    WaitForSingleObject(pi.hProcess, 2000);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    Sleep(300); // let the OS clean up the handles
                }
            }
            mame_proc = launch_mame(settings, config.rom_name);
            if (mame_proc) {
                std::cout << "Waiting for MAME shared memory...\n";
                if (wait_for_shmem()) {
                    std::cout << "MAME ready.\n";
                } else {
                    std::cerr << "Warning: MAME shared memory not seen after 20 s, continuing anyway.\n";
                }
            }
        }

        // Find MAME's window so we can forward keyboard input to it
        HWND mame_hwnd = nullptr;
        if (mame_proc) {
            for (int i = 0; i < 10 && !mame_hwnd; ++i) {
                Sleep(300);
                mame_hwnd = find_process_hwnd(mame_proc);
            }
            if (mame_hwnd) {
                std::cout << "MAME window found, keyboard forwarding enabled.\n";
                if (hide_mame_mode) ShowWindow(mame_hwnd, SW_HIDE);
                // Auto-dismiss the "one or more ROMs incorrect" warning MAME
                // shows at startup.  The dialog blocks on a keypress; send
                // Return after a brief delay so MAME has had time to render it.
                HWND hwnd_copy = mame_hwnd;
                HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                    HWND hw = reinterpret_cast<HWND>(p);
                    Sleep(1500);
                    PostMessageA(hw, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                    Sleep(50);
                    PostMessageA(hw, WM_KEYUP,   VK_RETURN, 0xC01C0001);
                    return 0;
                }, reinterpret_cast<LPVOID>(hwnd_copy), 0, nullptr);
                if (t) CloseHandle(t);
            }
        }

        if (inspect_mode) {
            fs::path out_path = exe_dir / "diagnostics" / (config.rom_name + "_diagnostics.json");
            fs::create_directories(out_path.parent_path());
            {
                std::ofstream started(out_path.string() + ".started");
                started << "inspect started\n";
            }
            std::cerr << "[main] starting diagnostics recorder: " << out_path.string() << "\n";
            DiagnosticsRecorder::run(config, out_path, 60, 2);
            {
                std::ofstream done(out_path.string() + ".done");
                done << "inspect finished\n";
            }
            std::cerr << "[main] diagnostics recorder finished\n";
        } else if (preview_mode) {
            auto preview = std::make_unique<PreviewWindow>(std::move(config));
            if (auto_exit_ms > 0) preview->set_auto_exit_ms(auto_exit_ms);
            if (mame_hwnd) preview->set_mame_hwnd(mame_hwnd);
            if (dynamic_mode) {
                preview->set_dynamic_mode(true);
                preview->set_beta_depth(beta_depth_mode);
                if (density_mode) preview->set_density_scoring(true);
                if (motion_mode)  preview->set_motion_scoring(true);
            }
            preview->run();
        } else {
            auto app = std::make_unique<XrApp>(std::move(config));
            app->set_spectator_enabled(spectator_mode);
            app->set_hide_mame(hide_mame_mode);
            if (dynamic_mode) {
                app->set_dynamic_mode(true);
                app->set_beta_depth(beta_depth_mode);
                if (density_mode) app->set_density_scoring(true);
                if (motion_mode)  app->set_motion_scoring(true);
            }
            if (mame_hwnd) app->set_mame_hwnd(mame_hwnd);
            app->run();
        }

        // Clean up MAME process when VR session ends
        if (mame_proc) {
            TerminateProcess(mame_proc, 0);
            CloseHandle(mame_proc);
        }

        std::cout << "RetroDepth exited cleanly.\n";
        return 0;

    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        std::cerr << "Fatal error: " << msg << "\n";
        MessageBoxA(nullptr, msg.c_str(), "RetroDepth Fatal Error", MB_ICONERROR | MB_OK);
        return 1;
    }
}
