#include "presto_display.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <algorithm>
#include <cstring>
#include <atomic>

namespace {
std::atomic<pimoroni::ST7701 *> scanout_driver{nullptr};
std::atomic<bool> scanout_ready{false};
}

// Arduino owns the core lifecycle and its flash/OTA lockout handshake.
void setup1() {
  pimoroni::ST7701 *driver;
  while (!(driver = scanout_driver.load(std::memory_order_acquire))) __wfe();
  driver->init();
  scanout_ready.store(true, std::memory_order_release);
  __sev();
}
void loop1() { __wfe(); }

namespace esphome::presto_display {
static const char *const TAG = "presto_display";
void PrestoDisplay::setup() {
  framebuffers_[0] = static_cast<uint16_t *>(pmalloc(480 * 480 * 2));
  framebuffers_[1] = static_cast<uint16_t *>(pmalloc(480 * 480 * 2));
  if (!framebuffers_[0] || !framebuffers_[1]) {
    ESP_LOGE(TAG, "Could not allocate framebuffers in PSRAM");
    mark_failed();
    return;
  }
  memset(framebuffers_[0], 0, 480 * 480 * 2);
  memset(framebuffers_[1], 0, 480 * 480 * 2);
  driver_ = new pimoroni::ST7701(480, 480, pimoroni::ROTATE_0,
      {spi1, 28, 26, 27, pimoroni::PIN_UNUSED, pimoroni::PIN_UNUSED, 45}, framebuffers_[1], nullptr, 1);
  scanout_driver.store(driver_, std::memory_order_release);
  __sev();
  while (!scanout_ready.load(std::memory_order_acquire)) delay(1);
  drawing_buffer_ = 0;
  set_brightness(brightness_);
  ESP_LOGI(TAG, "480x480 RGB565 ready, PSRAM %u bytes", unsigned(rp2040.getPSRAMSize()));
}
void PrestoDisplay::update() {
  if (is_failed()) return;
  do_update_();
  present();
}
void PrestoDisplay::present() {
  driver_->set_framebuffer(framebuffers_[drawing_buffer_]);
  driver_->wait_for_vsync();
  drawing_buffer_ ^= 1;
  memcpy(framebuffers_[drawing_buffer_], framebuffers_[drawing_buffer_ ^ 1], 480 * 480 * 2);
}
void PrestoDisplay::fill(Color color) {
  if (!framebuffers_[drawing_buffer_]) return;
  const uint16_t pixel = ((uint16_t(color.red) & 0xf8) << 8) |
      ((uint16_t(color.green) & 0xfc) << 3) | (color.blue >> 3);
  std::fill_n(framebuffers_[drawing_buffer_], 480 * 480, uint16_t((pixel >> 8) | (pixel << 8)));
}
void PrestoDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= 480 || y < 0 || y >= 480 || !framebuffers_[drawing_buffer_]) return;
  const uint16_t pixel = ((uint16_t(color.red) & 0xf8) << 8) |
      ((uint16_t(color.green) & 0xfc) << 3) | (color.blue >> 3);
  framebuffers_[drawing_buffer_][y * 480 + x] = (pixel >> 8) | (pixel << 8);
}
void PrestoDisplay::set_brightness(float brightness) {
  brightness_ = std::clamp(brightness, 0.0f, 1.0f);
  if (driver_) driver_->set_backlight(uint8_t(brightness_ * 255));
}
void PrestoDisplay::dump_config() {
  LOG_DISPLAY("", "Pimoroni Presto", this);
}
}

namespace esphome::presto_display {
bool PrestoDisplay::receive_frame_chunk(int32_t frame_id, int32_t offset,
                                       const std::string &pixels, bool is_last) {
  constexpr size_t frame_bytes = 480 * 480 * 2;
  constexpr size_t max_chunk_bytes = 8192;
  if (is_failed() || !driver_ || frame_id < 0 || offset < 0 ||
      size_t(offset) >= frame_bytes || pixels.empty() || pixels.size() % 4 != 0 ||
      pixels.size() > ((max_chunk_bytes + 2) / 3) * 4) return false;
  if (offset == 0) {
    receiving_frame_id_ = frame_id;
    received_bytes_ = 0;
  }
  if (frame_id != receiving_frame_id_ || size_t(offset) != received_bytes_) return false;
  uint8_t decoded[max_chunk_bytes];
  const size_t count = base64_decode(pixels, decoded, sizeof(decoded));
  if (count == 0 || count % 2 || count > frame_bytes - received_bytes_ ||
      is_last != (received_bytes_ + count == frame_bytes)) {
    receiving_frame_id_ = -1;
    return false;
  }
  memcpy(reinterpret_cast<uint8_t *>(get_framebuffer()) + offset, decoded, count);
  received_bytes_ += count;
  if (is_last) {
    present();
    presented_frame_id_ = frame_id;
    receiving_frame_id_ = -1;
  }
  return true;
}
}
