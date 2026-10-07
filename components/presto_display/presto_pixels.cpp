#include "presto_pixels.h"
#include <cstring>
#include <memory>
#include <new>
namespace esphome::presto_display {
static uint16_t read16(const uint8_t *p) {
  return uint16_t(p[0]) << 8 | p[1];
}
bool decode_pixels(const uint8_t *packet, size_t received, uint32_t presented_frame, uint8_t *patch,
                   tinfl_decompressor *inflater,
                   void (*copy_pixels)(uint8_t *, const uint8_t *, size_t)) {
  if (!copy_pixels) {
    copy_pixels = [](uint8_t *to, const uint8_t *from, size_t n) { memcpy(to, from, n); };
  }
  if (received <= 13) {
    return false;
  }
  const uint32_t base =
      uint32_t(packet[0]) << 24 | uint32_t(packet[1]) << 16 | uint32_t(packet[2]) << 8 | packet[3];
  const size_t x = read16(packet + 4), y = read16(packet + 6);
  const size_t width = read16(packet + 8), height = read16(packet + 10);
  if (!width || !height || x + width > 480 || y + height > 480 ||
      (base && base != uint32_t(presented_frame)) ||
      (!base && (x || y || width != 480 || height != 480))) {
    return false;
  }
  const size_t expected = width * height * 2;
  const uint8_t *body = packet + 13;
  size_t input = received - 13;
  if (packet[12] == 0) {
    // Inflate into a SRAM dictionary, then commit bounded output chunks.
    // Byte-at-a-time writes into memory-mapped PSRAM monopolize QMI.
    std::unique_ptr<uint8_t[]> dictionary(new (std::nothrow) uint8_t[TINFL_LZ_DICT_SIZE]);
    if (!dictionary) {
      return false;
    }
    // Large artwork must not leave miniz reading PSRAM one byte at a time.
    // Refill a small SRAM input window independently of its 32 KiB dictionary.
    constexpr size_t INPUT_WINDOW = 4096;
    std::unique_ptr<uint8_t[]> cached_input(new (std::nothrow) uint8_t[INPUT_WINDOW]);
    if (!cached_input) {
      return false;
    }
    tinfl_init(inflater);
    size_t consumed = 0, produced = 0, position = 0;
    size_t window_size = 0, window_position = 0;
    for (;;) {
      if (window_position == window_size) {
        window_size = input - consumed < INPUT_WINDOW ? input - consumed : INPUT_WINDOW;
        window_position = 0;
        copy_pixels(cached_input.get(), body + consumed, window_size);
      }
      size_t in_count = window_size - window_position;
      size_t out_count = TINFL_LZ_DICT_SIZE - position;
      const uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
                             (consumed + in_count < input ? TINFL_FLAG_HAS_MORE_INPUT : 0);
      const auto status =
          tinfl_decompress(inflater, cached_input.get() + window_position, &in_count,
                           dictionary.get(), dictionary.get() + position, &out_count, flags);
      consumed += in_count;
      window_position += in_count;
      if (out_count > expected - produced) {
        return false;
      }
      copy_pixels(patch + produced, dictionary.get() + position, out_count);
      produced += out_count;
      position = (position + out_count) & (TINFL_LZ_DICT_SIZE - 1);
      if (status == TINFL_STATUS_DONE) {
        return produced == expected && consumed == input;
      }
      if ((status != TINFL_STATUS_HAS_MORE_OUTPUT && status != TINFL_STATUS_NEEDS_MORE_INPUT) ||
          (!in_count && !out_count) ||
          (status == TINFL_STATUS_NEEDS_MORE_INPUT && consumed == input)) {
        return false;
      }
    }
  }
  if (packet[12] != 1) {
    return false;
  }
  size_t cursor = 0, output = 0;
  while (cursor < input) {
    if (input - cursor < 2) {
      return false;
    }
    const uint16_t control = uint16_t(body[cursor]) | uint16_t(body[cursor + 1]) << 8;
    cursor += 2;
    const size_t count = ((control & 0x7fff) + 1) * 2;
    if (count > expected - output) {
      return false;
    }
    if (control & 0x8000) {
      if (input - cursor < 2) {
        return false;
      }
      uint8_t repeated[960];
      for (size_t i = 0; i < sizeof(repeated); i += 2) {
        repeated[i] = body[cursor];
        repeated[i + 1] = body[cursor + 1];
      }
      for (size_t i = 0; i < count; i += sizeof(repeated)) {
        const size_t size = count - i < sizeof(repeated) ? count - i : sizeof(repeated);
        copy_pixels(patch + output + i, repeated, size);
      }
      cursor += 2;
    } else {
      if (input - cursor < count) {
        return false;
      }
      copy_pixels(patch + output, body + cursor, count);
      cursor += count;
    }
    output += count;
  }
  return output == expected;
}
} // namespace esphome::presto_display
