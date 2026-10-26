#pragma once
// Streaming patch decoder. Pure C++ with no ESPHome or Pico types, so the host tests
// compile and drive the exact code that ships.
//
// Wire format: `>IHHHHB` (base frame, x, y, width, height, codec) + body, where codec
// 0 = zlib and 1 = CastKit RLE. Each base64-decoded chunk (at most 3,000 B, in SRAM) is
// fed to the decoder as it arrives, and the output rows are written straight to their
// destination: the back framebuffer, or a frame slot. There is no PSRAM staging buffer.
#include <cstddef>
#include <cstdint>
#include "miniz_tinfl.h"

namespace esphome::presto_display {

constexpr int PANEL = 480;
constexpr size_t FRAME_BYTES = size_t(PANEL) * PANEL * 2;

struct Rect {
  int x{0}, y{0}, width{0}, height{0};
  bool empty() const { return width <= 0 || height <= 0; }
  size_t bytes() const { return empty() ? 0 : size_t(width) * size_t(height) * 2; }
};

constexpr Rect FULL_FRAME{0, 0, PANEL, PANEL};

// Bounding box of two rectangles.
Rect unite(const Rect &a, const Rect &b);
// `a` minus `b` as at most four disjoint rectangles; returns the count written.
int subtract(const Rect &a, const Rect &b, Rect out[4]);

constexpr size_t HEADER_BYTES = 13;

struct PatchHeader {
  uint32_t base{0};
  Rect rect;
  uint8_t codec{0};
};

// Parse the 13-byte header and check it against the frame its destination holds now: a
// full 480x480 frame with base 0, or any rectangle with base == `current_frame`.
bool parse_header(const uint8_t *bytes, uint32_t current_frame, PatchHeader &header);

// Stores decoded bytes. On the board `write` is a paced SRAM -> PSRAM copy.
class PixelWriter {
 public:
  virtual void write(uint8_t *to, const uint8_t *from, size_t size) = 0;

 protected:
  ~PixelWriter() = default;
};

class StreamDecoder {
 public:
  enum class Status {
    FAILED, // malformed input; the destination rectangle holds partial output
    NEED_INPUT, // every byte was consumed; feed the next chunk
    MORE, // the output budget ran out; feed the unconsumed rest again
    COMPLETE, // the whole rectangle was produced and the stream ended cleanly
  };
  StreamDecoder(tinfl_decompressor *inflater, uint8_t *dictionary, PixelWriter *writer)
      : inflater_(inflater), dictionary_(dictionary), writer_(writer) {}
  // Start a patch. `target` is where the rectangle's top-left pixel goes and `stride` is
  // the target's row length in pixels: PANEL for a framebuffer or a slot, or the width for
  // a compact buffer.
  void begin(const PatchHeader &header, uint8_t *target, int stride = PANEL);
  // Feed body bytes. `consumed` reports how many were used. Output stops soon after
  // `budget` bytes were written (it is checked between writes of at most 4,096 B), so a
  // caller can split a large chunk across several calls; pass SIZE_MAX for no limit.
  // `is_last` marks the final bytes of the body and must stay set when a final chunk is
  // resumed after MORE.
  Status feed(const uint8_t *data, size_t size, bool is_last, size_t &consumed, size_t budget);
  size_t produced() const { return produced_; }
  const PatchHeader &header() const { return header_; }

 protected:
  void emit_(const uint8_t *data, size_t size);
  bool flush_(size_t &budget);
  Status feed_zlib_(const uint8_t *data, size_t size, bool is_last, size_t &consumed,
                    size_t &budget);
  Status feed_rle_(const uint8_t *data, size_t size, bool is_last, size_t &consumed,
                   size_t &budget);
  tinfl_decompressor *inflater_;
  uint8_t *dictionary_;
  PixelWriter *writer_;
  uint8_t *target_{nullptr};
  int stride_{PANEL};
  PatchHeader header_;
  size_t expected_{0}, produced_{0};
  bool is_failed_{false}, is_complete_{false};
  // zlib: inflated bytes still waiting in the dictionary, and the last inflate status.
  size_t position_{0}, pending_at_{0}, pending_{0};
  int inflate_status_{0};
  // RLE: a control word may straddle chunks, and so may a run's value.
  uint8_t control_bytes_[2]{}, value_bytes_[2]{};
  int control_have_{0}, value_have_{0};
  size_t literal_left_{0}, repeat_left_{0}, repeat_count_{0};
  bool is_repeat_pending_{false};
  alignas(4) uint8_t repeated_[960];
};

} // namespace esphome::presto_display
