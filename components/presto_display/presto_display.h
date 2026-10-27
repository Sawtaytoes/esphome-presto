#pragma once
#include "esphome/components/display/display_buffer.h"
#include "st7701.hpp"
#include <Arduino.h>

namespace esphome::presto_display {
class PrestoDisplay : public display::DisplayBuffer {
 public:
  void setup() override;
  void on_shutdown() override;
  void pause_scanout();
  void wait_for_prefetch();
  void fill(Color color) override;
  void update() override;
  void dump_config() override;
  int get_width_internal() override { return 480; }
  int get_height_internal() override { return 480; }
  display::DisplayType get_display_type() override {
    return display::DisplayType::DISPLAY_TYPE_COLOR;
  }
  void set_brightness(float brightness);
  // The framebuffer drawing goes into; never the visible one.
  uint16_t *get_framebuffer() { return framebuffers_[drawing_buffer_]; }
  // One of the two framebuffers, by index.
  uint16_t *get_framebuffer(int index) { return framebuffers_[index & 1]; }
  // The buffer the scanout reads now: a framebuffer, or a buffer passed to show_buffer().
  uint16_t *get_front_framebuffer() { return front_; }
  void present(bool copy_previous = true);
  // Scan out any full 480x480 RGB565 buffer in PSRAM from the next vertical blank, and
  // return once it is visible. Showing one of the framebuffers makes drawing use the other.
  void show_buffer(uint16_t *buffer);
  uint32_t get_prefetch_overruns() const { return driver_->get_prefetch_overruns(); }
  // Scroll window (presto_scroll.h). The picture is set once at boot; a window word is
  // latched at the next frame start, and the latch count tells when an old one is gone.
  void set_scroll_picture(uint16_t *picture) { driver_->set_window_picture(picture); }
  void set_scroll_window(uint32_t word) { driver_->request_window(word); }
  uint32_t get_scroll_window_latches() const { return driver_->get_window_latches(); }

 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  uint16_t *framebuffers_[2] = {nullptr, nullptr};
  uint16_t *front_{nullptr};
  pimoroni::ST7701 *driver_{nullptr};
  uint8_t drawing_buffer_{0};
  float brightness_{0.02f};
};
} // namespace esphome::presto_display

#ifdef USE_PRESTO_BACKLIGHT
#include "presto_backlight.h"
#endif

#ifdef USE_PRESTO_REMOTE
#include "presto_remote.h"
#endif
#ifdef USE_PRESTO_TOUCH
#include "presto_touch.h"
#endif
