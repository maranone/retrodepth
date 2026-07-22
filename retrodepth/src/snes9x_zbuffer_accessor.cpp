#include "snes9x.h"
#include "gfx.h"
#include <cstdint>
#include <cstddef>

extern "C"
const uint8_t* snes9x_get_zbuffer_for_frame(const void* video_data, unsigned* out_stride) {
    if (!GFX.ZBuffer || !GFX.Screen || !video_data) {
        if (out_stride) *out_stride = 0;
        return nullptr;
    }
    const unsigned stride = GFX.RealPPL;
    if (out_stride) *out_stride = stride;
    const ptrdiff_t byte_offset =
        static_cast<const uint8_t*>(video_data) -
        reinterpret_cast<const uint8_t*>(GFX.Screen);
    const ptrdiff_t row_offset =
        (byte_offset >= 0 && GFX.Pitch > 0)
            ? (byte_offset / static_cast<ptrdiff_t>(GFX.Pitch)) : 0;
    return GFX.ZBuffer + row_offset * stride;
}
