#include "esphome/core/defines.h"
#ifdef USE_PRESTO_REMOTE
#include "presto_display.h"
#include "presto_remote.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "pico/time.h"

// PSRAM write pacing. Production values: wait for the scanout's row prefetch, write at most
// 960 bytes, then leave the QMI idle for 50 us. They are what made native-resolution
// music artwork steady; change them only with on-panel measurements.
#ifndef PRESTO_WRITE_BATCH
#define PRESTO_WRITE_BATCH 960
#endif
#ifndef PRESTO_PACE_US
#define PRESTO_PACE_US 50
#endif

namespace esphome::presto_display {
static const char *const TAG = "presto_remote";

void PrestoRemote::setup() {
  if (!receiver_.begin()) {
    ESP_LOGE(TAG, "Could not allocate the inflate state");
    mark_failed();
    return;
  }
  // The live JPEG body buffer, before the slots so they cannot take its PSRAM. Without it (or
  // without the decoder state) the board refuses codec 2 and does not advertise it.
  auto *jpeg = static_cast<uint8_t *>(pmalloc(JPEG_BODY_LIMIT));
  if (jpeg) {
    receiver_.set_jpeg_buffer(jpeg, JPEG_BODY_LIMIT);
  }
  if (!receiver_.has_jpeg()) {
    ESP_LOGW(TAG, "No memory for JPEG patches; live frames use zlib and RLE only");
  }
  if (codecs_) {
    codecs_->publish_state(receiver_.codecs());
  }
  // Frame slots live in PSRAM for the life of the firmware; drop_slots only empties them.
  // A slot that could not be allocated answers every upload with SLOT_UNAVAILABLE.
  uint8_t *slots[SLOT_COUNT];
  int count = 0;
  while (count < SLOT_COUNT && (slots[count] = static_cast<uint8_t *>(pmalloc(FRAME_BYTES)))) {
    ++count;
  }
  if (count < SLOT_COUNT) {
    ESP_LOGW(TAG, "PSRAM for %d of %d frame slots", count, SLOT_COUNT);
  }
  receiver_.set_slots(slots, count);
  ESP_LOGI(TAG, "%d frame slots, %u bytes of PSRAM free", count,
           unsigned(rp2040.getFreePSRAMHeap()));
}

bool PrestoRemote::is_ready_() {
  return !is_failed() && !display_->is_failed();
}

void PrestoRemote::frame_chunk(int frame_id, int touch_id, int format, int offset, bool final_chunk,
                               const StringRef &encoded) {
  if (!is_ready_()) {
    publish("error,invalid_chunk");
    return;
  }
  receiver_.frame_chunk(frame_id, touch_id, format, offset, final_chunk, encoded.c_str(),
                        encoded.size());
}

void PrestoRemote::slot_chunk(int slot, int frame_id, int format, int offset, bool final_chunk,
                              const StringRef &encoded) {
  if (!is_ready_()) {
    if (final_chunk) {
      char event[64];
      snprintf(event, sizeof(event), "slot,%d,%d,%d", slot, frame_id, int(SLOT_UNAVAILABLE));
      publish_slot(event);
    }
    return;
  }
  receiver_.slot_chunk(slot, frame_id, format, offset, final_chunk, encoded.c_str(),
                       encoded.size());
}

void PrestoRemote::show_slot(int slot, int touch_id) {
  if (!is_ready_()) {
    publish("error,slot_unavailable");
    return;
  }
  receiver_.show_slot(slot, touch_id);
}

void PrestoRemote::drop_slots() {
  if (is_ready_()) {
    receiver_.drop_slots();
  }
}

void PrestoRemote::touch(int phase, int x, int y) {
  if (phase < 0 || phase > 2 || x < 0 || x >= 480 || y < 0 || y >= 480) {
    return;
  }
  // The latch sees every contact, including releases while offline, so a swallowed press always
  // reaches its release.
  const TouchAction action = wake_.touch(phase, esphome::millis());
  if (action == TouchAction::WAKE) {
    apply_brightness_();
    publish("wake,1");
    return;
  }
  const int shown = receiver_.touch_frame();
  if (action == TouchAction::DROP || !shown) {
    return;
  }
  if (phase == 2) {
    x = last_x_;
    y = last_y_;
  } else {
    last_x_ = x;
    last_y_ = y;
  }
  char event[100];
  snprintf(event, sizeof(event), "touch,%d,%d,%d,%d,0,%d,0", ++sequence_, phase, x, y, shown);
  publish(event);
}

void __not_in_flash_func(PrestoRemote::wait_for_prefetch)() {
  display_->wait_for_prefetch();
}

void PrestoRemote::poll_touch() {
  if (touch_poller_ && esphome::millis() - last_touch_poll_ >= 20) {
    last_touch_poll_ = esphome::millis();
    touch_poller_();
  }
}

void PrestoRemote::apply_brightness_() {
  display_->set_brightness(wake_.brightness() / 100.0f);
}

void PrestoRemote::set_backlight(int percent) {
  wake_.set_backlight(percent);
  apply_brightness_();
}

void PrestoRemote::set_wake(int percent, int seconds) {
  wake_.set_wake(percent, seconds);
  apply_brightness_();
  char event[48];
  snprintf(event, sizeof(event), "wake_controls,%d,%d", percent, seconds);
  publish(event);
}

void PrestoRemote::controls_ack(int revision, int backlight) {
  char event[64];
  snprintf(event, sizeof(event), "controls,%d,%d", revision, backlight);
  publish(event);
}

void PrestoRemote::loop() {
  if (wake_.expire(esphome::millis())) {
    apply_brightness_();
    publish("wake,0");
  }
  if (!is_ready_()) {
    return;
  }
  // Idle work (the offline blank, slot decoding, framebuffer sync) in bounded pieces; keep
  // the loop running continuously while some is queued.
  if (receiver_.step()) {
    high_frequency_.start();
  } else {
    high_frequency_.stop();
  }
}

// ---------------------------------------------------------------------------------------
// Panel

// SRAM -> PSRAM, behind the scanout prefetch gate, paced.
void PrestoRemote::write(uint8_t *to, const uint8_t *from, size_t size) {
  while (size) {
    const size_t count = std::min(size, size_t(PRESTO_WRITE_BATCH));
    display_->wait_for_prefetch();
    memcpy(to, from, count);
    to += count;
    from += count;
    size -= count;
#if PRESTO_PACE_US > 0
    // Give the lower-priority XIP streaming hardware a QMI service window.
    sleep_us(PRESTO_PACE_US);
#endif
    poll_touch();
  }
}

// PSRAM -> SRAM, behind the scanout prefetch gate.
void PrestoRemote::read(uint8_t *to, const uint8_t *from, size_t size) {
  while (size) {
    const size_t count = std::min(size, size_t(PRESTO_WRITE_BATCH));
    display_->wait_for_prefetch();
    memcpy(to, from, count);
    to += count;
    from += count;
    size -= count;
  }
}

uint8_t *PrestoRemote::framebuffer(int index) {
  return reinterpret_cast<uint8_t *>(display_->get_framebuffer(index));
}

uint8_t *PrestoRemote::visible() {
  return reinterpret_cast<uint8_t *>(display_->get_front_framebuffer());
}

void PrestoRemote::show(uint8_t *buffer) {
  display_->show_buffer(reinterpret_cast<uint16_t *>(buffer));
}

void PrestoRemote::blank() {
  display_->fill(Color(0, 0, 0));
  display_->filled_rectangle(0, 0, 480, 4, Color(255, 0, 0));
  display_->present();
}

void PrestoRemote::publish(const char *event) {
  events_->publish_state(event);
}

void PrestoRemote::publish_slot(const char *event) {
  if (slot_events_) {
    slot_events_->publish_state(event);
  }
}

void PrestoRemote::publish_timing(const char *report) {
  if (timing_) {
    timing_->publish_state(report);
  }
}

uint32_t PrestoRemote::micros() {
  return esphome::micros();
}

uint32_t PrestoRemote::millis() {
  return esphome::millis();
}

uint32_t PrestoRemote::overruns() {
  return display_->get_prefetch_overruns();
}

size_t PrestoRemote::free_psram() {
  return rp2040.getFreePSRAMHeap();
}
} // namespace esphome::presto_display
#endif
