#pragma once
// Scroll window: panel rows [top, bottom) scanned out of a tall picture in PSRAM instead of
// the framebuffer, starting at the picture's row `offset`. Moving the window is a change of
// the scanout's source address, so a drag costs no PSRAM writes at all. Pure C++ with no
// ESPHome or Pico types, so the host tests compile and drive the exact code that ships;
// the display core's interrupt handler inlines `plan_batch`.
#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(__GNUC__)
#define PRESTO_SCROLL_INLINE inline __attribute__((always_inline))
#else
#define PRESTO_SCROLL_INLINE inline
#endif

namespace esphome::presto_display {

// The scanout fetches 8 rows per batch; with window edges on the same grid every batch comes
// wholly from the framebuffer or wholly from the picture.
constexpr int WINDOW_ALIGN = 8;
// The tallest picture the firmware allocates, and the PSRAM it leaves free after doing so.
constexpr int SCROLL_MAX_ROWS = 4096;
constexpr size_t SCROLL_PSRAM_RESERVE = 1024 * 1024;
// A smaller allocation is not worth having: it could not scroll a window of half the panel.
constexpr int SCROLL_MIN_ROWS = 480;

struct Window {
  bool is_shown{false};
  int top{0}, bottom{0}, offset{0};
  int height() const { return bottom - top; }
};

// One 32-bit word, so core 0 hands a whole window to the display core in one store: bit 31
// shown, bits 24-30 top / 8, bits 17-23 bottom / 8, bits 0-16 offset. Zero is hidden.
constexpr uint32_t WINDOW_SHOWN = 1u << 31;

PRESTO_SCROLL_INLINE uint32_t pack_window(const Window &window) {
  if (!window.is_shown) {
    return 0;
  }
  return WINDOW_SHOWN | uint32_t(window.top / WINDOW_ALIGN) << 24 |
         uint32_t(window.bottom / WINDOW_ALIGN) << 17 | (uint32_t(window.offset) & 0x1ffff);
}

PRESTO_SCROLL_INLINE Window unpack_window(uint32_t word) {
  Window window;
  window.is_shown = (word & WINDOW_SHOWN) != 0;
  if (window.is_shown) {
    window.top = int(word >> 24 & 0x7f) * WINDOW_ALIGN;
    window.bottom = int(word >> 17 & 0x7f) * WINDOW_ALIGN;
    window.offset = int(word & 0x1ffff);
  }
  return window;
}

// Where the scanout reads panel row `row`: the framebuffer's row, or inside a shown window
// the picture's row `row - top + offset`. Rows are `width` pixels.
PRESTO_SCROLL_INLINE const uint16_t *window_row(const uint16_t *framebuffer,
                                                const uint16_t *picture, int width, int row,
                                                uint32_t word) {
  if ((word & WINDOW_SHOWN) && picture) {
    const int top = int(word >> 24 & 0x7f) * WINDOW_ALIGN;
    const int bottom = int(word >> 17 & 0x7f) * WINDOW_ALIGN;
    if (row >= top && row < bottom) {
      return picture + size_t(width) * size_t(row - top + int(word & 0x1ffff));
    }
  }
  return framebuffer + size_t(width) * size_t(row);
}

// The handoff between core 0, which requests a window, and the display core, which latches
// it once per frame so every row of one frame uses the same window.
class WindowLatch {
 public:
  // Core 0.
  void request(uint32_t word) { requested_.store(word, std::memory_order_release); }
  uint32_t requested() const { return requested_.load(std::memory_order_acquire); }
  // Counts latches. Two latches after a request, nothing reads the window it replaced.
  uint32_t latches() const { return latches_.load(std::memory_order_acquire); }

  // Display core.
  PRESTO_SCROLL_INLINE uint32_t latch() {
    const uint32_t word = requested();
    publish(word);
    return word;
  }
  // A frame swap carries the window its first rows were copied with.
  PRESTO_SCROLL_INLINE void publish(uint32_t word) {
    active_ = word;
    latches_.fetch_add(1, std::memory_order_acq_rel);
  }
  uint32_t active() const { return active_; }

 protected:
  std::atomic<uint32_t> requested_{0}, latches_{0};
  uint32_t active_{0};
};

// The source of the 8-row prefetch batch that starts at panel row `row`. A frame's first
// batch (row 0) latches the requested window; that batch is fetched while the previous frame
// is still being scanned, so the window changes from the start of the next frame.
PRESTO_SCROLL_INLINE const uint16_t *plan_batch(WindowLatch &latch, const uint16_t *framebuffer,
                                                const uint16_t *picture, int width, int row) {
  const uint32_t word = row == 0 ? latch.latch() : latch.active();
  return window_row(framebuffer, picture, width, row, word);
}

// A finger on the window. A contact that starts inside it is reported to CastKit as usual,
// so a tap still reaches the page; once it travels THRESHOLD pixels vertically, and more
// vertically than sideways, the board takes it over and the picture follows the finger from
// the press. A sideways drag stays CastKit's (it may be a view swipe). A quick release
// glides on with friction; a press during the glide stops it and is never reported.
class ScrollDrag {
 public:
  enum class Verdict : uint8_t {
    FORWARD, // report the contact as usual
    TAKE, // the board takes the contact over from this report on; CastKit cancels it
    OWN, // the board owns the contact; report nothing
  };
  static constexpr int THRESHOLD = 12;
  // A release faster than this (pixels per millisecond) glides, and a glide slower than
  // STOP_SPEED stops.
  static constexpr float FLING_SPEED = 0.3f;
  static constexpr float STOP_SPEED = 0.02f;
  // The glide loses about 63% of its speed every DECAY_MS.
  static constexpr float DECAY_MS = 325.0f;
  // A finger that rested this long before lifting does not fling.
  static constexpr uint32_t REST_MS = 80;

  // phase: 0 press, 1 move, 2 release. `window` is what the scanout shows (or hidden) and
  // `max_offset` the picture's height less the window's. A move of an owned contact
  // updates `window.offset` and sets `is_moved` when it changed.
  Verdict touch(int phase, int x, int y, uint32_t now, Window &window, int max_offset,
                bool &is_moved) {
    is_moved = false;
    if (phase == 0) {
      start_x_ = x;
      start_y_ = y;
      last_y_ = y;
      last_at_ = now;
      speed_ = 0;
      if (is_gliding_ && window.is_shown) {
        // A press during a glide only stops it, as on a phone; it is never a tap.
        is_gliding_ = false;
        start_offset_ = window.offset;
        state_ = State::OWNED;
        return Verdict::OWN;
      }
      is_gliding_ = false;
      const bool is_inside = window.is_shown && y >= window.top && y < window.bottom;
      state_ = is_inside ? State::PENDING : State::FORWARDED;
      start_offset_ = window.offset;
      return Verdict::FORWARD;
    }
    if (phase == 1) {
      if (state_ == State::PENDING) {
        const int dx = x - start_x_, dy = y - start_y_;
        const int across = dx < 0 ? -dx : dx, along = dy < 0 ? -dy : dy;
        if (!window.is_shown) {
          state_ = State::FORWARDED;
          return Verdict::FORWARD;
        }
        if (along >= THRESHOLD && along > across) {
          state_ = State::OWNED;
          follow_(y, now, window, max_offset, is_moved);
          return Verdict::TAKE;
        }
        if (across >= THRESHOLD) {
          state_ = State::FORWARDED;
        }
        return Verdict::FORWARD;
      }
      if (state_ == State::OWNED) {
        if (window.is_shown) {
          follow_(y, now, window, max_offset, is_moved);
        }
        return Verdict::OWN;
      }
      return Verdict::FORWARD;
    }
    // Release.
    const bool is_owned = state_ == State::OWNED;
    state_ = State::NONE;
    if (!is_owned) {
      return Verdict::FORWARD;
    }
    if (window.is_shown) {
      const bool is_fling =
          now - last_at_ < REST_MS && (speed_ >= FLING_SPEED || speed_ <= -FLING_SPEED);
      if (is_fling) {
        is_gliding_ = true;
        glide_at_ = now;
        position_ = float(window.offset);
      } else {
        is_settled_ = true;
      }
    }
    return Verdict::OWN;
  }

  // Advance a glide to `now`. True while it continues.
  bool glide(uint32_t now, Window &window, int max_offset, bool &is_moved) {
    is_moved = false;
    if (!is_gliding_) {
      return false;
    }
    if (!window.is_shown) {
      is_gliding_ = false;
      return false;
    }
    const float elapsed = float(now - glide_at_);
    glide_at_ = now;
    if (elapsed > 0) {
      // Integrate the exponential decay exactly over the step.
      const float keep = decay_(elapsed);
      position_ += speed_ * DECAY_MS * (1.0f - keep);
      speed_ *= keep;
    }
    int offset = int(position_ + 0.5f);
    bool is_stopped = speed_ < STOP_SPEED && speed_ > -STOP_SPEED;
    if (offset <= 0) {
      offset = 0;
      is_stopped = true;
    } else if (offset >= max_offset) {
      offset = max_offset;
      is_stopped = true;
    }
    if (offset != window.offset) {
      window.offset = offset;
      is_moved = true;
    }
    if (is_stopped) {
      is_gliding_ = false;
      is_settled_ = true;
    }
    return is_gliding_;
  }

  // True once after a local scroll comes to rest: the release, or the end of its glide.
  bool take_settled() {
    const bool is_settled = is_settled_;
    is_settled_ = false;
    return is_settled;
  }
  bool is_gliding() const { return is_gliding_; }
  bool is_owned() const { return state_ == State::OWNED; }
  // The window went away: forget the contact and any glide. Later reports of the contact are
  // forwarded, and CastKit, which no longer has it, ignores them.
  void reset() {
    state_ = State::NONE;
    is_gliding_ = is_settled_ = false;
  }

 protected:
  enum class State : uint8_t { NONE, PENDING, FORWARDED, OWNED };

  void follow_(int y, uint32_t now, Window &window, int max_offset, bool &is_moved) {
    int offset = start_offset_ - (y - start_y_);
    offset = offset < 0 ? 0 : offset > max_offset ? max_offset : offset;
    const uint32_t elapsed = now - last_at_;
    if (elapsed > 0) {
      // Picture rows per millisecond, smoothed over the last few reports.
      const float instant = float(last_y_ - y) / float(elapsed);
      speed_ = 0.6f * instant + 0.4f * speed_;
      last_at_ = now;
    }
    last_y_ = y;
    if (offset != window.offset) {
      window.offset = offset;
      is_moved = true;
    }
  }

  static float decay_(float elapsed) {
    // exp(-elapsed / DECAY_MS) without libm: raise a short Taylor step to the 16th power.
    const float x = -elapsed / DECAY_MS / 16.0f;
    if (x < -1.0f) {
      return 0;
    }
    float value = 1.0f + x * (1.0f + x * (0.5f + x / 6.0f));
    for (int i = 0; i < 4; ++i) {
      value *= value;
    }
    return value < 0 ? 0 : value;
  }

  State state_{State::NONE};
  int start_x_{0}, start_y_{0}, start_offset_{0}, last_y_{0};
  uint32_t last_at_{0}, glide_at_{0};
  float speed_{0}, position_{0};
  bool is_gliding_{false}, is_settled_{false};
};

} // namespace esphome::presto_display
