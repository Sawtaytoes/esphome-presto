// Host stand-in for the board, driving the exact Receiver that ships. Memory is plain RAM,
// `show` swaps a pointer at once, and time moves only when the test advances it.
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
    // Nothing may ever write into the buffer the scanout is reading.
    if (to < shown_ + FRAME_BYTES && shown_ < to + size) {
      ++visible_writes;
    }
    memcpy(to, from, size);
    written += size;
  }
  void read(uint8_t *to, const uint8_t *from, size_t size) override { memcpy(to, from, size); }
  uint8_t *framebuffer(int index) override { return own_[index & 1].data(); }
  uint8_t *visible() override { return shown_; }
  void show(uint8_t *buffer) override {
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
  int shows{0};

 private:
  void count_(int entity) {
    if (++published_[entity] > 1) {
      ++double_publishes;
    }
  }
  int published_[3]{};
  std::vector<uint8_t> own_[2];
  uint8_t *shown_;
  int drawing_{0};
};

struct Bench {
  HostPanel panel;
  Receiver receiver{&panel};
  std::vector<std::vector<uint8_t>> slots;
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
void *create(int slot_count) {
  auto *bench = new Bench;
  if (!bench->receiver.begin()) {
    delete bench;
    return nullptr;
  }
  std::vector<uint8_t *> pointers;
  for (int i = 0; i < slot_count; ++i) {
    bench->slots.emplace_back(FRAME_BYTES, uint8_t(0x5a));
    pointers.push_back(bench->slots.back().data());
  }
  bench->receiver.set_slots(pointers.data(), slot_count);
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
int shows(void *bench) {
  return static_cast<Bench *>(bench)->panel.shows;
}
size_t written(void *bench) {
  return static_cast<Bench *>(bench)->panel.written;
}
}
