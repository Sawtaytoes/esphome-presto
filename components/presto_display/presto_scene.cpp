#include "esphome/core/defines.h"
#ifdef USE_PRESTO_NATIVE_SCENE
#include "presto_scene.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esphome::presto_display {
namespace {
constexpr size_t MAX_DOCUMENT = 6144;
constexpr size_t MAX_ITEMS = 32;
constexpr uint32_t INVALID_COLOR = 0x1000000;
void plain(lv_obj_t *obj) {
  lv_obj_remove_style_all(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
void label(lv_obj_t *obj, const lv_font_t *font, uint32_t color, const char *text) {
  lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
  lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(obj, text);
}
} // namespace

uint32_t NativeScene::color_(const char *text, uint32_t fallback) {
  if (!text) {
    return fallback;
  }
  if (strlen(text) != 7 || text[0] != '#') {
    return INVALID_COLOR;
  }
  uint32_t color = 0;
  for (int i = 1; i < 7; ++i) {
    const char c = text[i];
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
    if (digit < 0) {
      return INVALID_COLOR;
    }
    color = (color << 4) | digit;
  }
  return color;
}

bool NativeScene::text_(JsonVariant value, size_t maximum, bool identifier) {
  if (!value.is<const char *>()) {
    return false;
  }
  const char *s = value.as<const char *>();
  const size_t length = strlen(s);
  if (length > maximum || (identifier && length == 0)) {
    return false;
  }
  // IDs travel in comma-delimited event reports. Disallow control characters
  // and delimiters rather than allowing host data to fabricate another event.
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
    if (*p < 32 || (identifier && (*p == ',' || *p > 126))) {
      return false;
    }
  }
  return true;
}

bool NativeScene::validate_(JsonObject root) const {
  if (!root["version"].is<int>() || root["version"].as<int>() != 1 ||
      !root["revision"].is<uint32_t>() || root["revision"].as<uint32_t>() == 0 ||
      !text_(root["id"], 80, true) || !text_(root["title"], 100) ||
      !root["items"].is<JsonArray>()) {
    return false;
  }
  for (const char *key : {"subtitle", "value"}) {
    if (!root[key].isNull() && !text_(root[key], 100)) {
      return false;
    }
  }
  for (const char *key : {"background", "surface", "foreground", "muted", "accent"}) {
    if (!root[key].isNull() && (!root[key].is<const char *>() ||
                                color_(root[key].as<const char *>(), 0) == INVALID_COLOR)) {
      return false;
    }
  }
  for (const char *key : {"rowHeight", "gap", "padding", "radius"}) {
    if (!root[key].isNull() && !root[key].is<int>()) {
      return false;
    }
  }
  if ((!root["rowHeight"].isNull() &&
       (root["rowHeight"].as<int>() < 56 || root["rowHeight"].as<int>() > 160)) ||
      (!root["gap"].isNull() && (root["gap"].as<int>() < 0 || root["gap"].as<int>() > 20)) ||
      (!root["padding"].isNull() &&
       (root["padding"].as<int>() < 0 || root["padding"].as<int>() > 24)) ||
      (!root["radius"].isNull() &&
       (root["radius"].as<int>() < 0 || root["radius"].as<int>() > 24)) ||
      (!root["canBack"].isNull() && !root["canBack"].is<bool>())) {
    return false;
  }
  JsonArray items = root["items"];
  if (items.size() > MAX_ITEMS) {
    return false;
  }
  for (size_t i = 0; i < items.size(); ++i) {
    JsonObject item = items[i];
    if (item.isNull() || !text_(item["id"], 80, true) || !text_(item["title"], 100)) {
      return false;
    }
    for (const char *key : {"detail", "value"}) {
      if (!item[key].isNull() && !text_(item[key], 100)) {
        return false;
      }
    }
    if (!item["color"].isNull() && (!item["color"].is<const char *>() ||
                                    color_(item["color"].as<const char *>(), 0) == INVALID_COLOR)) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(item["id"].as<const char *>(), items[j]["id"].as<const char *>()) == 0) {
        return false;
      }
    }
  }
  return true;
}

void NativeScene::setup(lv_obj_t *screen, const lv_font_t *title_font, const lv_font_t *body_font,
                        const lv_font_t *detail_font, Publish status, Publish event) {
  screen_ = screen;
  title_font_ = title_font;
  body_font_ = body_font;
  detail_font_ = detail_font;
  status_ = std::move(status);
  event_ = std::move(event);
  status_("ready,scene-v1,list,32,6144");
}

void NativeScene::emit_(const char *type, const std::string &value) {
  if (event_) {
    event_(std::string(type) + "," + std::to_string(revision_) + "," + value);
  }
}

void NativeScene::clicked_(lv_event_t *event) {
  auto *item = static_cast<Item *>(lv_event_get_user_data(event));
  item->owner->emit_("click", item->id);
}

void NativeScene::scrolled_(lv_event_t *event) {
  auto *self = static_cast<NativeScene *>(lv_event_get_user_data(event));
  self->emit_("scroll", std::to_string(lv_obj_get_scroll_y(self->list_)));
}

void NativeScene::apply(const std::string &document) {
  if (!screen_ || document.size() > MAX_DOCUMENT) {
    if (status_) {
      status_("error,size_or_not_ready");
    }
    return;
  }
  const bool accepted = json::parse_json(document, [this](JsonObject root) {
    if (!validate_(root)) {
      return false;
    }
    const uint32_t revision = root["revision"];
    if (revision <= revision_) {
      status_("error,stale_revision," + std::to_string(revision_));
      return true;
    }
    draw_(root);
    status_("applied," + std::to_string(revision_) + "," + std::to_string(items_.size()));
    return true;
  });
  if (!accepted) {
    status_("error,invalid_scene");
  }
}

void NativeScene::draw_(JsonObject root) {
  const std::string id = root["id"].as<const char *>();
  const int offset = list_ && id == scene_id_ ? lv_obj_get_scroll_y(list_) : 0;
  if (list_) {
    lv_anim_delete(list_, nullptr);
  }
  lv_obj_clean(screen_);
  items_.clear();
  items_.reserve(root["items"].as<JsonArray>().size());
  scene_id_ = id;
  revision_ = root["revision"];
  can_back_ = root["canBack"] | false;
  const uint32_t background = color_(root["background"], 0x131822);
  const uint32_t surface = color_(root["surface"], 0x1F2631);
  const uint32_t foreground = color_(root["foreground"], 0xEDF0F5);
  const uint32_t muted = color_(root["muted"], 0xA9B2C1);
  const uint32_t accent = color_(root["accent"], 0x6DAE4F);
  const int padding = root["padding"] | 12;
  const int gap = root["gap"] | 6;
  const int row_height = root["rowHeight"] | 76;
  const int radius = root["radius"] | 8;
  plain(screen_);
  lv_obj_set_style_bg_color(screen_, lv_color_hex(background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, LV_PART_MAIN);
  header_ = lv_obj_create(screen_);
  plain(header_);
  lv_obj_set_size(header_, 480, 80);
  if (can_back_) {
    lv_obj_add_flag(header_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header_, clicked_, LV_EVENT_CLICKED, &back_);
  }
  title_ = lv_label_create(header_);
  lv_obj_set_pos(title_, 16, 10);
  lv_obj_set_size(title_, 340, 32);
  label(title_, title_font_, foreground, root["title"] | "");
  subtitle_ = lv_label_create(header_);
  lv_obj_set_pos(subtitle_, 16, 46);
  lv_obj_set_size(subtitle_, 440, 25);
  label(subtitle_, detail_font_, muted, root["subtitle"] | "");
  value_ = lv_label_create(header_);
  lv_obj_set_pos(value_, 355, 14);
  lv_obj_set_size(value_, 108, 32);
  lv_obj_set_style_text_align(value_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  label(value_, title_font_, accent, root["value"] | "");
  list_ = lv_obj_create(screen_);
  plain(list_);
  lv_obj_set_pos(list_, 0, 80);
  lv_obj_set_size(list_, 480, 400);
  lv_obj_set_style_bg_color(list_, lv_color_hex(background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list_, padding, LV_PART_MAIN);
  lv_obj_set_style_pad_row(list_, gap, LV_PART_MAIN);
  lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(list_, scrolled_, LV_EVENT_SCROLL_END, this);
  for (JsonObject item : root["items"].as<JsonArray>()) {
    items_.push_back({this, item["id"].as<const char *>()});
    lv_obj_t *row = lv_obj_create(list_);
    plain(row);
    lv_obj_set_size(row, 480 - padding * 2, row_height);
    lv_obj_set_style_bg_color(row, lv_color_hex(surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 3, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(color_(item["color"], accent)), LV_PART_MAIN);
    lv_obj_add_flag(
        row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_add_event_cb(row, clicked_, LV_EVENT_CLICKED, &items_.back());
    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_pos(name, 14, 9);
    lv_obj_set_size(name, 320, 28);
    label(name, body_font_, foreground, item["title"] | "");
    lv_obj_t *detail = lv_label_create(row);
    lv_obj_set_pos(detail, 14, row_height - 29);
    lv_obj_set_size(detail, 350, 24);
    label(detail, detail_font_, muted, item["detail"] | "");
    lv_obj_t *amount = lv_label_create(row);
    lv_obj_set_pos(amount, 340, 12);
    lv_obj_set_size(amount, 480 - padding * 2 - 354, 28);
    lv_obj_set_style_text_align(amount, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    label(amount, body_font_, foreground, item["value"] | "");
  }
  lv_obj_update_layout(list_);
  lv_obj_scroll_to_y(list_, offset, LV_ANIM_OFF);
}

void NativeScene::scroll_to(int offset) {
  if (list_) {
    lv_obj_scroll_to_y(list_, std::max(0, offset), LV_ANIM_OFF);
  }
}

void NativeScene::benchmark() {
  if (!list_) {
    return;
  }
  lv_obj_update_layout(list_);
  lv_anim_delete(list_, nullptr);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, list_);
  lv_anim_set_values(&anim, 0, lv_obj_get_scroll_y(list_) + lv_obj_get_scroll_bottom(list_));
  lv_anim_set_duration(&anim, 6000);
  lv_anim_set_reverse_duration(&anim, 6000);
  lv_anim_set_path_cb(&anim, lv_anim_path_linear);
  lv_anim_set_exec_cb(&anim, [](void *obj, int32_t y) {
    lv_obj_scroll_to_y(static_cast<lv_obj_t *>(obj), y, LV_ANIM_OFF);
  });
  lv_anim_start(&anim);
}
} // namespace esphome::presto_display
#endif
