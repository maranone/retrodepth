#include "palette_control.h"

PaletteRouteWriter::PaletteRouteWriter() {
    m_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(RDPaletteRoute),
        "Local\\RetroDepthControl");
    if (m_mapping)
        m_view = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS,
                               0, 0, sizeof(RDPaletteRoute));
}

PaletteRouteWriter::~PaletteRouteWriter() {
    if (m_view)    { UnmapViewOfFile(m_view);  m_view    = nullptr; }
    if (m_mapping) { CloseHandle(m_mapping);   m_mapping = nullptr; }
}

void PaletteRouteWriter::write(const uint8_t route[256], bool thumb_requested) {
    if (!m_view) return;
    RDPaletteRoute ctrl{};
    ctrl.magic           = RD_CTRL_MAGIC;
    ctrl.thumb_requested = thumb_requested ? 1u : 0u;
    memcpy(ctrl.route, route, 256);
    memcpy(m_view, &ctrl, sizeof(RDPaletteRoute));
}

void PaletteRouteWriter::write_entry(int palette_index, uint8_t group) {
    if (!m_view || palette_index < 0 || palette_index > 255) return;
    // Write only the single route byte — magic and thumb_requested are left intact.
    uint8_t* base = static_cast<uint8_t*>(m_view);
    base[offsetof(RDPaletteRoute, route) + palette_index] = group;
}

void PaletteRouteWriter::set_thumb_requested(bool v) {
    if (!m_view) return;
    uint8_t* base = static_cast<uint8_t*>(m_view);
    base[offsetof(RDPaletteRoute, thumb_requested)] = v ? 1u : 0u;
}
