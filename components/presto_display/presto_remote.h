#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_REMOTE
#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "miniz_tinfl.h"
#include <cstdint>
#include <functional>
namespace esphome::presto_display {
class PrestoDisplay;
class PrestoRemote : public Component {
 public:
  void setup() override;
  void loop() override;
  void set_display(PrestoDisplay *display) { display_ = display; }
  void set_events(text_sensor::TextSensor *events) { events_ = events; }
  void frame_chunk(int frame_id, int touch_id, int format, int offset, bool final_chunk,
                   const StringRef &encoded);
  void touch(int phase, int x, int y);
  void controls_ack(int revision, int backlight);
  void set_touch_poller(std::function<void()> poller) { touch_poller_ = std::move(poller); }
  void poll_touch();
  void wait_for_prefetch();

 protected:
  void error_(const char *reason);
  bool decode_packet_();
  PrestoDisplay *display_{nullptr};
  text_sensor::TextSensor *events_{nullptr};
  uint8_t *packet_{nullptr}, *patch_{nullptr};
  uint8_t chunk_buffer_[3000];
  tinfl_decompressor *inflater_{nullptr};
  size_t received_{0}, encoded_offset_{0};
  int receiving_frame_{0}, receiving_touch_{0}, presented_frame_{0}, sequence_{0};
  uint32_t presented_at_{0}, last_touch_poll_{0};
  std::function<void()> touch_poller_;
  bool offline_{false};
  bool touch_enabled_{false};
  int last_x_{0}, last_y_{0};
};
} // namespace esphome::presto_display

#endif
