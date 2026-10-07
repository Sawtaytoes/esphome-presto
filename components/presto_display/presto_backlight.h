#pragma once
#include "presto_display.h"
#include "esphome/components/output/float_output.h"
namespace esphome::presto_display {
class PrestoBacklight : public output::FloatOutput {
 public:
  void set_display(PrestoDisplay *display) { display_ = display; }
 protected:
  void write_state(float state) override { display_->set_brightness(state); }
  PrestoDisplay *display_;
};
}
