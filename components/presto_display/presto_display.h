#pragma once
#include "esphome/components/display/display_buffer.h"
#include "st7701.hpp"
#include <Arduino.h>

namespace esphome::presto_display {
class PrestoDisplay : public display::DisplayBuffer {
 public:
  void setup() override;
  void fill(Color color) override;
  void update() override;
  void dump_config() override;
  int get_width_internal() override { return 480; }
  int get_height_internal() override { return 480; }
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_COLOR; }
  void set_brightness(float brightness);
  uint16_t *get_framebuffer() { return framebuffers_[drawing_buffer_]; }
  void present();
  // Sequential RGB565-BE chunks are staged until a complete frame is received.
  bool receive_frame_chunk(int32_t frame_id, int32_t offset, const std::string &pixels, bool is_last);
  int32_t get_presented_frame_id() const { return presented_frame_id_; }
 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  uint16_t *framebuffers_[2] = {nullptr, nullptr};
  pimoroni::ST7701 *driver_{nullptr};
  uint8_t drawing_buffer_{0};
  float brightness_{0.02f};
  int32_t receiving_frame_id_{-1};
  int32_t presented_frame_id_{-1};
  size_t received_bytes_{0};
};
}

#ifdef USE_PRESTO_BACKLIGHT
#include "presto_backlight.h"
#endif
