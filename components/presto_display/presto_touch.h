#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_TOUCH
#include "esphome/components/touchscreen/touchscreen.h"
namespace esphome::presto_display {
class PrestoTouch : public touchscreen::Touchscreen {
 public:
  void setup() override;
  void dump_config() override;
  uint32_t take_max_sample_gap_us() {
    const uint32_t result = max_sample_gap_us_;
    max_sample_gap_us_ = 0;
    return result;
  }
  void reset_sample_gap() { last_sample_us_ = max_sample_gap_us_ = 0; }

 protected:
  void update_touches() override;
  bool recover_();
  uint32_t retry_at_{0};
  bool was_down_{false};
  uint32_t last_sample_us_{0}, max_sample_gap_us_{0};
};
} // namespace esphome::presto_display

#endif
