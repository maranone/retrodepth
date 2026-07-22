#pragma once
#include <cstdint>

// Number of capturable hardware layers:
//   indices 0-3 = BG0-BG3 (snes9x BG array order)
//   index 4     = OBJ (sprites)
#define SNES9X_LAYER_COUNT 5

extern "C" {

const uint16_t* snes9x_get_layer_pixels(int layer, unsigned* out_stride);
const uint8_t*  snes9x_get_layer_mask(int layer, unsigned* out_stride);
void            snes9x_clear_layer_capture();
void            snes9x_set_layer_capture_mask(uint32_t mask);
uint32_t        snes9x_get_layer_capture_mask();
void            snes9x_layer_capture_put(int8_t layer_idx, uint32_t offset, uint16_t color);

} // extern "C"
