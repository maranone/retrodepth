#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

static constexpr uint32_t RD_CTRL_MAGIC = 0x52445052u; // 'RDPR'

#pragma pack(push, 1)
struct RDPaletteRoute {
    uint32_t magic;
    uint8_t  route[256];
    uint8_t  thumb_requested; // 1 = editor open, render thumbnails
    uint8_t  _pad[3];
};
#pragma pack(pop)

class PaletteRouteWriter {
public:
    PaletteRouteWriter();
    ~PaletteRouteWriter();
    void write(const uint8_t route[256], bool thumb_requested = false);
    // Write a single palette entry without touching the rest of the struct.
    // Only valid after write() has been called at least once (magic + thumb set).
    void write_entry(int palette_index, uint8_t group);
    // Toggle only the thumb_requested byte (leaves route and magic intact).
    void set_thumb_requested(bool v);
private:
    HANDLE m_mapping = nullptr;
    void*  m_view    = nullptr;
};
