// Host stand-in for the board, driving the exact Receiver that ships. Memory is plain RAM,
// `show` swaps a pointer at once, and time moves only when the test advances it. The scroll
// window is requested by the receiver and latched like the scanout does: at a swap, at a
// release, or when the test starts a frame.
#include "presto_receiver.h"
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::presto_display;

namespace {
class HostPanel : public Panel {
 public:
  HostPanel() : own_{std::vector<uint8_t>(FRAME_BYTES), std::vector<uint8_t>(FRAME_BYTES)} {
    shown_ = own_[1].data();
  }
  void write(uint8_t *to, const uint8_t *from, size_t size) override {
    // Nothing may ever write into the buffer the scanout is reading, nor into the picture
    // rows a window that is requested or still latched reads.
    if (to < shown_ + FRAME_BYTES && shown_ < to + size) {
      ++visible_writes;
    }
    if (overlaps_window_(requested_, to, size) || overlaps_window_(latched_, to, size)) {
      ++visible_writes;
    }
    memcpy(to, from, size);
    written += size;
    ++write_calls;
  }
  void read(uint8_t *to, const uint8_t *from, size_t size) override { memcpy(to, from, size); }
  uint8_t *framebuffer(int index) override { return own_[index & 1].data(); }
  uint8_t *visible() override { return shown_; }
  void show(uint8_t *buffer) override {
    latched_ = requested_;
    shown_ = buffer;
    if (buffer == own_[0].data()) {
      drawing_ = 1;
    } else if (buffer == own_[1].data()) {
      drawing_ = 0;
    }
    ++shows;
  }
  // Like PrestoDisplay: draw into the drawing buffer, present it, copy it to the other.
  void blank() override {
    latched_ = requested_;
    uint8_t *back = own_[drawing_].data();
    memset(back, 0, FRAME_BYTES);
    for (size_t i = 0; i < size_t(PANEL) * 4; ++i) {
      back[i * 2] = 0xf8;
    }
    shown_ = back;
    drawing_ ^= 1;
    memcpy(own_[drawing_].data(), back, FRAME_BYTES);
    ++shows;
  }
  void publish(const char *event) override {
    events.emplace_back(event);
    count_(0);
  }
  void publish_slot(const char *event) override {
    slot_events.emplace_back(event);
    count_(1);
  }
  void publish_timing(const char *report) override {
    timings.emplace_back(report);
    count_(2);
  }
  void set_window(const Window &window) override {
    requested_ = window;
    ++window_requests;
  }
  void release_window() override {
    latched_ = requested_;
    ++releases;
  }
  // The scanout's next frame start.
  void frame_start() { latched_ = requested_; }
  const Window &latched() const { return latched_; }
  uint8_t *scroll_picture{nullptr};
  int window_requests{0}, releases{0};
  // Every harness entry point is one ESPHome call: an action, or one loop() step.
  void begin_call() { published_[0] = published_[1] = published_[2] = 0; }
  uint32_t micros() override { return now_ms * 1000; }
  uint32_t millis() override { return now_ms; }
  uint32_t overruns() override { return 0; }
  size_t free_psram() override { return 4242; }

  std::vector<std::string> events, slot_events, timings;
  // ESPHome keeps only the newest unsent state per entity: one call must not publish two.
  int double_publishes{0}, visible_writes{0};
  uint32_t now_ms{1000};
  size_t written{0};
  size_t write_calls{0};
  int shows{0};

 private:
  void count_(int entity) {
    if (++published_[entity] > 1) {
      ++double_publishes;
    }
  }
  bool overlaps_window_(const Window &window, const uint8_t *to, size_t size) const {
    if (!window.is_shown || !scroll_picture) {
      return false;
    }
    const uint8_t *first = scroll_picture + size_t(window.offset) * PANEL * 2;
    const uint8_t *last = first + size_t(window.height()) * PANEL * 2;
    return to < last && first < to + size;
  }
  Window requested_, latched_;
  int published_[3]{};
  std::vector<uint8_t> own_[2];
  uint8_t *shown_;
  int drawing_{0};
};

struct Bench {
  HostPanel panel;
  Receiver receiver{&panel};
  std::vector<std::vector<uint8_t>> slots;
  std::vector<uint8_t> jpeg;
  std::vector<uint8_t> scroll;
  std::vector<uint8_t> composite;
};
Bench &begin(void *bench) {
  auto *b = static_cast<Bench *>(bench);
  b->panel.begin_call();
  return *b;
}

bool inside(const Rect &rect, int x, int y) {
  return !rect.empty() && x >= rect.x && x < rect.x + rect.width && y >= rect.y &&
         y < rect.y + rect.height;
}
} // namespace

extern "C" {
// `jpeg_capacity` 0 is a board without the JPEG buffer; `scroll_rows` 0 one without the
// scroll picture.
void *create(int slot_count, size_t jpeg_capacity, int scroll_rows) {
  auto *bench = new Bench;
  if (!bench->receiver.begin()) {
    delete bench;
    return nullptr;
  }
  if (jpeg_capacity) {
    bench->jpeg.assign(jpeg_capacity, uint8_t(0xa5));
    bench->receiver.set_jpeg_buffer(bench->jpeg.data(), jpeg_capacity);
  }
  std::vector<uint8_t *> pointers;
  for (int i = 0; i < slot_count; ++i) {
    bench->slots.emplace_back(FRAME_BYTES, uint8_t(0x5a));
    pointers.push_back(bench->slots.back().data());
  }
  bench->receiver.set_slots(pointers.data(), slot_count);
  if (scroll_rows > 0) {
    bench->scroll.assign(size_t(scroll_rows) * PANEL * 2, uint8_t(0x3c));
    bench->panel.scroll_picture = bench->scroll.data();
    bench->receiver.set_scroll_picture(bench->scroll.data(), scroll_rows);
  }
  return bench;
}
void destroy(void *bench) {
  delete static_cast<Bench *>(bench);
}
void frame_chunk(void *bench, int frame_id, int touch_id, int format, int offset, bool final_chunk,
                 const char *encoded, size_t size) {
  begin(bench).receiver.frame_chunk(frame_id, touch_id, format, offset, final_chunk, encoded, size);
}
void slot_chunk(void *bench, int slot, int frame_id, int format, int offset, bool final_chunk,
                const char *encoded, size_t size) {
  begin(bench).receiver.slot_chunk(slot, frame_id, format, offset, final_chunk, encoded, size);
}
void show_slot(void *bench, int slot, int touch_id) {
  begin(bench).receiver.show_slot(slot, touch_id);
}
void drop_slots(void *bench) {
  begin(bench).receiver.drop_slots();
}
void scroll_chunk(void *bench, int frame_id, int format, int offset, bool final_chunk,
                  const char *encoded, size_t size) {
  begin(bench).receiver.scroll_chunk(frame_id, format, offset, final_chunk, encoded, size);
}
void scroll_show(void *bench, int frame_id, int top, int bottom, int scroll_y) {
  begin(bench).receiver.scroll_show(frame_id, top, bottom, scroll_y);
}
void scroll_hide(void *bench) {
  begin(bench).receiver.scroll_hide();
}
// One contact report; `out` receives is_forwarded, phase, flags, offset.
void touch(void *bench, int phase, int x, int y, int *out) {
  const Receiver::TouchReport report = begin(bench).receiver.touch(phase, x, y);
  out[0] = report.is_forwarded;
  out[1] = report.phase;
  out[2] = report.flags;
  out[3] = report.offset;
}
void frame_start(void *bench) {
  static_cast<Bench *>(bench)->panel.frame_start();
}
// What the glass shows: the visible framebuffer, with the latched window's rows from the
// scroll picture.
const uint8_t *composite(void *bench) {
  auto *b = static_cast<Bench *>(bench);
  const uint8_t *shown = b->panel.visible();
  b->composite.assign(shown, shown + FRAME_BYTES);
  const Window &window = b->panel.latched();
  if (window.is_shown) {
    const size_t row_bytes = size_t(PANEL) * 2;
    memcpy(b->composite.data() + size_t(window.top) * row_bytes,
           b->scroll.data() + size_t(window.offset) * row_bytes,
           size_t(window.height()) * row_bytes);
  }
  return b->composite.data();
}
// The latched window: is_shown, top, bottom, offset.
void latched_window(void *bench, int *out) {
  const Window &window = static_cast<Bench *>(bench)->panel.latched();
  out[0] = window.is_shown;
  out[1] = window.top;
  out[2] = window.bottom;
  out[3] = window.offset;
}
const uint8_t *scroll_memory(void *bench) {
  return static_cast<Bench *>(bench)->scroll.data();
}
unsigned scroll_frame(void *bench) {
  return static_cast<Bench *>(bench)->receiver.scroll_frame();
}
int scroll_height(void *bench) {
  return static_cast<Bench *>(bench)->receiver.scroll_height();
}
int releases(void *bench) {
  return static_cast<Bench *>(bench)->panel.releases;
}
int window_requests(void *bench) {
  return static_cast<Bench *>(bench)->panel.window_requests;
}

// The scanout's batch planner (presto_scroll.h) on two stand-in buffers. Returns the panel
// row a batch reads from the framebuffer, or 100000 plus the picture row it reads.
void *latch_create() {
  return new WindowLatch;
}
void latch_destroy(void *latch) {
  delete static_cast<WindowLatch *>(latch);
}
void latch_request(void *latch, bool is_shown, int top, int bottom, int offset) {
  Window window;
  window.is_shown = is_shown;
  window.top = top;
  window.bottom = bottom;
  window.offset = offset;
  static_cast<WindowLatch *>(latch)->request(pack_window(window));
}
void latch_publish_requested(void *latch) {
  auto *window_latch = static_cast<WindowLatch *>(latch);
  window_latch->publish(window_latch->requested());
}
unsigned latch_count(void *latch) {
  return static_cast<WindowLatch *>(latch)->latches();
}
int plan_row(void *latch, int row, bool has_picture) {
  static uint16_t framebuffer[PANEL * PANEL];
  static uint16_t picture[SCROLL_MAX_ROWS * PANEL];
  const uint16_t *source = plan_batch(*static_cast<WindowLatch *>(latch), framebuffer,
                                      has_picture ? picture : nullptr, PANEL, row);
  if (source >= picture && source < picture + SCROLL_MAX_ROWS * PANEL) {
    return 100000 + int((source - picture) / PANEL);
  }
  return int((source - framebuffer) / PANEL);
}
// pack_window then unpack_window: is_shown, top, bottom, offset.
void round_trip_window(bool is_shown, int top, int bottom, int offset, int *out) {
  Window window;
  window.is_shown = is_shown;
  window.top = top;
  window.bottom = bottom;
  window.offset = offset;
  const Window back = unpack_window(pack_window(window));
  out[0] = back.is_shown;
  out[1] = back.top;
  out[2] = back.bottom;
  out[3] = back.offset;
}
bool step(void *bench) {
  return begin(bench).receiver.step();
}
void advance(void *bench, unsigned ms) {
  static_cast<Bench *>(bench)->panel.now_ms += ms;
}
int event_count(void *bench) {
  return int(static_cast<Bench *>(bench)->panel.events.size());
}
const char *event(void *bench, int index) {
  return static_cast<Bench *>(bench)->panel.events[index].c_str();
}
int slot_event_count(void *bench) {
  return int(static_cast<Bench *>(bench)->panel.slot_events.size());
}
const char *slot_event(void *bench, int index) {
  return static_cast<Bench *>(bench)->panel.slot_events[index].c_str();
}
int double_publishes(void *bench) {
  return static_cast<Bench *>(bench)->panel.double_publishes;
}
int visible_writes(void *bench) {
  return static_cast<Bench *>(bench)->panel.visible_writes;
}
// Pixels where a framebuffer differs from the picture outside the area the receiver
// tracks as stale (or is decoding a live frame into). Must always be 0.
int untracked_differences(void *bench) {
  auto *b = static_cast<Bench *>(bench);
  if (!b->receiver.is_attached()) {
    return 0;
  }
  const uint8_t *shown = b->panel.visible();
  int count = 0;
  for (int index = 0; index < 2; ++index) {
    const uint8_t *own = b->panel.framebuffer(index);
    if (own == shown) {
      continue;
    }
    const Rect stale = b->receiver.stale(index);
    const Rect live = index == b->receiver.back_index() ? b->receiver.live_rect() : Rect{};
    for (int y = 0; y < PANEL; ++y) {
      for (int x = 0; x < PANEL; ++x) {
        const size_t at = (size_t(y) * PANEL + x) * 2;
        if (!inside(stale, x, y) && !inside(live, x, y) &&
            (own[at] != shown[at] || own[at + 1] != shown[at + 1])) {
          ++count;
        }
      }
    }
  }
  return count;
}
int timing_count(void *bench) {
  return int(static_cast<Bench *>(bench)->panel.timings.size());
}
const char *timing(void *bench, int index) {
  return static_cast<Bench *>(bench)->panel.timings[index].c_str();
}
const uint8_t *visible(void *bench) {
  return static_cast<Bench *>(bench)->panel.visible();
}
const uint8_t *framebuffer(void *bench, int index) {
  return static_cast<Bench *>(bench)->panel.framebuffer(index);
}
const uint8_t *slot_memory(void *bench, int index) {
  return static_cast<Bench *>(bench)->slots[index].data();
}
unsigned slot_frame(void *bench, int slot) {
  return static_cast<Bench *>(bench)->receiver.slot_frame(slot);
}
int presented_frame(void *bench) {
  return static_cast<Bench *>(bench)->receiver.presented_frame();
}
int touch_frame(void *bench) {
  return static_cast<Bench *>(bench)->receiver.touch_frame();
}
bool is_settling(void *bench) {
  return static_cast<Bench *>(bench)->receiver.is_settling();
}
bool is_slot_visible(void *bench) {
  return static_cast<Bench *>(bench)->receiver.is_slot_visible();
}
const char *codecs(void *bench) {
  return static_cast<Bench *>(bench)->receiver.codecs();
}
int shows(void *bench) {
  return static_cast<Bench *>(bench)->panel.shows;
}
size_t written(void *bench) {
  return static_cast<Bench *>(bench)->panel.written;
}
size_t write_calls(void *bench) {
  return static_cast<Bench *>(bench)->panel.write_calls;
}
}
