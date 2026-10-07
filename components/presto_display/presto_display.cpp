#include "presto_display.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <algorithm>
#include <cstring>
#include <atomic>

// Keep decode/network stack growth away from the real-time display core.
bool core1_separate_stack = true;
bool core1_disable_systick = true;

namespace {
std::atomic<pimoroni::ST7701 *> scanout_driver{nullptr};
std::atomic<bool> scanout_ready{false};
std::atomic<bool> pause_requested{false};
std::atomic<bool> scanout_paused{false};
bool flash_paused = false;
} // namespace

// Arduino's doorbell handler parks core 1 while the shared QMI interface
// programs flash. Stop PSRAM DMA before that handler and restart it afterward.
void __not_in_flash_func(before_flash_lockout)() {
  if (multicore_doorbell_is_set_current_core(_MFIFO::_doorbell) &&
      !scanout_paused.load(std::memory_order_acquire)) {
    scanout_driver.load(std::memory_order_acquire)->pause_scanout();
    flash_paused = true;
  }
}
void after_flash_lockout() {
  if (flash_paused) {
    flash_paused = false;
    scanout_driver.load(std::memory_order_acquire)->resume_scanout();
  }
}

// Arduino owns the core lifecycle and its flash/OTA lockout handshake.
void setup1() {
  pimoroni::ST7701 *driver;
  while (!(driver = scanout_driver.load(std::memory_order_acquire))) {
    __wfe();
  }
  driver->init();
  const uint irq = multicore_doorbell_irq_num(_MFIFO::_doorbell);
  irq_add_shared_handler(irq, before_flash_lockout, 255);
  irq_add_shared_handler(irq, after_flash_lockout, 0);
  scanout_ready.store(true, std::memory_order_release);
  __sev();
}
void loop1() {
  if (pause_requested.load(std::memory_order_acquire) &&
      !scanout_paused.load(std::memory_order_relaxed)) {
    scanout_driver.load(std::memory_order_acquire)->pause_scanout();
    scanout_paused.store(true, std::memory_order_release);
    __sev();
  }
  __wfe();
}

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
                                 {spi1, 28, 26, 27, pimoroni::PIN_UNUSED, pimoroni::PIN_UNUSED, 45},
                                 framebuffers_[1], nullptr, 1);
  scanout_driver.store(driver_, std::memory_order_release);
  __sev();
  while (!scanout_ready.load(std::memory_order_acquire)) {
    delay(1);
  }
  drawing_buffer_ = 0;
  set_brightness(brightness_);
  ESP_LOGI(TAG, "480x480 RGB565, DMA-buffered scanout, PSRAM %u bytes",
           unsigned(rp2040.getPSRAMSize()));
}
void __not_in_flash_func(PrestoDisplay::wait_for_prefetch)() {
  driver_->wait_for_prefetch();
}
void PrestoDisplay::pause_scanout() {
  if (!scanout_ready.load(std::memory_order_acquire)) {
    return;
  }
  pause_requested.store(true, std::memory_order_release);
  __sev();
  while (!scanout_paused.load(std::memory_order_acquire)) {
    delay(1);
  }
}
void PrestoDisplay::on_shutdown() {
  pause_scanout();
}
void PrestoDisplay::update() {
  if (is_failed()) {
    return;
  }
  do_update_();
  present();
}
void PrestoDisplay::present(bool copy_previous) {
  driver_->set_framebuffer(framebuffers_[drawing_buffer_]);
  driver_->wait_for_vsync();
  drawing_buffer_ ^= 1;
  if (copy_previous) {
    memcpy(framebuffers_[drawing_buffer_], framebuffers_[drawing_buffer_ ^ 1], 480 * 480 * 2);
  }
}
void PrestoDisplay::fill(Color color) {
  if (!framebuffers_[drawing_buffer_]) {
    return;
  }
  const uint16_t pixel = ((uint16_t(color.red) & 0xf8) << 8) |
                         ((uint16_t(color.green) & 0xfc) << 3) | (color.blue >> 3);
  std::fill_n(framebuffers_[drawing_buffer_], 480 * 480, uint16_t((pixel >> 8) | (pixel << 8)));
}
void PrestoDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= 480 || y < 0 || y >= 480 || !framebuffers_[drawing_buffer_]) {
    return;
  }
  const uint16_t pixel = ((uint16_t(color.red) & 0xf8) << 8) |
                         ((uint16_t(color.green) & 0xfc) << 3) | (color.blue >> 3);
  framebuffers_[drawing_buffer_][y * 480 + x] = (pixel >> 8) | (pixel << 8);
}
void PrestoDisplay::set_brightness(float brightness) {
  brightness_ = std::clamp(brightness, 0.0f, 1.0f);
  if (driver_) {
    driver_->set_backlight(uint8_t(brightness_ * 255));
  }
}
void PrestoDisplay::dump_config() {
  LOG_DISPLAY("", "Pimoroni Presto", this);
}
} // namespace esphome::presto_display
