#pragma once
// Which parts of the back framebuffer are out of date, for a renderer that redraws only
// what changed (LVGL's partial mode). No board or ESPHome types, so the host tests compile
// it directly.
#include <vector>
#include "presto_stream.h"

namespace esphome::presto_display {

// Double buffering for a partial renderer. Each frame draws only its changed areas into the
// back framebuffer, which still holds the frame before last. Before that frame is presented,
// the areas the PREVIOUS frame drew, minus the ones this frame drew, are copied from the
// visible framebuffer (which holds them, unchanged since). A continuous scroll redraws the
// same area every frame, so it copies nothing.
class FrameRegions {
 public:
  // Record an area drawn into the back framebuffer this frame. A strip directly below the
  // last one with the same columns extends it, so a renderer's strips stay one rectangle.
  void add(const Rect &rect) {
    if (rect.empty()) {
      return;
    }
    if (!current_.empty()) {
      Rect &last = current_.back();
      if (last.x == rect.x && last.width == rect.width && last.y + last.height == rect.y) {
        last.height += rect.height;
        return;
      }
    }
    current_.push_back(rect);
  }
  bool has_current() const { return !current_.empty(); }
  const std::vector<Rect> &current() const { return current_; }
  const std::vector<Rect> &previous() const { return previous_; }

  // The areas to copy from the visible framebuffer into the back one before presenting.
  std::vector<Rect> stale() const {
    std::vector<Rect> pieces(previous_), next;
    Rect parts[4];
    for (const Rect &drawn : current_) {
      next.clear();
      for (const Rect &piece : pieces) {
        const int count = subtract(piece, drawn, parts);
        next.insert(next.end(), parts, parts + count);
      }
      pieces.swap(next);
      if (pieces.empty()) {
        break;
      }
    }
    return pieces;
  }

  // The frame was presented: what it drew is now what the new back framebuffer lacks.
  void presented() {
    previous_.swap(current_);
    current_.clear();
  }

 protected:
  std::vector<Rect> current_, previous_;
};

} // namespace esphome::presto_display
