#pragma once
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <utility>

enum class ExtractionType {
    FullFrame,          // pass the whole frame through (single-layer fallback)
    Region,             // crop to a rect; everything outside is transparent
    ColorKey,           // make pixels matching 'color' transparent (remove bg)
    ColorKeyInverted,   // keep only pixels matching 'color' (isolate a layer)
};

struct LayerConfig {
    std::string id;
    float depth_meters      = 1.0f;  // distance from viewer (larger = further)
    float quad_width_meters = 2.0f;  // physical width of the quad in world space
    std::vector<float> copies;  // extra copies at depth_meters + each offset (meters)

    ExtractionType extraction_type = ExtractionType::FullFrame;

    // Region extraction: [x, y, width, height] in source pixels
    std::array<int, 4> rect = {0, 0, 0, 0};

    // ColorKey / ColorKeyInverted
    std::array<uint8_t, 3> color = {0, 0, 0};
    int tolerance = 8;
};

struct GameConfig {
    std::string game;
    std::string rom_name;             // MAME ROM name (e.g. "1942"); defaults to game
    std::string source_window_title;  // window title to search for (e.g. "1942")
    int virtual_width  = 320;
    int virtual_height = 240;
    float quad_y_meters = 1.6f;  // vertical center of all quads in STAGE space (floor=0)
    std::vector<LayerConfig> layers;

    // NeoGeo palette→group routing (256 entries).
    // palette_route[i] = 0-3 → grp0-grp3; 0xFF = default (grp0).
    uint8_t palette_route[256] = {};

    std::string config_path; // absolute path used by save(); empty = no-op

    // Loads and parses configs/<game>.json (or any explicit path).
    static GameConfig load(const std::string& path);

    // Returns a single full-frame layer — useful for Phase 1 flat display.
    static GameConfig make_flat(const std::string& window_title);
    static GameConfig make_default_neogeo(const std::string& game_name);
    static GameConfig make_default_cps1(const std::string& game_name);
    static GameConfig make_default_cps2(const std::string& game_name);
    static GameConfig make_default_konami(const std::string& game_name);
    static GameConfig make_default_nes(const std::string& game_name);
    static GameConfig make_default_sms(const std::string& game_name);
    static GameConfig make_default_snes(const std::string& game_name);
    static GameConfig make_default_genesis(const std::string& game_name);
    static GameConfig make_default_gb(const std::string& game_name);
    static GameConfig make_default_gbc(const std::string& game_name);

    // Writes current state back to config_path. No-op if config_path is empty.
    void save() const;
};

// Redistribute current layer depths uniformly between the farthest and nearest
// layers while preserving the current depth ordering.
void even_spread_layer_depths(std::vector<LayerConfig>& layers);
