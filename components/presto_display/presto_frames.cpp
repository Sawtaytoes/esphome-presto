#include "esphome/core/defines.h"
#ifdef USE_PRESTO_PARTIAL_FRAMES
#include "presto_frames.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "pico/time.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

// The receiver's PSRAM write pacing (presto_remote.cpp): wait for the scanout's row
// prefetch, write at most 960 bytes, then leave the QMI idle for 50 us. Override both with
// build flags only together with on-panel measurements.
#ifndef PRESTO_WRITE_BATCH
#define PRESTO_WRITE_BATCH 960
#endif
#ifndef PRESTO_PACE_US
#define PRESTO_PACE_US 50
#endif

namespace esphome::presto_display {
static const char *const TAG = "presto_frames";
static constexpr int SIZE = 480;
// Frames further apart than this are separate bursts, not one animation.
static constexpr uint32_t BURST_GAP_US = 500000;

void PrestoFrameDisplay::dump_config() {
  PrestoDisplay::dump_config();
  ESP_LOGCONFIG(TAG,
                "  Partial frames: paced writes of %u bytes, %u us pause\n"
                "  Free heap: %u bytes",
                unsigned(PRESTO_WRITE_BATCH), unsigned(PRESTO_PACE_US),
                unsigned(rp2040.getFreeHeap()));
}

void PrestoFrameDisplay::begin_frame() {
  if (!is_frame_started_) {
    is_frame_started_ = true;
    frame_start_us_ = esphome::micros();
  }
}

// The swap requested by present_frame() happens at the next vertical blank. Until then the
// new back framebuffer is still on the glass, so every write waits here first.
void PrestoFrameDisplay::finish_swap_() {
  if (swap_pending_) {
    driver_->wait_for_vsync();
    swap_pending_ = false;
  }
}

void PrestoFrameDisplay::pause_() {
  if (batch_used_ == 0) {
    return;
  }
#if PRESTO_PACE_US > 0
  // Give the lower-priority XIP streaming hardware a QMI service window.
  const uint32_t start = time_us_32();
  sleep_us(PRESTO_PACE_US);
  frame_pace_us_ += time_us_32() - start;
#endif
  batch_used_ = 0;
}

// SRAM -> PSRAM behind the prefetch gate, at most PRESTO_WRITE_BATCH bytes between pauses.
// Short rows share a batch, so a narrow strip pays one pause per 960 bytes, not per row.
void PrestoFrameDisplay::paced_write_(uint8_t *to, const uint8_t *from, size_t size) {
  while (size) {
    const size_t count = std::min(size, size_t(PRESTO_WRITE_BATCH) - batch_used_);
    const uint32_t gate_start = time_us_32();
    wait_for_prefetch();
    const uint32_t copy_start = time_us_32();
    const uint32_t gate_us = copy_start - gate_start;
    frame_gate_us_ += gate_us;
    max_gate_us_ = std::max(max_gate_us_, gate_us);
    memcpy(to, from, count);
    frame_copy_us_ += time_us_32() - copy_start;
    to += count;
    from += count;
    size -= count;
    batch_used_ += count;
    if (batch_used_ >= size_t(PRESTO_WRITE_BATCH)) {
      pause_();
    }
  }
}

void PrestoFrameDisplay::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr,
                                        display::ColorOrder order, display::ColorBitness bitness,
                                        bool big_endian, int x_offset, int y_offset, int x_pad) {
  if (!framebuffers_[0] || !driver_) {
    return;
  }
  if (bitness != display::COLOR_BITNESS_565 || order != display::COLOR_ORDER_RGB) {
    // The generic per-pixel path would write PSRAM unpaced. LVGL always sends RGB565.
    ESP_LOGW(TAG, "Only RGB565 blocks are drawn");
    return;
  }
  const size_t stride = size_t(x_offset) + w + x_pad;
  if (x_start < 0) {
    x_offset -= x_start;
    w += x_start;
    x_start = 0;
  }
  if (y_start < 0) {
    y_offset -= y_start;
    h += y_start;
    y_start = 0;
  }
  w = std::min(w, SIZE - x_start);
  h = std::min(h, SIZE - y_start);
  if (w <= 0 || h <= 0) {
    return;
  }
  begin_frame();
  const uint32_t wait_start = esphome::micros();
  finish_swap_();
  const uint32_t write_start = esphome::micros();
  uint16_t *back = framebuffers_[drawing_buffer_];
  for (int row = 0; row < h; ++row) {
    const uint8_t *from = ptr + ((size_t(y_offset) + row) * stride + x_offset) * 2;
    if (!big_endian) {
      // The framebuffer holds big-endian RGB565. LVGL swaps already with byte_order: big_endian.
      for (int x = 0; x < w; ++x) {
        row_[x] = uint16_t(from[2 * x] << 8 | from[2 * x + 1]);
      }
      from = reinterpret_cast<const uint8_t *>(row_);
    }
    paced_write_(reinterpret_cast<uint8_t *>(back + (y_start + row) * SIZE + x_start), from,
                 size_t(w) * 2);
  }
  pause_();
  const uint32_t end = esphome::micros();
  frame_vsync_us_ += write_start - wait_start;
  frame_write_us_ += end - write_start;
  frame_pixels_ += uint32_t(w) * h;
  regions_.add({x_start, y_start, w, h});
}

// Visible -> back for one rectangle: a gated read of each row into SRAM, then a paced write.
void PrestoFrameDisplay::copy_from_front_(const Rect &rect) {
  const uint16_t *front = front_;
  uint16_t *back = framebuffers_[drawing_buffer_];
  const size_t bytes = size_t(rect.width) * 2;
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    wait_for_prefetch();
    memcpy(row_, front + y * SIZE + rect.x, bytes);
    paced_write_(reinterpret_cast<uint8_t *>(back + y * SIZE + rect.x),
                 reinterpret_cast<const uint8_t *>(row_), bytes);
  }
}

void PrestoFrameDisplay::present_frame() {
  if (!regions_.has_current() || !driver_) {
    is_frame_started_ = false;
    return;
  }
  const uint32_t sync_start = esphome::micros();
  finish_swap_();
  for (const Rect &rect : regions_.stale()) {
    copy_from_front_(rect);
  }
  pause_();
  const uint32_t present_start = esphome::micros();
  uint16_t *back = framebuffers_[drawing_buffer_];
  // Prepares the first scanout rows (paced reads), then swaps at the next vertical blank.
  // The wait for that blank is left to the next write.
  driver_->set_framebuffer(back);
  front_ = back;
  drawing_buffer_ ^= 1;
  swap_pending_ = true;
  regions_.presented();
  const uint32_t now = esphome::micros();

  if (frame_pixels_ < min_report_pixels_) {
    ++small_frames_;
  } else {
    const uint32_t frame_us = now - frame_start_us_;
    ++frames_;
    frame_sum_us_ += frame_us;
    max_frame_us_ = std::max(max_frame_us_, frame_us);
    write_sum_us_ += frame_write_us_;
    vsync_sum_us_ += frame_vsync_us_;
    sync_sum_us_ += present_start - sync_start;
    present_sum_us_ += now - present_start;
    pixel_sum_ += frame_pixels_;
    gate_sum_us_ += frame_gate_us_;
    copy_sum_us_ += frame_copy_us_;
    pace_sum_us_ += frame_pace_us_;
    if (has_last_present_) {
      max_interval_us_ = std::max(max_interval_us_, now - last_present_us_);
    }
    if (has_last_present_ && now - last_present_us_ < BURST_GAP_US) {
      ++intervals_;
      interval_sum_us_ += now - last_present_us_;
    }
    last_present_us_ = now;
    has_last_present_ = true;
  }
  is_frame_started_ = false;
  frame_write_us_ = frame_vsync_us_ = frame_pixels_ = 0;
  frame_gate_us_ = frame_copy_us_ = frame_pace_us_ = 0;
}

bool PrestoFrameDisplay::take_report(FrameReport &report) {
  if (frames_ == 0) {
    return false;
  }
  const auto mean = [this](uint64_t sum) { return uint32_t(sum / frames_); };
  report.frames = frames_;
  report.small_frames = small_frames_;
  report.fps = intervals_ ? float(intervals_) * 1e6f / float(interval_sum_us_) : 0.0f;
  report.frame_us = mean(frame_sum_us_);
  report.max_frame_us = max_frame_us_;
  report.write_us = mean(write_sum_us_);
  report.vsync_us = mean(vsync_sum_us_);
  report.sync_us = mean(sync_sum_us_);
  report.present_us = mean(present_sum_us_);
  const uint32_t accounted = report.write_us + report.vsync_us + report.sync_us + report.present_us;
  report.render_us = report.frame_us > accounted ? report.frame_us - accounted : 0;
  report.pixels = mean(pixel_sum_);
  report.overruns = get_prefetch_overruns();
  report.window_overruns = report.overruns - window_overruns_start_;
  report.gate_us = mean(gate_sum_us_);
  report.copy_us = mean(copy_sum_us_);
  report.pace_us = mean(pace_sum_us_);
  report.max_gate_us = max_gate_us_;
  report.max_interval_us = max_interval_us_;
  window_overruns_start_ = report.overruns;
  frames_ = small_frames_ = intervals_ = max_frame_us_ = 0;
  interval_sum_us_ = frame_sum_us_ = write_sum_us_ = vsync_sum_us_ = sync_sum_us_ =
      present_sum_us_ = pixel_sum_ = 0;
  gate_sum_us_ = copy_sum_us_ = pace_sum_us_ = 0;
  max_gate_us_ = max_interval_us_ = 0;
  return true;
}

void PrestoFrameDisplay::format_report(const FrameReport &report, char *detail, size_t detail_size,
                                       char *summary, size_t summary_size) {
  const auto ms = [](uint32_t us) { return us / 1000.0f; };
  snprintf(detail, detail_size,
           "fps=%.1f frame=%.1fms max=%.1fms render=%.1f write=%.1f vsync=%.1f sync=%.1f "
           "present=%.1f px=%u frames=%u small=%u overruns=%u(+%u) "
           "gate=%.1f copy=%.1f pace=%.1f gateMax=%.1f intervalMax=%.1f",
           report.fps, ms(report.frame_us), ms(report.max_frame_us), ms(report.render_us),
           ms(report.write_us), ms(report.vsync_us), ms(report.sync_us), ms(report.present_us),
           unsigned(report.pixels), unsigned(report.frames), unsigned(report.small_frames),
           unsigned(report.overruns), unsigned(report.window_overruns), ms(report.gate_us),
           ms(report.copy_us), ms(report.pace_us), ms(report.max_gate_us), ms(report.max_interval_us));
  if (report.fps > 0) {
    snprintf(summary, summary_size, "%.1f fps  %u ms  ovr %u", report.fps,
             unsigned((report.frame_us + 500) / 1000), unsigned(report.overruns));
  } else {
    snprintf(summary, summary_size, "-- fps  %u ms  ovr %u",
             unsigned((report.frame_us + 500) / 1000), unsigned(report.overruns));
  }
}

} // namespace esphome::presto_display
#endif
