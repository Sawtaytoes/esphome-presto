#pragma once
// CastKit frame receiver: live patches, on-board frame slots, and the bookkeeping that keeps
// the two framebuffers in step. Pure C++ behind the `Panel` interface, so the host tests
// drive the exact code that ships; `PrestoRemote` adapts it to ESPHome and the display.
//
// Buffers. The display owns two framebuffers; the receiver may also show a frame slot.
// `front_` is what the scanout reads. Live patches always decode into `own_[back_]`, never
// into the visible buffer and never into a slot. `stale_[i]` bounds where `own_[i]` may
// differ from the visible picture. Idle time copies the stale area of the back buffer from
// the front in small paced steps (idle sync), starting SYNC_DELAY_MS after the last live
// chunk so a continuous stream is not slowed; a patch that arrives first copies only
// `stale - patch` (lazy sync). Showing a slot is a pointer swap at vertical blank; both own
// buffers are then stale everywhere, and idle time copies the slot into them at once, swaps
// the panel back onto an own buffer with the same picture (so the slot is free to be
// rewritten), and stops once both own buffers match the picture again.
#include <cstddef>
#include <cstdint>
#include "miniz_tinfl.h"
#include "presto_stream.h"

namespace esphome::presto_display {

constexpr int SLOT_COUNT = 6;

// The last field of `slot,<slot>,<frame_id>,<status>`. Any nonzero status leaves the slot
// empty.
enum SlotStatus : int {
  SLOT_OK = 0,
  SLOT_INVALID_CHUNK = 1, // bad parameters or base64
  SLOT_CHUNK_ORDER = 2, // no upload in progress for this slot and frame, or a gap
  SLOT_INVALID_PACKET = 3, // bad header, wrong base frame, or a malformed body
  SLOT_UNAVAILABLE = 4, // slot number out of range, or no PSRAM for slots
  SLOT_DROPPED = 5, // drop_slots arrived while the final chunk was still decoding
};

class Panel : public PixelWriter {
 public:
  // PSRAM -> SRAM, behind the scanout prefetch gate.
  virtual void read(uint8_t *to, const uint8_t *from, size_t size) = 0;
  // The display's two framebuffers, and the buffer the scanout reads now.
  virtual uint8_t *framebuffer(int index) = 0;
  virtual uint8_t *visible() = 0;
  // Scan `buffer` out from the next vertical blank; returns once it is visible. Showing one
  // of the two framebuffers makes the display draw into the other.
  virtual void show(uint8_t *buffer) = 0;
  // Draw the offline marker and present it so both framebuffers hold it.
  virtual void blank() = 0;
  // Display Events: frame acknowledgements, errors, touches, wake.
  virtual void publish(const char *event) = 0;
  // Display Slot Events: `slot,...` only, so they never replace a queued acknowledgement.
  virtual void publish_slot(const char *event) = 0;
  // Display Timing (optional diagnostics).
  virtual void publish_timing(const char *report) = 0;
  virtual uint32_t micros() = 0;
  virtual uint32_t millis() = 0;
  virtual uint32_t overruns() = 0;
  virtual size_t free_psram() = 0;

 protected:
  ~Panel() = default;
};

class Receiver {
 public:
  explicit Receiver(Panel *panel) : panel_(panel), live_writer_(panel), slot_writer_(panel) {}
  // Allocates the two inflate states and dictionaries (SRAM). False if that failed.
  bool begin();
  // Frame slots, FRAME_BYTES each (PSRAM on the board). `count` may be 0.
  void set_slots(uint8_t *const *slots, int count);

  // API actions. `encoded` is base64, at most 4,000 characters per chunk.
  void frame_chunk(int frame_id, int touch_id, int format, int offset, bool final_chunk,
                   const char *encoded, size_t size);
  void slot_chunk(int slot, int frame_id, int format, int offset, bool final_chunk,
                  const char *encoded, size_t size);
  void show_slot(int slot, int touch_id);
  void drop_slots();

  // One bounded piece of idle work (offline blank, slot decode, framebuffer sync). Returns
  // true while more is queued, so the caller can keep the loop running fast.
  bool step();

  // The frame a touch reports, or 0 when the picture is stale or offline.
  int touch_frame() const;
  int presented_frame() const { return presented_frame_; }
  // Test and diagnostic views.
  uint32_t slot_frame(int slot) const;
  // True while idle sync has work left: a stale back buffer, or a slot on screen.
  bool is_settling() const { return needs_sync_(); }
  bool is_slot_visible() const { return slot_index_(front_) >= 0; }
  // Where framebuffer `index` may differ from the picture; empty for the visible one.
  Rect stale(int index) const { return stale_[index & 1]; }
  // The rectangle a live frame is decoding into the back buffer now, or empty.
  Rect live_rect() const { return is_live_writing_() ? header_.rect : Rect{}; }
  int back_index() const { return back_; }
  bool is_attached() const { return is_attached_; }

  static constexpr uint32_t OFFLINE_MS = 7000;
  // Idle sync after a live frame waits this long after the last live chunk.
  static constexpr uint32_t SYNC_DELAY_MS = 150;
  // A live frame with no new chunk for this long is abandoned.
  static constexpr uint32_t STALL_MS = 3000;
  // Idle work per `step()`: framebuffer rows synchronized, or slot bytes decoded.
  static constexpr int SETTLE_ROWS = 16;
  static constexpr size_t SLOT_BUDGET = 16384;

 protected:
  class TimedWriter : public PixelWriter {
   public:
    explicit TimedWriter(Panel *panel) : panel_(panel) {}
    void write(uint8_t *to, const uint8_t *from, size_t size) override;
    uint32_t us{0};

   protected:
    Panel *panel_;
  };

  // Live frames.
  void error_(const char *reason);
  void abandon_live_();
  bool start_live_(const uint8_t *data, size_t size, size_t &consumed);
  void finish_live_(int frame_id, int touch_id);
  bool is_live_writing_() const;

  // Framebuffers.
  void attach_();
  void check_panel_();
  size_t prepare_back_(const Rect &next);
  void present_back_(const Rect &drawn);
  void copy_rect_(uint8_t *to, const uint8_t *from, const Rect &rect, PixelWriter *writer);
  int slot_index_(const uint8_t *buffer) const;
  bool needs_sync_() const;
  bool settle_step_();
  void release_slot_();

  // Slots.
  struct SlotReport {
    char event[40];
    char timing[112];
  };
  SlotReport &queue_slot_report_(int slot, int frame_id, int status);
  void blank_();
  void slot_error_(int slot, int frame_id, int status, bool final_chunk);
  void slot_finish_();
  bool slot_run_(size_t budget);
  void slot_end_();

  Panel *panel_;
  TimedWriter live_writer_, slot_writer_;
  tinfl_decompressor *live_inflater_{nullptr}, *slot_inflater_{nullptr};
  uint8_t *live_dictionary_{nullptr}, *slot_dictionary_{nullptr};
  StreamDecoder *live_decoder_{nullptr}, *slot_decoder_{nullptr};

  uint8_t *own_[2]{nullptr, nullptr};
  Rect stale_[2];
  uint8_t *front_{nullptr};
  int back_{0};
  bool is_attached_{false}, is_slot_turn_{false};
  // Set by show_slot: sync without waiting until both framebuffers hold the picture.
  bool is_sync_urgent_{false};
  size_t settle_bytes_{0};
  uint32_t settle_us_{0};

  // Live frame in progress.
  uint8_t chunk_[3000];
  uint8_t header_bytes_[HEADER_BYTES];
  size_t header_have_{0}, encoded_offset_{0};
  PatchHeader header_;
  // Small patches (heartbeats, a blinking colon) decode to SRAM first so an unchanged one
  // costs no swap.
  static constexpr size_t SMALL_BYTES = 4096;
  uint8_t small_[SMALL_BYTES];
  bool is_small_{false}, is_live_started_{false};
  int receiving_frame_{0}, receiving_touch_{0};
  int presented_frame_{0};
  uint32_t presented_at_{0};
  bool is_offline_{false}, is_blank_pending_{false};
  uint32_t last_chunk_at_{0};
  uint32_t decode_us_{0}, sync_us_{0}, first_chunk_at_{0};
  size_t sync_bytes_{0};

  // Slots.
  uint8_t *slots_[SLOT_COUNT]{};
  uint32_t slot_frames_[SLOT_COUNT]{};
  int slot_count_{0};
  struct SlotUpload {
    bool is_active{false}, is_final{false}, is_decoding{false};
    int slot{0}, frame_id{0}, status{SLOT_OK};
    uint32_t base_frame{0};
    size_t encoded_offset{0}, header_have{0};
    uint8_t header_bytes[HEADER_BYTES];
    // Base64-decoded bytes of the newest chunk not yet decoded into the slot.
    uint8_t data[3000];
    size_t size{0}, used{0};
    uint32_t decode_us{0}, first_chunk_at{0};
  } upload_;
  static constexpr int REPORT_CAPACITY = 2 * SLOT_COUNT;
  SlotReport reports_[REPORT_CAPACITY];
  int report_first_{0}, report_count_{0};
};

// Strict base64 for one chunk: groups of four, '=' padding only at the end of the final
// chunk. Returns the decoded size, or 0 when the input is not valid.
size_t decode_base64_chunk(const char *encoded, size_t size, bool final_chunk, uint8_t *out,
                           size_t capacity);

} // namespace esphome::presto_display
