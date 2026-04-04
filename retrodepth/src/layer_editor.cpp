#include "layer_editor.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <iostream>

enum Action {
    ACT_DEPTH_INC = 0, ACT_DEPTH_DEC,
    ACT_WIDTH_INC,     ACT_WIDTH_DEC,
    ACT_SPREAD,        ACT_COMPRESS
};

LayerEditor::LayerEditor(GameConfig& cfg) : m_cfg(cfg) {
    m_kr[ACT_DEPTH_INC] = { 'W' };
    m_kr[ACT_DEPTH_DEC] = { 'S' };
    m_kr[ACT_WIDTH_INC] = { 'D' };
    m_kr[ACT_WIDTH_DEC] = { 'A' };
    m_kr[ACT_SPREAD]    = { 'V' };
    m_kr[ACT_COMPRESS]  = { 'C' };
    push_ctrl_update();
}

bool LayerEditor::blink_on() const {
    return (GetTickCount64() / 125) % 2 == 0;
}

void LayerEditor::push_ctrl_update() {
    m_ctrl_writer.write(m_cfg.palette_route, m_thumb_requested);
}

void LayerEditor::set_thumb_requested(bool v) {
    m_thumb_requested = v;
    push_ctrl_update();
}

void LayerEditor::set_palette_group(int palette_index, int group) {
    if (palette_index < 0 || palette_index > 255) return;
    if (group < 0 || group > 3) return;
    m_cfg.palette_route[palette_index] = (uint8_t)group;
    push_ctrl_update();
}

int LayerEditor::get_palette_group(int palette_index) const {
    if (palette_index < 0 || palette_index > 255) return 0;
    uint8_t g = m_cfg.palette_route[palette_index];
    return (g == 0xFF) ? 0 : (int)g;
}

void LayerEditor::on_new_frame(const std::vector<LayerFrame>& /*frames*/) {
    // No-op.
}

void LayerEditor::fire_action(int action) {
    if (m_cfg.layers.empty()) return;
    LayerConfig& lc = m_cfg.layers[m_selected];

    switch (action) {
    case ACT_DEPTH_INC:
        lc.depth_meters = std::max(0.1f, lc.depth_meters + k_depth_step);
        break;
    case ACT_DEPTH_DEC:
        lc.depth_meters = std::max(0.1f, lc.depth_meters - k_depth_step);
        break;
    case ACT_WIDTH_INC:
        lc.quad_width_meters = std::max(0.1f, lc.quad_width_meters + k_width_step);
        break;
    case ACT_WIDTH_DEC:
        lc.quad_width_meters = std::max(0.1f, lc.quad_width_meters - k_width_step);
        break;
    case ACT_SPREAD:
        for (auto& c : lc.copies) c *= k_copy_spread;
        break;
    case ACT_COMPRESS:
        for (auto& c : lc.copies) c = std::max(0.001f, c * k_copy_compress);
        break;
    }
}

void LayerEditor::poll_keys() {
    bool r_down = (GetAsyncKeyState('R') & 0x8000) != 0;
    if (r_down && !m_r_was_down) {
        m_active = !m_active;
        if (!m_active) {
            m_cfg.save();
            std::cout << "[editor] saved to " << m_cfg.config_path << "\n";
        } else {
            m_selected = std::min(m_selected, (int)m_cfg.layers.size() - 1);
            std::cout << "[editor] edit mode ON\n";
        }
    }
    m_r_was_down = r_down;

    if (!m_active || m_cfg.layers.empty()) return;

    bool q_down = (GetAsyncKeyState('Q') & 0x8000) != 0;
    if (q_down && !m_q_was_down)
        m_selected = (m_selected - 1 + (int)m_cfg.layers.size()) % (int)m_cfg.layers.size();
    m_q_was_down = q_down;

    bool e_down = (GetAsyncKeyState('E') & 0x8000) != 0;
    if (e_down && !m_e_was_down)
        m_selected = (m_selected + 1) % (int)m_cfg.layers.size();
    m_e_was_down = e_down;

    bool z_down = (GetAsyncKeyState('Z') & 0x8000) != 0;
    if (z_down && !m_z_was_down) {
        auto& copies = m_cfg.layers[m_selected].copies;
        copies.push_back(copies.empty() ? 0.01f : copies.back() + 0.01f);
    }
    m_z_was_down = z_down;

    bool x_down = (GetAsyncKeyState('X') & 0x8000) != 0;
    if (x_down && !m_x_was_down) {
        auto& copies = m_cfg.layers[m_selected].copies;
        if (!copies.empty()) copies.pop_back();
    }
    m_x_was_down = x_down;

    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 6; ++i) {
        bool down = (GetAsyncKeyState(m_kr[i].vkey) & 0x8000) != 0;
        if (down) {
            if (!m_kr[i].was_down) {
                fire_action(i);
                m_kr[i].next_fire = now + k_initial_delay_ms;
            } else if (now >= m_kr[i].next_fire) {
                fire_action(i);
                m_kr[i].next_fire = now + k_repeat_ms;
            }
        }
        m_kr[i].was_down = down;
    }
}

std::string LayerEditor::get_overlay_text() const {
    std::string out;
    out += "== LAYER EDITOR ==\n";
    out += "Q/E:layer  W/S:depth\n";
    out += "A/D:width  Z/X:copies\n";
    out += "V/C:spread  R:save+exit\n";
    out += "------------------\n";

    char buf[64];
    for (int i = 0; i < (int)m_cfg.layers.size(); ++i) {
        const auto& lc = m_cfg.layers[i];
        char id7[8];
        snprintf(id7, sizeof(id7), "%-7.7s", lc.id.c_str());
        snprintf(buf, sizeof(buf), "%c%s d=%.2f w=%.2f c%d",
            (i == m_selected) ? '>' : ' ',
            id7,
            lc.depth_meters,
            lc.quad_width_meters,
            (int)lc.copies.size());
        out += buf;
        out += "\n";
    }

    return out;
}
