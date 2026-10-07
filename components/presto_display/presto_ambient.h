#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_AMBIENT
#include "esphome/components/light/addressable_light.h"
#include "hardware/pio.h"
#include <array>
namespace esphome::presto_display {
class PrestoAmbient : public light::AddressableLight {
 public:
  void setup() override;
  void dump_config() override;
  void write_state(light::LightState *state) override;
  int32_t size() const override { return 7; }
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }
  void clear_effect_data() override { effect_data_.fill(0); }
  void set_pixels(const uint8_t *rgb);

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;
  void send_pixels_();
  mutable std::array<uint8_t, 21> pixels_{};
  mutable std::array<uint8_t, 7> effect_data_{};
  PIO pio_{pio2};
  uint sm_{0};
  uint32_t last_write_{0};
};
} // namespace esphome::presto_display

#endif
