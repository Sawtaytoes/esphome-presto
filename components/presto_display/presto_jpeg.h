#pragma once
// Codec 2: a baseline JPEG patch. Pure C++ with no ESPHome or Pico types, so the host tests
// compile and drive the exact code that ships. The vendored JPEGDEC header stays out of this
// one; only presto_jpeg.cpp includes it.
//
// The compressed body is collected whole (PSRAM on the board, written through the paced
// writer and read back through the prefetch-gated reader), checked marker by marker, and
// decoded on the final chunk. JPEGDEC hands back blocks of big-endian RGB565 pixels that are
// clipped to the rectangle and written row by row through the same paced writer.
#include <cstddef>
#include <cstdint>
#include "presto_stream.h"

class JPEGDEC;

namespace esphome::presto_display {

// The largest compressed body accepted. A q75 480x360 camera frame is about 30 KB.
constexpr size_t JPEG_BODY_LIMIT = 128 * 1024;

// Reads PSRAM into SRAM; on the board it waits for the scanout's row prefetch.
class PixelReader {
 public:
  virtual void read(uint8_t *to, const uint8_t *from, size_t size) = 0;

 protected:
  ~PixelReader() = default;
};

enum class JpegResult {
  OK,
  TOO_LARGE, // the body is over the buffer's capacity
  SIZE_MISMATCH, // the image is not exactly the header's rectangle
  UNSUPPORTED, // not one baseline scan of 8-bit YCbCr 4:4:4, 4:2:2, or 4:2:0
  INVALID, // malformed, truncated, or rejected by the decoder
};

// The `error,<reason>` for a failed JPEG patch.
const char *jpeg_reason(JpegResult result);

struct JpegInfo {
  int width{0}, height{0};
  // Luma sampling factors, horizontal in the high nibble: 0x11, 0x21, or 0x22.
  uint8_t sampling{0};
};

class JpegDecoder {
 public:
  explicit JpegDecoder(PixelReader *reader) : reader_(reader) {}
  // Allocates JPEGDEC's state (about 18 KB, SRAM). False if that failed.
  bool allocate();
  // Where the compressed body is collected: PSRAM on the board.
  void set_buffer(uint8_t *buffer, size_t capacity) {
    buffer_ = buffer;
    capacity_ = capacity;
  }
  bool is_ready() const { return codec_ && buffer_ && capacity_; }

  void begin() { size_ = 0; }
  // Append body bytes through `writer`. False once the body would exceed the capacity.
  bool append(const uint8_t *data, size_t size, PixelWriter *writer);
  // Check the collected body without decoding it.
  JpegResult inspect(JpegInfo &info);
  // Check the collected body, then decode it into `target` (the rectangle's top-left pixel,
  // rows `stride` pixels apart) through `writer`. Nothing outside the rectangle is written.
  JpegResult decode(const Rect &rect, uint8_t *target, int stride, PixelWriter *writer);
  size_t size() const { return size_; }

  // JPEGDEC callbacks; not for other callers.
  int32_t read_at_(int32_t position, uint8_t *to, int32_t length);
  bool draw_(int x, int y, int pitch, int width, int height, const uint16_t *pixels);

 protected:
  bool read_(size_t at, uint8_t *to, size_t length);

  PixelReader *reader_;
  JPEGDEC *codec_{nullptr};
  uint8_t *buffer_{nullptr};
  size_t capacity_{0}, size_{0};
  // The decode in progress.
  Rect rect_;
  uint8_t *target_{nullptr};
  int stride_{PANEL};
  PixelWriter *writer_{nullptr};
  size_t drawn_{0};
  bool is_outside_{false};
};

} // namespace esphome::presto_display
