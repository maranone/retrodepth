#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include "game_config.h"
#include "palette_control.h"
#include "layer_processor.h"  // LayerFrame

// Live layer editor driven by keyboard hotkeys.
// Call poll_keys() + on_new_frame() once per frame.
class LayerEditor {
public:
    explicit LayerEditor(GameConfig& cfg);

    void poll_keys();
    void on_new_frame(const std::vector<LayerFrame>& frames);

    bool is_active()        const { return m_active; }
    int  get_selected()     const { return m_selected; }
    void set_selected(int i)      { m_selected = i; }
    bool blink_on()         const;
    bool is_solo()          const { return m_solo; }
    void set_solo(bool s)         { m_solo = s; }
    std::string get_overlay_text() const;

    // Palette routing
    void push_ctrl_update();
    void set_palette_group(int palette_index, int group);
    int  get_palette_group(int palette_index) const;
    void set_thumb_requested(bool v);

private:
    void fire_action(int action);

    GameConfig& m_cfg;
    bool m_active   = false;
    int  m_selected = 0;
    bool m_solo     = false;

    // Edge-triggered keys
    bool m_r_was_down = false;
    bool m_q_was_down = false;
    bool m_e_was_down = false;
    bool m_z_was_down = false;
    bool m_x_was_down = false;

    // Hold-to-repeat: W S D A V C  (indices 0-5)
    struct KeyRepeat {
        int       vkey      = 0;
        bool      was_down  = false;
        ULONGLONG next_fire = 0;
    };
    KeyRepeat m_kr[6];

    PaletteRouteWriter m_ctrl_writer;
    bool m_thumb_requested = false;

    static constexpr ULONGLONG k_initial_delay_ms = 250;
    static constexpr ULONGLONG k_repeat_ms        = 80;
    static constexpr float     k_depth_step       = 0.05f;
    static constexpr float     k_width_step       = 0.05f;
    static constexpr float     k_copy_spread      = 1.15f;
    static constexpr float     k_copy_compress    = 0.87f;
};
