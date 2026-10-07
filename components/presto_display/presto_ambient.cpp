#include "esphome/core/defines.h"
#ifdef USE_PRESTO_AMBIENT
#include "presto_ambient.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "hardware/clocks.h"
#include "pio/presto_ws2812.pio.h"
#include <cstring>
namespace esphome::presto_display {
static const char *const TAG = "presto_ambient";
void PrestoAmbient::setup() {
  pio_set_gpio_base(pio_, 16);
  sm_ = pio_claim_unused_sm(pio_, true);
  const uint offset = pio_add_program(pio_, &presto_ws2812_program);
  pio_gpio_init(pio_, 33);
  pio_sm_set_consecutive_pindirs(pio_, sm_, 33, 1, true);
  auto c = presto_ws2812_program_get_default_config(offset);
  sm_config_set_sideset_pins(&c, 33);
  sm_config_set_out_shift(&c, false, true, 24);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  sm_config_set_clkdiv(&c, float(clock_get_hz(clk_sys)) / 8000000.0f);
  pio_sm_init(pio_, sm_, offset, &c);
  pio_sm_set_enabled(pio_, sm_, true);
  send_pixels_();
}
void PrestoAmbient::write_state(light::LightState *state) {
  send_pixels_();
}
void PrestoAmbient::set_pixels(const uint8_t *rgb) {
  memcpy(pixels_.data(), rgb, pixels_.size());
  send_pixels_();
}
void PrestoAmbient::send_pixels_() {
  // Wait for the preceding seven pixels and their reset latch to finish.
  while (uint32_t(micros() - last_write_) < 300) {
    delayMicroseconds(1);
  }
  for (int i = 0; i < 7; ++i) {
    const uint32_t grb = (uint32_t(pixels_[i * 3 + 1]) << 24) | (uint32_t(pixels_[i * 3]) << 16) |
                         (uint32_t(pixels_[i * 3 + 2]) << 8);
    pio_sm_put_blocking(pio_, sm_, grb);
  }
  last_write_ = micros();
}
light::ESPColorView PrestoAmbient::get_view_internal(int32_t index) const {
  uint8_t *p = pixels_.data() + index * 3;
  return {p, p + 1, p + 2, nullptr, effect_data_.data() + index, &correction_};
}
void PrestoAmbient::dump_config() {
  ESP_LOGCONFIG(TAG, "Presto rear LEDs: 7, GPIO33, PIO2");
}
} // namespace esphome::presto_display

#endif
