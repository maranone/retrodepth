#include "game_config.h"
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static ExtractionType parse_extraction_type(const std::string& s) {
    if (s == "full_frame")          return ExtractionType::FullFrame;
    if (s == "region")              return ExtractionType::Region;
    if (s == "color_key")           return ExtractionType::ColorKey;
    if (s == "color_key_inverted")  return ExtractionType::ColorKeyInverted;
    throw std::runtime_error("Unknown extraction type: " + s);
}

static std::string extraction_type_to_string(ExtractionType t) {
    switch (t) {
    case ExtractionType::FullFrame:        return "full_frame";
    case ExtractionType::Region:           return "region";
    case ExtractionType::ColorKey:         return "color_key";
    case ExtractionType::ColorKeyInverted: return "color_key_inverted";
    }
    return "full_frame";
}

static bool looks_like_snes_rom(const std::string& rom_name) {
    static const std::string k_prefix = "snes -cart \"";
    return rom_name.size() >= k_prefix.size()
        && rom_name.compare(0, k_prefix.size(), k_prefix) == 0;
}

static bool looks_like_sms_rom(const std::string& rom_name) {
    static const std::string k_prefix_a = "sms -cart \"";
    static const std::string k_prefix_b = "sms1 -cart \"";
    static const std::string k_prefix_c = "smsj -cart \"";
    static const std::string k_prefix_d = "smskr -cart \"";
    return (rom_name.size() >= k_prefix_a.size()
            && rom_name.compare(0, k_prefix_a.size(), k_prefix_a) == 0)
        || (rom_name.size() >= k_prefix_b.size()
            && rom_name.compare(0, k_prefix_b.size(), k_prefix_b) == 0)
        || (rom_name.size() >= k_prefix_c.size()
            && rom_name.compare(0, k_prefix_c.size(), k_prefix_c) == 0)
        || (rom_name.size() >= k_prefix_d.size()
            && rom_name.compare(0, k_prefix_d.size(), k_prefix_d) == 0);
}

static bool looks_like_genesis_rom(const std::string& rom_name) {
    static const std::string k_prefix = "genesis -cart \"";
    return rom_name.size() >= k_prefix.size()
        && rom_name.compare(0, k_prefix.size(), k_prefix) == 0;
}

void even_spread_layer_depths(std::vector<LayerConfig>& layers) {
    const int n = (int)layers.size();
    if (n < 2)
        return;

    float mn = layers[0].depth_meters;
    float mx = layers[0].depth_meters;
    for (const auto& lc : layers) {
        mn = std::min(mn, lc.depth_meters);
        mx = std::max(mx, lc.depth_meters);
    }
    if (mx - mn < 0.1f)
        mx = mn + 0.5f * (n - 1);

    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i)
        idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return layers[a].depth_meters > layers[b].depth_meters;
    });

    for (int i = 0; i < n; ++i) {
        float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        float d = mx + t * (mn - mx);
        layers[idx[i]].depth_meters = d < 0.05f ? 0.05f : d;
    }
}

static bool is_legacy_snes_layout(const GameConfig& cfg) {
    if (!looks_like_snes_rom(cfg.rom_name) || cfg.layers.size() != 5)
        return false;
    static const char* k_ids[] = { "background", "bg3", "bg2", "bg1", "sprites" };
    for (int i = 0; i < 5; ++i) {
        if (cfg.layers[i].id != k_ids[i])
            return false;
    }
    return true;
}

static void upgrade_legacy_snes_layout(GameConfig& cfg) {
    if (!is_legacy_snes_layout(cfg))
        return;

    float far_d = cfg.layers.front().depth_meters;
    float near_d = cfg.layers.back().depth_meters;
    float width = cfg.layers.front().quad_width_meters;

    static const char* k_ids[] = {
        "background",
        "bg3",
        "bg2_low",
        "bg2_high",
        "bg1_low",
        "bg1_high",
        "sprites_low",
        "sprites_high"
    };

    std::vector<LayerConfig> upgraded;
    upgraded.reserve(8);
    for (int i = 0; i < 8; ++i) {
        float t = (8 > 1) ? (float)i / 7.0f : 0.0f;
        LayerConfig lc;
        lc.id = k_ids[i];
        lc.depth_meters = far_d + t * (near_d - far_d);
        lc.quad_width_meters = width;
        lc.extraction_type = ExtractionType::FullFrame;

        const LayerConfig* src = nullptr;
        if (i <= 1) src = &cfg.layers[i];
        else if (i <= 3) src = &cfg.layers[2];
        else if (i <= 5) src = &cfg.layers[3];
        else             src = &cfg.layers[4];
        lc.copies = src->copies;
        upgraded.push_back(std::move(lc));
    }
    cfg.layers = std::move(upgraded);
}

static bool is_legacy_genesis_layout(const GameConfig& cfg) {
    if (!looks_like_genesis_rom(cfg.rom_name) || cfg.layers.size() != 4)
        return false;
    static const char* k_ids[] = { "background", "plane_b", "plane_a", "sprites" };
    for (int i = 0; i < 4; ++i) {
        if (cfg.layers[i].id != k_ids[i])
            return false;
    }
    return true;
}

static bool is_old_default_genesis_7bucket_layout(const GameConfig& cfg) {
    if (!looks_like_genesis_rom(cfg.rom_name) || cfg.layers.size() != 7)
        return false;

    static const struct {
        const char* id;
        float depth;
    } k_old[] = {
        { "background",   2.00f },
        { "plane_b_low",  1.82f },
        { "plane_b_high", 1.63f },
        { "plane_a_low",  1.45f },
        { "plane_a_high", 1.27f },
        { "sprites_low",  1.08f },
        { "sprites_high", 0.90f },
    };

    for (int i = 0; i < 7; ++i) {
        if (cfg.layers[i].id != k_old[i].id)
            return false;
        if (std::fabs(cfg.layers[i].depth_meters - k_old[i].depth) > 0.02f)
            return false;
    }
    return true;
}

static void upgrade_legacy_genesis_layout(GameConfig& cfg) {
    if (!is_legacy_genesis_layout(cfg))
        return;

    float far_d = cfg.layers.front().depth_meters;
    float near_d = cfg.layers.back().depth_meters;
    float width = cfg.layers.front().quad_width_meters;

    static const char* k_ids[] = {
        "background",
        "plane_b_low",
        "plane_b_high",
        "plane_a_low",
        "plane_a_high",
        "sprites_low",
        "sprites_high"
    };

    std::vector<LayerConfig> upgraded;
    upgraded.reserve(7);
    for (int i = 0; i < 7; ++i) {
        float t = (7 > 1) ? (float)i / 6.0f : 0.0f;
        LayerConfig lc;
        lc.id = k_ids[i];
        lc.depth_meters = far_d + t * (near_d - far_d);
        lc.quad_width_meters = width;
        lc.extraction_type = ExtractionType::FullFrame;

        const LayerConfig* src = nullptr;
        if (i == 0)      src = &cfg.layers[0];
        else if (i <= 2) src = &cfg.layers[1];
        else if (i <= 4) src = &cfg.layers[2];
        else             src = &cfg.layers[3];
        lc.copies = src->copies;
        upgraded.push_back(std::move(lc));
    }
    cfg.layers = std::move(upgraded);
}

static void upgrade_old_default_genesis_7bucket_layout(GameConfig& cfg) {
    if (!is_old_default_genesis_7bucket_layout(cfg))
        return;

    // Global Genesis default tuned from Streets of Rage 2:
    // low-priority sprites sit slightly behind plane_a_high.
    cfg.layers[5].depth_meters = 1.28f;
}

GameConfig GameConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open config: " + path);

    json j;
    f >> j;

    GameConfig cfg;
    cfg.game                 = j.value("game", "");
    cfg.rom_name             = j.value("rom_name", cfg.game);
    cfg.source_window_title  = j.value("source_window_title", cfg.game);

    if (j.contains("virtual_screen")) {
        cfg.virtual_width  = j["virtual_screen"].value("width",  320);
        cfg.virtual_height = j["virtual_screen"].value("height", 240);
    }
    cfg.quad_y_meters = j.value("quad_y_meters", 1.6f);

    // Load palette route (256-entry array; all zeros = all grp0 by default)
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));
    if (j.contains("palette_route")) {
        const auto& pr = j["palette_route"];
        for (int i = 0; i < 256 && i < (int)pr.size(); ++i)
            cfg.palette_route[i] = (uint8_t)pr[i].get<int>();
    }

    for (const auto& jl : j.value("layers", json::array())) {
        LayerConfig layer;
        layer.id               = jl.value("id", "layer");
        layer.depth_meters      = jl.value("depth_meters",      1.0f);
        layer.quad_width_meters = jl.value("quad_width_meters", 2.0f);
        if (jl.contains("copies"))
            layer.copies = jl["copies"].get<std::vector<float>>();

        if (jl.contains("extraction")) {
            const auto& ex = jl["extraction"];
            layer.extraction_type = parse_extraction_type(ex.value("type", "full_frame"));

            if (ex.contains("rect")) {
                auto r = ex["rect"].get<std::vector<int>>();
                layer.rect = {r[0], r[1], r[2], r[3]};
            }
            if (ex.contains("color")) {
                auto c = ex["color"].get<std::vector<int>>();
                layer.color = {
                    static_cast<uint8_t>(c[0]),
                    static_cast<uint8_t>(c[1]),
                    static_cast<uint8_t>(c[2])
                };
            }
            layer.tolerance = ex.value("tolerance", 8);
        }

        cfg.layers.push_back(std::move(layer));
    }

    // Migration: replace old pal_g0-7/spr_chars/grp0-2 layers with grp0-3.
    {
        std::vector<int> old_grp_indices;
        int spr_chars_idx = -1;
        for (int i = 0; i < (int)cfg.layers.size(); ++i) {
            const std::string& id = cfg.layers[i].id;
            bool is_old = (id.size() >= 5 && id.substr(0, 5) == "pal_g")
                       || (id.size() >= 3 && id.substr(0, 3) == "grp");
            if (is_old) old_grp_indices.push_back(i);
            else if (id == "spr_chars") spr_chars_idx = i;
        }

        // Need migration if we have <4 grp layers or only old pal_g layers
        bool need_migrate = !old_grp_indices.empty();
        bool has_grp3 = false;
        for (int i : old_grp_indices)
            if (cfg.layers[i].id == "grp3") { has_grp3 = true; break; }
        if (has_grp3 && (int)old_grp_indices.size() == 4)
            need_migrate = false; // already up to date

        if (need_migrate && !old_grp_indices.empty()) {
            float d0 = cfg.layers[old_grp_indices.front()].depth_meters;
            float d1 = cfg.layers[old_grp_indices.back()].depth_meters;
            float w0 = cfg.layers[old_grp_indices.front()].quad_width_meters;
            float w1 = cfg.layers[old_grp_indices.back()].quad_width_meters;

            std::vector<LayerConfig> grp_layers(4);
            for (int g = 0; g < 4; ++g) {
                float t = (float)g / 3.0f;
                grp_layers[g].id                = "grp" + std::to_string(g);
                grp_layers[g].depth_meters      = d0 + t * (d1 - d0);
                grp_layers[g].quad_width_meters = w0 + t * (w1 - w0);
                grp_layers[g].extraction_type   = ExtractionType::FullFrame;
                int src = (int)old_grp_indices.size() * g / 4;
                if (src < (int)old_grp_indices.size())
                    grp_layers[g].copies = cfg.layers[old_grp_indices[src]].copies;
            }

            std::vector<LayerConfig> new_layers;
            for (int i = 0; i < (int)cfg.layers.size(); ++i) {
                bool skip = false;
                for (int idx : old_grp_indices) if (i == idx) { skip = true; break; }
                if (skip || i == spr_chars_idx) continue;
                new_layers.push_back(cfg.layers[i]);
            }
            int insert_pos = (!new_layers.empty()) ? 1 : 0;
            for (int g = 3; g >= 0; --g)
                new_layers.insert(new_layers.begin() + insert_pos, grp_layers[g]);
            cfg.layers = std::move(new_layers);
        }
    }

    // Fallback: if no layers defined, show the whole frame flat
    if (cfg.layers.empty()) {
        LayerConfig flat;
        flat.id = "full";
        flat.depth_meters = 1.5f;
        flat.quad_width_meters = 2.0f;
        flat.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(flat);
    }

    // Upgrade old single-layer Neo Geo configs to the standard split-layer layout.
    if (cfg.layers.size() == 1 && cfg.layers[0].id == "full") {
        GameConfig neo = GameConfig::make_default_neogeo(cfg.game.empty() ? cfg.rom_name : cfg.game);
        neo.game = cfg.game.empty() ? neo.game : cfg.game;
        neo.rom_name = cfg.rom_name.empty() ? neo.rom_name : cfg.rom_name;
        neo.source_window_title = cfg.source_window_title.empty() ? neo.source_window_title : cfg.source_window_title;
        neo.virtual_width = cfg.virtual_width;
        neo.virtual_height = cfg.virtual_height;
        neo.quad_y_meters = cfg.quad_y_meters;
        memcpy(neo.palette_route, cfg.palette_route, sizeof(cfg.palette_route));
        neo.config_path = path;
        neo.save();
        return neo;
    }

    upgrade_legacy_snes_layout(cfg);
    upgrade_legacy_genesis_layout(cfg);
    upgrade_old_default_genesis_7bucket_layout(cfg);

    cfg.config_path = path;
    return cfg;
}

void GameConfig::save() const {
    if (config_path.empty()) return;

    json j;
    j["game"]                = game;
    j["rom_name"]            = rom_name;
    j["source_window_title"] = source_window_title;
    j["virtual_screen"]      = { {"width", virtual_width}, {"height", virtual_height} };
    j["quad_y_meters"]       = quad_y_meters;
    json pr = json::array();
    for (int i = 0; i < 256; ++i) pr.push_back((int)palette_route[i]);
    j["palette_route"] = pr;

    json layers_arr = json::array();
    for (const auto& lc : layers) {
        json jl;
        jl["id"]               = lc.id;
        jl["depth_meters"]     = lc.depth_meters;
        jl["quad_width_meters"]= lc.quad_width_meters;
        jl["copies"]           = lc.copies;

        json ex;
        ex["type"] = extraction_type_to_string(lc.extraction_type);
        if (lc.extraction_type == ExtractionType::Region)
            ex["rect"] = lc.rect;
        if (lc.extraction_type == ExtractionType::ColorKey ||
            lc.extraction_type == ExtractionType::ColorKeyInverted) {
            ex["color"]     = lc.color;
            ex["tolerance"] = lc.tolerance;
        }
        jl["extraction"] = ex;

        layers_arr.push_back(jl);
    }
    j["layers"] = layers_arr;

    std::ofstream f(config_path);
    f << j.dump(2) << "\n";
}

GameConfig GameConfig::make_flat(const std::string& window_title) {
    GameConfig cfg;
    cfg.game                = window_title;
    cfg.rom_name            = window_title;
    cfg.source_window_title = window_title;
    LayerConfig flat;
    flat.id = "full";
    flat.depth_meters = 1.5f;
    flat.quad_width_meters = 2.0f;
    flat.extraction_type = ExtractionType::FullFrame;
    cfg.layers.push_back(flat);
    return cfg;
}

// CPS1: hardware layers — background fill, scroll3 (bg_tilemap[2]), scroll2,
// scroll1 (bg_tilemap[0]), sprites.  Depths tuned for an arcade cabinet feel.
GameConfig GameConfig::make_default_cps1(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = game_name;
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 384;
    cfg.virtual_height      = 224;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "background", 2.00f, 2.56f },
        { "scroll3",    1.80f, 2.56f },
        { "scroll2",    1.50f, 2.56f },
        { "scroll1",    1.20f, 2.56f },
        { "sprites",    1.00f, 2.56f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

// CPS2 uses the same hardware layer structure as CPS1 (scroll1/2/3 + sprites).
GameConfig GameConfig::make_default_cps2(const std::string& game_name) {
    GameConfig cfg = make_default_cps1(game_name);
    cfg.virtual_width  = 384;
    cfg.virtual_height = 224;
    return cfg;
}

// Konami K052109 hardware (TMNT, Simpsons, X-Men, etc.)
// Layers: scroll2 (opaque bg), scroll1 (mid), scroll0 (fg), sprites.
GameConfig GameConfig::make_default_konami(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = game_name;
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 320;
    cfg.virtual_height      = 240;

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "scroll2", 2.00f, 2.56f },
        { "scroll1", 1.60f, 2.56f },
        { "scroll0", 1.20f, 2.56f },
        { "sprites", 1.00f, 2.56f },
    };
    for (const auto& l : k_layers) {
        LayerConfig layer;
        layer.id               = l.id;
        layer.depth_meters     = l.depth;
        layer.quad_width_meters = l.width;
        cfg.layers.push_back(std::move(layer));
    }
    return cfg;
}

GameConfig GameConfig::make_default_nes(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = "nes -cart \"" + game_name + "\"";
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 256;
    cfg.virtual_height      = 240;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "backdrop",       2.00f, 2.56f },
        { "background",     1.52f, 2.56f },
        { "sprites_behind", 1.28f, 2.56f },
        { "sprites_front",  1.02f, 2.56f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_sms(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = "sms1 -cart \"" + game_name + "\"";
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 256;
    cfg.virtual_height      = 192;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "backdrop",  2.00f, 2.56f },
        { "bg_low",    1.62f, 2.56f },
        { "bg_high",   1.28f, 2.56f },
        { "sprites",   0.96f, 2.56f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_snes(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = "snes -cart \"" + game_name + "\"";
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 512;   // MAME doubles SNES width
    cfg.virtual_height      = 224;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "background",   2.00f, 2.56f },
        { "bg3",          1.84f, 2.56f },
        { "bg2_low",      1.69f, 2.56f },
        { "bg2_high",     1.53f, 2.56f },
        { "bg1_low",      1.37f, 2.56f },
        { "bg1_high",     1.21f, 2.56f },
        { "sprites_low",  1.06f, 2.56f },
        { "sprites_high", 0.90f, 2.56f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

// Genesis/MD: stable VR buckets following VDP priority bands.
// Current MAME export is still 4 layers, so ShmemReader auto-spreads them
// across this range until the driver exposes the split priorities explicitly.
GameConfig GameConfig::make_default_genesis(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = "genesis -cart \"" + game_name + "\"";
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 320;
    cfg.virtual_height      = 224;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "background",    2.00f, 2.56f },
        { "plane_b_low",   1.82f, 2.56f },
        { "plane_b_high",  1.63f, 2.56f },
        { "plane_a_low",   1.45f, 2.56f },
        { "plane_a_high",  1.27f, 2.56f },
        { "sprites_low",   1.28f, 2.56f },
        { "sprites_high",  0.90f, 2.56f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_gb(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = "gameboy -cart \"" + game_name + "\"";
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 160;
    cfg.virtual_height      = 144;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct { const char* id; float depth; float width; } k_layers[] = {
        { "backdrop",       2.00f, 2.40f },
        { "background",     1.58f, 2.40f },
        { "window",         1.34f, 2.40f },
        { "sprites_behind", 1.14f, 2.40f },
        { "sprites_front",  0.94f, 2.40f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}

GameConfig GameConfig::make_default_gbc(const std::string& game_name) {
    GameConfig cfg = make_default_gb(game_name);
    cfg.rom_name = "gbcolor -cart \"" + game_name + "\"";
    return cfg;
}

GameConfig GameConfig::make_default_neogeo(const std::string& game_name) {
    GameConfig cfg;
    cfg.game                = game_name;
    cfg.rom_name            = game_name;
    cfg.source_window_title = game_name;
    cfg.virtual_width       = 320;
    cfg.virtual_height      = 240;
    cfg.quad_y_meters       = 1.6f;
    memset(cfg.palette_route, 0, sizeof(cfg.palette_route));

    static const struct {
        const char* id;
        float depth;
        float width;
    } k_layers[] = {
        { "background", 1.50f, 2.25f },
        { "grp0",       1.43f, 2.25f },
        { "grp1",       1.36f, 2.25f },
        { "grp2",       1.29f, 2.25f },
        { "grp3",       1.22f, 2.25f },
        { "fix",        1.15f, 2.25f },
    };

    cfg.layers.clear();
    for (const auto& src : k_layers) {
        LayerConfig lc;
        lc.id = src.id;
        lc.depth_meters = src.depth;
        lc.quad_width_meters = src.width;
        lc.extraction_type = ExtractionType::FullFrame;
        cfg.layers.push_back(std::move(lc));
    }
    return cfg;
}
