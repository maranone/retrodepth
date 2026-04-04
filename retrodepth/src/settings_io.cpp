#include "settings_io.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string trim_copy(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

}

Settings load_settings(const fs::path& exe_dir) {
    Settings s;
    s.bios_path = (exe_dir / "bios").string();
    s.roms_path = (exe_dir / "roms").string();
    for (const auto& p : { exe_dir / "configs/settings.json", fs::path("configs/settings.json") }) {
        std::ifstream f(p);
        if (!f.is_open()) continue;
        json j;
        f >> j;
        s.mame_exe  = j.value("mame_exe", "");
        s.mame_args = j.value("mame_args", "");
        s.roms_path = j.value("roms_path", s.roms_path);
        if (j.contains("bios_path"))
            s.bios_path = j.value("bios_path", s.bios_path);
        // Resolve relative paths against exe_dir so they work regardless of cwd.
        {
            auto parts = split_rom_paths(s.roms_path);
            for (auto& rp : parts)
                if (rp.is_relative()) rp = exe_dir / rp;
            s.roms_path = join_rom_paths(parts);
        }
        if (fs::path(s.bios_path).is_relative())
            s.bios_path = (exe_dir / s.bios_path).string();
        if (!s.mame_exe.empty() && fs::path(s.mame_exe).is_relative())
            s.mame_exe = (exe_dir / s.mame_exe).string();
        return s;
    }
    return s;
}

void save_settings(const fs::path& exe_dir, const Settings& s) {
    fs::path cfg_dir = exe_dir / "configs";
    fs::create_directories(cfg_dir);
    json j;
    j["mame_exe"] = s.mame_exe;
    j["mame_args"] = s.mame_args;
    j["roms_path"] = s.roms_path;
    j["bios_path"] = s.bios_path;
    std::ofstream f(cfg_dir / "settings.json");
    f << j.dump(2) << "\n";
}

std::vector<fs::path> split_rom_paths(const std::string& roms_path) {
    std::vector<fs::path> out;
    std::istringstream in(roms_path);
    std::string part;
    while (std::getline(in, part, ';')) {
        part = trim_copy(part);
        if (part.empty()) continue;
        fs::path candidate = part;
        if (std::find(out.begin(), out.end(), candidate) == out.end())
            out.push_back(std::move(candidate));
    }
    return out;
}

std::string join_rom_paths(const std::vector<fs::path>& paths) {
    std::string out;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        if (!out.empty()) out += ';';
        out += path.string();
    }
    return out;
}
