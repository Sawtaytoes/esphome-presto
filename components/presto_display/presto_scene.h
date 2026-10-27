#pragma once
#include "esphome/core/defines.h"
#ifdef USE_PRESTO_NATIVE_SCENE
#include "esphome/components/json/json_util.h"
#include "lvgl.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace esphome::presto_display {

// A bounded, declarative list scene. No points, music, or other application rules
// live here: the host supplies text, geometry, colors, and semantic item IDs.
class NativeScene {
 public:
  using Publish = std::function<void(const std::string &)>;
  void setup(lv_obj_t *screen, const lv_font_t *title_font, const lv_font_t *body_font,
             const lv_font_t *detail_font, Publish status, Publish event);
  void apply(const std::string &document);
  void scroll_to(int offset);
  void benchmark();

 protected:
  struct Item {
    NativeScene *owner;
    std::string id;
  };
  bool validate_(JsonObject root) const;
  void draw_(JsonObject root);
  void emit_(const char *type, const std::string &value);
  static void clicked_(lv_event_t *event);
  static void scrolled_(lv_event_t *event);
  static uint32_t color_(const char *text, uint32_t fallback);
  static bool text_(JsonVariant value, size_t maximum, bool identifier = false);

  lv_obj_t *screen_{nullptr}, *header_{nullptr}, *title_{nullptr}, *subtitle_{nullptr},
      *value_{nullptr}, *list_{nullptr};
  const lv_font_t *title_font_{nullptr}, *body_font_{nullptr}, *detail_font_{nullptr};
  Publish status_, event_;
  std::vector<Item> items_;
  Item back_{this, "back"};
  std::string scene_id_;
  uint32_t revision_{0};
  bool can_back_{false};
};

} // namespace esphome::presto_display
#endif
