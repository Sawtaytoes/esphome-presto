#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_REMOTE
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "presto_receiver.h"
#include "presto_wake.h"
#include <cstdint>
#include <functional>
namespace esphome::presto_display {
class PrestoDisplay;
// ESPHome side of the CastKit receiver: API actions, touch and wake, and the board's
// PSRAM access rules. The frame logic is `Receiver`.
class PrestoRemote : public Component, public Panel {
 public:
  PrestoRemote() : receiver_(this) {}
  void setup() override;
  void loop() override;
  void set_display(PrestoDisplay *display) { display_ = display; }
  void set_events(text_sensor::TextSensor *events) { events_ = events; }
  // `slot,...` reports. Without it slots still work but report nothing, and CastKit needs
  // the reports to use slots.
  void set_slot_events(text_sensor::TextSensor *slot_events) { slot_events_ = slot_events; }
  // Optional diagnostic sensor. A separate entity, so a timing report can never replace a
  // queued acknowledgement in ESPHome's one-state-per-entity send buffer.
  void set_timing(text_sensor::TextSensor *timing) { timing_ = timing; }
  // The live codecs this firmware decodes, `zlib,rle,jpeg`. CastKit sends JPEG only to a board
  // that lists `jpeg` here.
  void set_codecs(text_sensor::TextSensor *codecs) { codecs_ = codecs; }
  void frame_chunk(int frame_id, int touch_id, int format, int offset, bool final_chunk,
                   const StringRef &encoded);
  void slot_chunk(int slot, int frame_id, int format, int offset, bool final_chunk,
                  const StringRef &encoded);
  void show_slot(int slot, int touch_id);
  void drop_slots();
  void touch(int phase, int x, int y);
  void controls_ack(int revision, int backlight);
  // CastKit's commanded level. Zero is dark, but a press still lights the panel at the wake level.
  void set_backlight(int percent);
  void set_wake(int percent, int seconds);
  void set_touch_poller(std::function<void()> poller) { touch_poller_ = std::move(poller); }
  void poll_touch();
  void wait_for_prefetch();

  // Panel
  void write(uint8_t *to, const uint8_t *from, size_t size) override;
  void read(uint8_t *to, const uint8_t *from, size_t size) override;
  uint8_t *framebuffer(int index) override;
  uint8_t *visible() override;
  void show(uint8_t *buffer) override;
  void blank() override;
  void publish(const char *event) override;
  void publish_slot(const char *event) override;
  void publish_timing(const char *report) override;
  uint32_t micros() override;
  uint32_t millis() override;
  uint32_t overruns() override;
  size_t free_psram() override;

 protected:
  bool is_ready_();
  void apply_brightness_();
  PrestoDisplay *display_{nullptr};
  text_sensor::TextSensor *events_{nullptr}, *slot_events_{nullptr}, *timing_{nullptr},
      *codecs_{nullptr};
  Receiver receiver_;
  HighFrequencyLoopRequester high_frequency_;
  uint32_t last_touch_poll_{0};
  std::function<void()> touch_poller_;
  WakeLatch wake_;
  int sequence_{0};
  int last_x_{0}, last_y_{0};
};
} // namespace esphome::presto_display

#endif
