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
  uint16_t *get_framebuffer() { return framebuffers_[drawing_buffer_]; }
  void present(bool copy_previous = true);
  uint32_t get_prefetch_overruns() const { return driver_->get_prefetch_overruns(); }

 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  uint16_t *framebuffers_[2] = {nullptr, nullptr};
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
