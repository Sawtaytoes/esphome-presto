#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_PARTIAL_FRAMES
#include "presto_display.h"
#include "presto_regions.h"
#ifdef USE_PRESTO_NATIVE_SCENE
#include "presto_scene.h"
#endif
#include <cstddef>
#include <cstdint>

namespace esphome::presto_display {

// Timing of the frames presented since the last report. All times are microseconds.
struct FrameReport {
  uint32_t frames{0}; // frames presented in the report window
  uint32_t small_frames{0}; // frames below the size floor, left out of every figure
  float fps{0}; // over back-to-back frames only; 0 when there were none
  uint32_t frame_us{0}; // mean, from LVGL's render start to the swap request
  uint32_t max_frame_us{0};
  uint32_t render_us{0}; // elapsed remainder, including rendering and scheduling
  uint32_t write_us{0}; // mean paced PSRAM writes of the drawn pixels
  uint32_t vsync_us{0}; // mean wait for the previous frame's vertical blank
  uint32_t sync_us{0}; // mean copy of stale areas from the visible framebuffer
  uint32_t present_us{0}; // mean scanout preparation for the swap
  uint32_t pixels{0}; // mean pixels drawn per frame
  uint32_t overruns{0}; // scanout prefetch overruns since boot
  uint32_t window_overruns{0}; // prefetch overruns during the report window
  uint32_t gate_us{0}, copy_us{0}, pace_us{0}; // all paced writes, including stale sync
  uint32_t max_gate_us{0};
  uint32_t max_interval_us{0}; // all presentation intervals, including idle gaps
};

// The display for LVGL's partial renderer (`partial_frames: true`). LVGL draws each strip
// in SRAM and flushes it through draw_pixels_at, which copies it into the back framebuffer
// through the same prefetch gate and pacing as the CastKit receiver. `present_frame()` at
// the end of an LVGL refresh brings the rest of the back framebuffer up to date and swaps
// at vertical blank. Never combine with the CastKit receiver: both draw the back buffer.
class PrestoFrameDisplay : public PrestoDisplay {
 public:
  using PrestoDisplay::draw_pixels_at;
  // LVGL drives every frame. The inherited update() would copy the whole frame unpaced.
  void update() override {}
  void dump_config() override;
  void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr,
                      display::ColorOrder order, display::ColorBitness bitness, bool big_endian,
                      int x_offset, int y_offset, int x_pad) override;
  // LVGL's `on_draw_start`: the frame's timing starts here.
  void begin_frame();
  // LVGL's `on_draw_end`. Presents the frame; does nothing when the refresh drew nothing.
  void present_frame();
  // Frames that draw fewer pixels (a label changing) are counted but left out of the
  // figures, so a report shown on screen never reports its own redraw. Default 480x48.
  void set_min_report_pixels(uint32_t pixels) { min_report_pixels_ = pixels; }
  // False when no frame of at least the minimum size was presented since the last report.
  bool take_report(FrameReport &report);
#ifdef USE_PRESTO_NATIVE_SCENE
  NativeScene &scene() {
    return scene_;
  }
#endif
  // Format a report: `detail` for a text sensor, `summary` for an on-screen label.
  static void format_report(const FrameReport &report, char *detail, size_t detail_size,
                            char *summary, size_t summary_size);

 protected:
  void finish_swap_();
  void paced_write_(uint8_t *to, const uint8_t *from, size_t size);
  void pause_();
  void copy_from_front_(const Rect &rect);

  FrameRegions regions_;
#ifdef USE_PRESTO_NATIVE_SCENE
  NativeScene scene_;
#endif
  bool swap_pending_{false};
  size_t batch_used_{0};
  alignas(4) uint16_t row_[480];

  // The frame being drawn.
  uint32_t frame_start_us_{0};
  bool is_frame_started_{false};
  uint32_t frame_write_us_{0}, frame_vsync_us_{0}, frame_pixels_{0};
  uint32_t frame_gate_us_{0}, frame_copy_us_{0}, frame_pace_us_{0};
  uint32_t max_gate_us_{0}, max_interval_us_{0};
  uint64_t gate_sum_us_{0}, copy_sum_us_{0}, pace_sum_us_{0};
  // The report window.
  uint32_t last_present_us_{0};
  bool has_last_present_{false};
  uint32_t window_overruns_start_{0};
  uint32_t frames_{0}, small_frames_{0}, intervals_{0};
  uint64_t interval_sum_us_{0}, frame_sum_us_{0}, write_sum_us_{0}, vsync_sum_us_{0},
      sync_sum_us_{0}, present_sum_us_{0}, pixel_sum_{0};
  uint32_t max_frame_us_{0};
  uint32_t min_report_pixels_{480 * 48};
};

} // namespace esphome::presto_display
#endif
