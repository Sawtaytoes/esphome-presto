#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_TOUCH
#include "esphome/components/touchscreen/touchscreen.h"
namespace esphome::presto_display {
class PrestoTouch : public touchscreen::Touchscreen {
 public:
  void setup() override;
  void dump_config() override;

 protected:
  void update_touches() override;
  bool recover_();
  uint32_t retry_at_{0};
  bool was_down_{false};
};
} // namespace esphome::presto_display

#endif
