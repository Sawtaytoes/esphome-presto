#include "esphome/core/defines.h"
#ifdef USE_PRESTO_TOUCH
#include "presto_touch.h"
#include <algorithm>
#include "esphome/core/log.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/time.h"
namespace esphome::presto_display {
bool PrestoTouch::recover_() {
  i2c_deinit(i2c1);
  gpio_init(30);
  gpio_set_dir(30, GPIO_IN);
  gpio_pull_up(30);
  gpio_init(31);
  gpio_set_dir(31, GPIO_OUT);
  gpio_put(31, 1);
  for (int i = 0; i < 18 && !gpio_get(30); ++i) {
    gpio_put(31, 0);
    sleep_us(5);
    gpio_put(31, 1);
    sleep_us(5);
  }
  const bool released = gpio_get(30);
  i2c_init(i2c1, 400000);
  gpio_set_function(30, GPIO_FUNC_I2C);
  gpio_set_function(31, GPIO_FUNC_I2C);
  gpio_pull_up(30);
  gpio_pull_up(31);
  return released;
}
void PrestoTouch::setup() {
  gpio_init(32);
  gpio_set_dir(32, GPIO_IN);
  gpio_pull_up(32);
  recover_();
  if (x_raw_max_ == x_raw_min_) {
    x_raw_max_ = 480;
  }
  if (y_raw_max_ == y_raw_min_) {
    y_raw_max_ = 480;
  }
  uint8_t reg = 0xa3, chip = 0;
  const int sent = i2c_write_timeout_us(i2c1, 0x48, &reg, 1, true, 2000);
  const int got = sent == 1 ? i2c_read_timeout_us(i2c1, 0x48, &chip, 1, false, 2000) : -1;
  ESP_LOGI("presto_touch", "I2C1 touch probe: chip=%u read=%d SDA=%d", chip, got, gpio_get(30));
}
void PrestoTouch::dump_config() {
  ESP_LOGCONFIG("presto_touch", "Presto touch: I2C1 SDA30 SCL31 INT32, address 0x48");
}
void PrestoTouch::update_touches() {
  if (int32_t(millis() - retry_at_) < 0) {
    return;
  }
  if (gpio_get(32) && !was_down_) {
    return;
  }
  uint8_t reg = 0, data[15] = {};
  const int sent = i2c_write_timeout_us(i2c1, 0x48, &reg, 1, true, 2000);
  const int got = sent == 1 ? i2c_read_timeout_us(i2c1, 0x48, data, sizeof(data), false, 2000) : -1;
  if (got != sizeof(data)) {
    retry_at_ = millis() + 1000;
    was_down_ = false;
    ESP_LOGW("presto_touch", "Touch bus timeout; recovery released SDA=%d", recover_());
    return;
  }
  was_down_ = false;
  const uint8_t count = std::min(uint8_t(data[2] & 0xf), uint8_t(2));
  for (uint8_t index = 0; index < count; ++index) {
    const auto *p = data + 3 + 6 * index;
    const uint8_t phase = p[0] >> 6;
    // Retain the controller's final UP coordinates for fast swipes; the
    // next empty report releases the native ESPHome contact.
    if (phase == 3) {
      continue;
    }
    const int x = (p[0] & 0xf) << 8 | p[1], y = (p[2] & 0xf) << 8 | p[3];
    if (x < 480 && y < 480) {
      add_raw_touch_position_(p[2] >> 4, x, y);
      was_down_ = true;
    }
  }
}
} // namespace esphome::presto_display
#endif
