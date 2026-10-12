#pragma once
#include <cstdint>
namespace esphome::presto_display {
// What the receiver does with one touch report, decided on the board so a dark panel lights at
// once.
enum class TouchAction : uint8_t { DROP, WAKE, FORWARD };
// Tap-to-wake: while CastKit holds the backlight at zero, a press lights the panel at the wake
// level for a fixed window instead of reaching the page. The waking contact is swallowed through
// its release so it cannot press whatever is under the finger; later contacts are forwarded and
// restart the window. Pure state, no ESPHome types, so the host tests exercise this exact code.
class WakeLatch {
 public:
  void set_backlight(int percent) {
    backlight_ = percent;
    if (percent > 0) {
      is_awake_ = false;
    }
  }
  // Zero percent disables waking; the duration is clamped to 1-600 seconds.
  void set_wake(int percent, int seconds) {
    if (percent < 0 || percent > 100) {
      return;
    }
    wake_percent_ = percent;
    duration_ms_ = static_cast<uint32_t>(seconds < 1 ? 1 : seconds > 600 ? 600 : seconds) * 1000;
    if (!percent) {
      is_awake_ = false;
    }
  }
  // phase: 0 press, 1 move, 2 release.
  TouchAction touch(int phase, uint32_t now) {
    if (backlight_ < 0) {
      return TouchAction::DROP;
    }
    if (is_swallowing_) {
      if (phase == 2) {
        is_swallowing_ = false;
      }
      awake_until_ = now + duration_ms_;
      return TouchAction::DROP;
    }
    if (backlight_ > 0) {
      return TouchAction::FORWARD;
    }
    if (is_awake_) {
      awake_until_ = now + duration_ms_;
      return TouchAction::FORWARD;
    }
    if (phase != 0 || !wake_percent_) {
      return TouchAction::DROP;
    }
    is_awake_ = is_swallowing_ = true;
    awake_until_ = now + duration_ms_;
    return TouchAction::WAKE;
  }
  // True once, when the window lapses. A finger still on the glass holds the panel awake.
  bool expire(uint32_t now) {
    if (!is_awake_ || is_swallowing_ || static_cast<int32_t>(now - awake_until_) < 0) {
      return false;
    }
    is_awake_ = false;
    return true;
  }
  int brightness() const { return backlight_ > 0 ? backlight_ : is_awake_ ? wake_percent_ : 0; }
  bool is_awake() const { return is_awake_; }

 protected:
  int backlight_{-1}, wake_percent_{0};
  uint32_t duration_ms_{30000}, awake_until_{0};
  bool is_awake_{false}, is_swallowing_{false};
};
} // namespace esphome::presto_display
