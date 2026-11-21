#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_AMBIENT
#include "esphome/components/light/addressable_light.h"
#include "esphome/core/macros.h"
#include "esphome/core/version.h"
#include "hardware/pio.h"
#include <array>
namespace esphome::presto_display {
// ESPHome 2026.11 replaced AddressableLight::get_view_internal() with a color accessor
// object that the base class reads through `accessor_`.
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 11, 0)
#define PRESTO_AMBIENT_COLOR_ACCESSOR
#endif
class PrestoAmbient : public light::AddressableLight {
 public:
#ifdef PRESTO_AMBIENT_COLOR_ACCESSOR
  PrestoAmbient() {
    this->accessor_ = &this->color_accessor_;
  }
#endif
  void setup() override;
  void dump_config() override;
  void write_state(light::LightState *state) override;
  int32_t size() const override {
    return 7;
  }
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }
  void clear_effect_data() override {
    effect_data_.fill(0);
  }
  void set_pixels(const uint8_t *rgb);

 protected:
#ifndef PRESTO_AMBIENT_COLOR_ACCESSOR
  light::ESPColorView get_view_internal(int32_t index) const override;
#endif
  void send_pixels_();
  mutable std::array<uint8_t, 21> pixels_{};
  mutable std::array<uint8_t, 7> effect_data_{};
#ifdef PRESTO_AMBIENT_COLOR_ACCESSOR
  // Same buffer and byte order (R, G, B) as the old view; writes go through correction_.
  light::CorrectedBufferColorAccessor color_accessor_{
      pixels_.data(), effect_data_.data(), {0, 1, 2}, &this->correction_};
#endif
  PIO pio_{pio2};
  uint sm_{0};
  uint32_t last_write_{0};
};
} // namespace esphome::presto_display

#endif
