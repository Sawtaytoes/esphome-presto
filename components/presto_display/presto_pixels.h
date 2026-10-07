#pragma once
#include <cstddef>
#include <cstdint>
#include "miniz_tinfl.h"
namespace esphome::presto_display {
bool decode_pixels(const uint8_t *packet, size_t received, uint32_t presented_frame, uint8_t *patch,
                   tinfl_decompressor *inflater,
                   void (*copy_pixels)(uint8_t *, const uint8_t *, size_t) = nullptr);
}
