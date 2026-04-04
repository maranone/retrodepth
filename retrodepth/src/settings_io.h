#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct Settings {
    std::string mame_exe;
    std::string mame_args;
    std::string roms_path;
    std::string bios_path;
};

Settings load_settings(const std::filesystem::path& exe_dir);
void save_settings(const std::filesystem::path& exe_dir, const Settings& s);
std::vector<std::filesystem::path> split_rom_paths(const std::string& roms_path);
std::string join_rom_paths(const std::vector<std::filesystem::path>& paths);
