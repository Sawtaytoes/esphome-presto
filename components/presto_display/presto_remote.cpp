#include "esphome/core/defines.h"
#ifdef USE_PRESTO_REMOTE
#include "presto_display.h"
#include "presto_remote.h"
#include "presto_pixels.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "pico/time.h"
namespace esphome::presto_display {
static constexpr size_t FRAME_BYTES = 480 * 480 * 2;
static constexpr size_t PACKET_BYTES = 500000;
static PrestoRemote *active_receiver = nullptr;
static void copy_pixels_paced(uint8_t *to, const uint8_t *from, size_t size) {
  alignas(4) uint8_t row[960];
  while (size) {
    const size_t count = std::min(size, sizeof(row));
    if (active_receiver) {
      active_receiver->wait_for_prefetch();
    }
    memcpy(row, from, count);
    if (active_receiver) {
      active_receiver->wait_for_prefetch();
    }
    memcpy(to, row, count);
    from += count;
    to += count;
    size -= count;
    // Give the lower-priority XIP streaming hardware a QMI service window.
    sleep_us(50);
    if (active_receiver) {
      active_receiver->poll_touch();
    }
  }
}
static uint16_t read16(const uint8_t *p) {
  return uint16_t(p[0]) << 8 | p[1];
}
void PrestoRemote::setup() {
  packet_ = static_cast<uint8_t *>(pmalloc(PACKET_BYTES));
  patch_ = static_cast<uint8_t *>(pmalloc(FRAME_BYTES));
  // miniz's state is too large for the RP2 loop stack; allocate it once.
  inflater_ = new tinfl_decompressor;
  if (!packet_ || !patch_ || !inflater_) {
    mark_failed();
  }
}
void PrestoRemote::error_(const char *reason) {
  receiving_frame_ = 0;
  received_ = encoded_offset_ = 0;
  events_->publish_state(std::string("error,") + reason);
}
void PrestoRemote::frame_chunk(int frame_id, int touch_id, int format, int offset, bool final_chunk,
                               const StringRef &encoded) {
  if (is_failed() || frame_id <= 0 || touch_id < 0 || format != 4 || offset < 0 ||
      encoded.size() == 0 || encoded.size() > 4000 || encoded.size() % 4) {
    error_("invalid_chunk");
    return;
  }
  if (!offset) {
    receiving_frame_ = frame_id;
    receiving_touch_ = touch_id;
    received_ = encoded_offset_ = 0;
  }
  if (frame_id != receiving_frame_ || touch_id != receiving_touch_ ||
      size_t(offset) != encoded_offset_ || received_ + encoded.size() / 4 * 3 > PACKET_BYTES) {
    error_("chunk_order_or_size");
    return;
  }
  for (size_t i = 0; i < encoded.size(); ++i) {
    const char c = encoded[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
          c == '/' || (c == '=' && final_chunk && i >= encoded.size() - 2))) {
      error_("invalid_base64");
      return;
    }
  }
  const size_t count = base64_decode(reinterpret_cast<const uint8_t *>(encoded.c_str()),
                                     encoded.size(), chunk_buffer_, sizeof(chunk_buffer_));
  if (!count) {
    error_("invalid_base64");
    return;
  }
  active_receiver = this;
  copy_pixels_paced(packet_ + received_, chunk_buffer_, count);
  received_ += count;
  encoded_offset_ += encoded.size();
  if (!final_chunk) {
    return;
  }
  active_receiver = this;
  const uint32_t started = micros();
  if (!decode_packet_()) {
    error_("invalid_packet_or_base");
    return;
  }
  const uint32_t decoded_at = micros();
  const uint16_t x = read16(packet_ + 4), y = read16(packet_ + 6);
  const uint16_t width = read16(packet_ + 8), height = read16(packet_ + 10);
  auto *back = reinterpret_cast<uint8_t *>(display_->get_framebuffer());
  bool changed = false;
  for (size_t row = 0; row < height; ++row) {
    display_->wait_for_prefetch();
    if (memcmp(back + ((y + row) * 480 + x) * 2, patch_ + row * width * 2, width * 2)) {
      changed = true;
      break;
    }
  }
  if (changed) {
    for (size_t row = 0; row < height; ++row) {
      copy_pixels_paced(back + ((y + row) * 480 + x) * 2, patch_ + row * width * 2, width * 2);
    }
    display_->present(false);
    back = reinterpret_cast<uint8_t *>(display_->get_framebuffer());
    for (size_t row = 0; row < height; ++row) {
      copy_pixels_paced(back + ((y + row) * 480 + x) * 2, patch_ + row * width * 2, width * 2);
    }
  }
  const uint32_t draw_us = micros() - decoded_at;
  presented_frame_ = frame_id;
  presented_at_ = millis();
  offline_ = false;
  receiving_frame_ = 0;
  received_ = encoded_offset_ = 0;
  char event[120];
  snprintf(event, sizeof(event), "frame,%d,%d,0,%lu,%lu,%lu", frame_id, touch_id,
           static_cast<unsigned long>(display_->get_prefetch_overruns()),
           static_cast<unsigned long>(decoded_at - started), static_cast<unsigned long>(draw_us));
  events_->publish_state(event);
}
bool PrestoRemote::decode_packet_() {
  return decode_pixels(packet_, received_, presented_frame_, patch_, inflater_, copy_pixels_paced);
}
void PrestoRemote::touch(int phase, int x, int y) {
  if (!touch_enabled_ || phase < 0 || phase > 2 || x < 0 || x >= 480 || y < 0 || y >= 480 ||
      !presented_frame_ || millis() - presented_at_ > 7000) {
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
  snprintf(event, sizeof(event), "touch,%d,%d,%d,%d,0,%d,0", ++sequence_, phase, x, y,
           presented_frame_);
  events_->publish_state(event);
}
void __not_in_flash_func(PrestoRemote::wait_for_prefetch)() {
  display_->wait_for_prefetch();
}
void PrestoRemote::poll_touch() {
  if (touch_poller_ && millis() - last_touch_poll_ >= 20) {
    last_touch_poll_ = millis();
    touch_poller_();
  }
}
void PrestoRemote::controls_ack(int revision, int backlight) {
  touch_enabled_ = backlight > 0;
  char event[64];
  snprintf(event, sizeof(event), "controls,%d,%d", revision, backlight);
  events_->publish_state(event);
}
void PrestoRemote::loop() {
  if (!presented_frame_ || offline_ || millis() - presented_at_ <= 7000) {
    return;
  }
  display_->fill(Color(0, 0, 0));
  display_->filled_rectangle(0, 0, 480, 4, Color(255, 0, 0));
  display_->present();
  presented_frame_ = 0;
  offline_ = true;
}
} // namespace esphome::presto_display
#endif
