#include "presto_receiver.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace esphome::presto_display {

static size_t offset_of(const Rect &rect) {
  return (size_t(rect.y) * PANEL + rect.x) * 2;
}

static int read16(const uint8_t *p) {
  return int(p[0]) << 8 | p[1];
}

// The scroll picture's packet: base 0 with the whole picture (x 0, y 0, PANEL wide, 1 to
// `rows` high), or base == `current_frame` with a rectangle inside its `current_height`.
static bool parse_scroll_header(const uint8_t *p, uint32_t current_frame, int current_height,
                                int rows, PatchHeader &header) {
  header.base = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  header.rect = {read16(p + 4), read16(p + 6), read16(p + 8), read16(p + 10)};
  header.codec = p[12];
  const Rect &r = header.rect;
  if (!r.width || !r.height || r.x + r.width > PANEL || header.codec > CODEC_RLE) {
    return false;
  }
  if (!header.base) {
    return !r.x && !r.y && r.width == PANEL && r.height <= rows;
  }
  return header.base == current_frame && r.y + r.height <= current_height;
}

static int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

size_t decode_base64_chunk(const char *encoded, size_t size, bool final_chunk, uint8_t *out,
                           size_t capacity) {
  if (!size || size % 4) {
    return 0;
  }
  size_t written = 0;
  for (size_t i = 0; i < size; i += 4) {
    uint32_t bits = 0;
    int padding = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = encoded[i + j];
      int value = 0;
      if (c == '=') {
        // Padding only in the last two places of the final chunk.
        if (!final_chunk || i + 4 != size || j < 2) {
          return 0;
        }
        ++padding;
      } else {
        value = base64_value(c);
        if (value < 0 || padding) {
          return 0;
        }
      }
      bits = bits << 6 | uint32_t(value);
    }
    const size_t count = 3 - padding;
    if (written + count > capacity) {
      return 0;
    }
    out[written++] = uint8_t(bits >> 16);
    if (count > 1) {
      out[written++] = uint8_t(bits >> 8);
    }
    if (count > 2) {
      out[written++] = uint8_t(bits);
    }
  }
  return written;
}

void Receiver::TimedWriter::write(uint8_t *to, const uint8_t *from, size_t size) {
  const uint32_t started = panel_->micros();
  panel_->write(to, from, size);
  us += panel_->micros() - started;
}

bool Receiver::begin() {
  // Allocated once: the receiver allocates nothing per frame. The dictionaries must be
  // SRAM; the inflater makes byte-sized back-references into them.
  live_inflater_ = new (std::nothrow) tinfl_decompressor;
  slot_inflater_ = new (std::nothrow) tinfl_decompressor;
  live_dictionary_ = new (std::nothrow) uint8_t[TINFL_LZ_DICT_SIZE];
  slot_dictionary_ = new (std::nothrow) uint8_t[TINFL_LZ_DICT_SIZE];
  if (!live_inflater_ || !slot_inflater_ || !live_dictionary_ || !slot_dictionary_) {
    return false;
  }
  live_decoder_ = new (std::nothrow) StreamDecoder(live_inflater_, live_dictionary_, &live_writer_);
  slot_decoder_ = new (std::nothrow) StreamDecoder(slot_inflater_, slot_dictionary_, &slot_writer_);
  if (!live_decoder_ || !slot_decoder_) {
    return false;
  }
  // JPEG is optional: without its state, codec 2 is refused and not advertised.
  live_jpeg_.allocate();
  live_decoder_->set_jpeg(&live_jpeg_);
  return true;
}

void Receiver::set_slots(uint8_t *const *slots, int count) {
  slot_count_ = std::clamp(count, 0, SLOT_COUNT);
  for (int i = 0; i < SLOT_COUNT; ++i) {
    slots_[i] = i < slot_count_ ? slots[i] : nullptr;
    slot_frames_[i] = 0;
  }
}

void Receiver::set_scroll_picture(uint8_t *picture, int rows) {
  scroll_picture_ = rows > 0 ? picture : nullptr;
  scroll_rows_ = scroll_picture_ ? rows : 0;
  scroll_frame_ = 0;
  scroll_height_ = 0;
}

const char *Receiver::codecs() const {
  snprintf(codecs_, sizeof(codecs_), "%s", has_jpeg() ? "zlib,rle,jpeg" : "zlib,rle");
  if (scroll_rows_) {
    const size_t used = strlen(codecs_);
    snprintf(codecs_ + used, sizeof(codecs_) - used, ",scroll=%d", scroll_rows_);
  }
  return codecs_;
}

uint32_t Receiver::slot_frame(int slot) const {
  return slot >= 0 && slot < slot_count_ ? slot_frames_[slot] : 0;
}

int Receiver::touch_frame() const {
  if (!presented_frame_ || panel_->millis() - presented_at_ > OFFLINE_MS) {
    return 0;
  }
  return presented_frame_;
}

// ---------------------------------------------------------------------------------------
// Framebuffers

int Receiver::slot_index_(const uint8_t *buffer) const {
  for (int i = 0; i < slot_count_; ++i) {
    if (buffer && slots_[i] == buffer) {
      return i;
    }
  }
  return -1;
}

void Receiver::attach_() {
  own_[0] = panel_->framebuffer(0);
  own_[1] = panel_->framebuffer(1);
  front_ = panel_->visible();
  if (front_ == own_[0] || front_ == own_[1]) {
    back_ = front_ == own_[0] ? 1 : 0;
  }
  // Trust nothing about the buffer that is not visible.
  stale_[0] = stale_[1] = FULL_FRAME;
  if (front_ == own_[1 - back_]) {
    stale_[1 - back_] = {};
  }
  is_attached_ = true;
}

void Receiver::check_panel_() {
  // First use, or the display presented a buffer without the receiver.
  if (!is_attached_ || panel_->visible() != front_) {
    attach_();
  }
}

void Receiver::copy_rect_(uint8_t *to, const uint8_t *from, const Rect &rect, PixelWriter *writer) {
  alignas(4) uint8_t row[PANEL * 2];
  const size_t bytes = size_t(rect.width) * 2;
  for (int line = 0; line < rect.height; ++line) {
    const size_t offset = (size_t(rect.y + line) * PANEL + rect.x) * 2;
    panel_->read(row, from + offset, bytes);
    writer->write(to + offset, row, bytes);
  }
}

// Make the back buffer equal to the visible picture everywhere outside `next`.
size_t Receiver::prepare_back_(const Rect &next) {
  Rect pieces[4];
  const int count = subtract(stale_[back_], next, pieces);
  size_t bytes = 0;
  for (int i = 0; i < count; ++i) {
    copy_rect_(own_[back_], front_, pieces[i], &live_writer_);
    bytes += pieces[i].bytes();
  }
  // Outside `next` the back buffer now matches; inside, the patch replaces it.
  stale_[back_] = {};
  return bytes;
}

// The back buffer, which differs from the visible picture only inside `drawn`, is shown.
void Receiver::present_back_(const Rect &drawn) {
  show_(own_[back_]);
  const int other = 1 - back_;
  // The other own buffer was the picture (or a stale copy of a slot); either way it now
  // differs from the new picture by at most its old staleness plus `drawn`.
  stale_[other] = unite(stale_[other], drawn);
  stale_[back_] = {};
  front_ = own_[back_];
  back_ = other;
}

bool Receiver::is_live_writing_() const {
  return receiving_frame_ && is_live_started_ && !is_small_;
}

bool Receiver::needs_sync_() const {
  return is_attached_ && (slot_index_(front_) >= 0 || !stale_[back_].empty());
}

// One piece of idle sync: copy part of the back buffer's stale area from the picture, or,
// once a slot's picture is in the back buffer, move the scanout onto it. Returns true while
// work is left.
bool Receiver::settle_step_() {
  if (!needs_sync_()) {
    return false;
  }
  const uint32_t started = panel_->micros();
  Rect &stale = stale_[back_];
  if (!stale.empty()) {
    const int rows = std::min(stale.height, SETTLE_ROWS);
    const Rect piece{stale.x, stale.y, stale.width, rows};
    copy_rect_(own_[back_], front_, piece, panel_);
    settle_bytes_ += piece.bytes();
    stale.y += rows;
    stale.height -= rows;
  } else {
    // The back buffer holds the slot's picture: scan it out instead, which frees the slot.
    show_(own_[back_]);
    front_ = own_[back_];
    back_ = 1 - back_;
  }
  settle_us_ += panel_->micros() - started;
  if (needs_sync_()) {
    return true;
  }
  is_sync_urgent_ = false;
  char report[64];
  snprintf(report, sizeof(report), "settled,%lu,%lu", static_cast<unsigned long>(settle_bytes_),
           static_cast<unsigned long>(settle_us_));
  panel_->publish_timing(report);
  settle_bytes_ = 0;
  settle_us_ = 0;
  return false;
}

// Move the scanout off a slot now, with the same picture, so the slot can be rewritten.
void Receiver::release_slot_() {
  if (slot_index_(front_) < 0) {
    return;
  }
  // A live frame decoding into the back buffer keeps it; use the other one.
  const int target = is_live_writing_() ? 1 - back_ : back_;
  const uint32_t started = panel_->micros();
  if (!stale_[target].empty()) {
    copy_rect_(own_[target], front_, stale_[target], panel_);
    settle_bytes_ += stale_[target].bytes();
    stale_[target] = {};
  }
  show_(own_[target]);
  front_ = own_[target];
  back_ = 1 - target;
  settle_us_ += panel_->micros() - started;
}

// Every swap: it takes effect with the window requested before it, so once a hidden window
// is swapped past, nothing reads the scroll picture.
void Receiver::show_(uint8_t *buffer) {
  panel_->show(buffer);
  if (!window_.is_shown) {
    is_window_latched_ = false;
  }
}

// ---------------------------------------------------------------------------------------
// Scroll window: `scroll_show`, `scroll_hide`, and the board's own drag

int Receiver::max_offset_() const {
  return window_.is_shown ? std::max(0, scroll_height_ - window_.height()) : 0;
}

void Receiver::queue_window_report_(int frame_id, int status) {
  SlotReport &report = queue_report_();
  if (status != SLOT_OK) {
    snprintf(report.event, sizeof(report.event), "scroll_window,%d,%d,0,0,0", frame_id, status);
  } else if (!window_.is_shown) {
    snprintf(report.event, sizeof(report.event), "scroll_window,0,0,0,0,0");
  } else {
    snprintf(report.event, sizeof(report.event), "scroll_window,%d,0,%d,%d,%d", frame_id,
             window_.top, window_.bottom, window_.offset);
  }
}

// Hide the window from the next frame. Before writing into the picture (`is_writing`), wait
// until the scanout has stopped reading it.
void Receiver::hide_window_(bool is_writing) {
  if (window_.is_shown) {
    window_ = Window{};
    drag_.reset();
    panel_->set_window(window_);
    queue_window_report_(0, SLOT_OK);
  }
  if (is_writing && is_window_latched_) {
    panel_->release_window();
    is_window_latched_ = false;
  }
}

void Receiver::scroll_show(int frame_id, int top, int bottom, int scroll_y) {
  if (upload_.is_active && upload_.slot == SCROLL_TARGET && upload_.is_final) {
    // Its final chunk has arrived; finish decoding it now.
    slot_run_(SIZE_MAX);
  }
  int status = SLOT_OK;
  if (!scroll_picture_ || frame_id <= 0 || uint32_t(frame_id) != scroll_frame_) {
    status = SLOT_UNAVAILABLE;
  } else if (top < 0 || bottom > PANEL || top >= bottom || top % WINDOW_ALIGN ||
             bottom % WINDOW_ALIGN || bottom - top > scroll_height_) {
    status = SLOT_INVALID_PACKET;
  }
  if (status != SLOT_OK) {
    queue_window_report_(frame_id, status);
    return;
  }
  // A contact begun on the window as it was belongs to that window.
  drag_.reset();
  window_.is_shown = true;
  window_.top = top;
  window_.bottom = bottom;
  window_.offset = std::clamp(scroll_y, 0, max_offset_());
  panel_->set_window(window_);
  is_window_latched_ = true;
  queue_window_report_(frame_id, SLOT_OK);
}

void Receiver::scroll_hide() {
  const bool was_shown = window_.is_shown;
  hide_window_(false);
  if (!was_shown) {
    // Answer every hide, so CastKit always hears the window's state.
    queue_window_report_(0, SLOT_OK);
  }
}

Receiver::TouchReport Receiver::touch(int phase, int x, int y) {
  bool is_moved = false;
  const auto verdict = drag_.touch(phase, x, y, panel_->millis(), window_, max_offset_(), is_moved);
  if (is_moved) {
    panel_->set_window(window_);
  }
  TouchReport report;
  report.is_forwarded = verdict != ScrollDrag::Verdict::OWN;
  report.phase = phase;
  if (window_.is_shown) {
    report.flags = TOUCH_WINDOW;
    report.offset = window_.offset;
  }
  if (verdict == ScrollDrag::Verdict::TAKE) {
    report.phase = 2;
    report.flags |= TOUCH_TAKEN;
  }
  return report;
}

// The glide after a fling, and the `scroll_at` report once a local scroll rests. True while
// the glide continues.
bool Receiver::glide_(uint32_t now) {
  bool is_moved = false;
  const bool is_gliding = drag_.glide(now, window_, max_offset_(), is_moved);
  if (is_moved) {
    panel_->set_window(window_);
  }
  if (drag_.take_settled() && window_.is_shown) {
    SlotReport &report = queue_report_();
    snprintf(report.event, sizeof(report.event), "scroll_at,%lu,%d",
             static_cast<unsigned long>(scroll_frame_), window_.offset);
  }
  return is_gliding;
}

// ---------------------------------------------------------------------------------------
// Live frames: `frame_chunk`

void Receiver::abandon_live_() {
  // Whatever was written into the back buffer is no longer the picture.
  if (is_live_writing_()) {
    stale_[back_] = unite(stale_[back_], header_.rect);
  }
  receiving_frame_ = 0;
  encoded_offset_ = header_have_ = 0;
  is_live_started_ = false;
}

void Receiver::error_(const char *reason) {
  abandon_live_();
  char event[64];
  snprintf(event, sizeof(event), "error,%s", reason);
  panel_->publish(event);
}

void Receiver::frame_chunk(int frame_id, int touch_id, int format, int offset, bool final_chunk,
                           const char *encoded, size_t size) {
  if (!live_decoder_ || frame_id <= 0 || touch_id < 0 || format != 4 || offset < 0 || size == 0 ||
      size > 4000 || size % 4) {
    error_("invalid_chunk");
    return;
  }
  if (!offset) {
    abandon_live_();
    receiving_frame_ = frame_id;
    receiving_touch_ = touch_id;
    decode_us_ = sync_us_ = 0;
    sync_bytes_ = 0;
    live_writer_.us = 0;
    first_chunk_at_ = panel_->millis();
  }
  if (frame_id != receiving_frame_ || touch_id != receiving_touch_ ||
      size_t(offset) != encoded_offset_) {
    error_("chunk_order_or_size");
    return;
  }
  last_chunk_at_ = panel_->millis();
  const size_t count = decode_base64_chunk(encoded, size, final_chunk, chunk_, sizeof(chunk_));
  if (!count) {
    error_("invalid_base64");
    return;
  }
  encoded_offset_ += size;
  const uint32_t started = panel_->micros(), sync_before = sync_us_;
  size_t used = 0;
  if (header_have_ < HEADER_BYTES && !start_live_(chunk_, count, used)) {
    if (header_have_ == HEADER_BYTES || final_chunk) {
      error_("invalid_packet_or_base");
    }
    return;
  }
  size_t consumed = 0;
  const auto status =
      live_decoder_->feed(chunk_ + used, count - used, final_chunk, consumed, SIZE_MAX);
  // For a JPEG the final chunk's call includes the whole decode.
  decode_us_ += panel_->micros() - started - (sync_us_ - sync_before);
  if (status == StreamDecoder::Status::FAILED ||
      (final_chunk && status != StreamDecoder::Status::COMPLETE)) {
    const char *reason = live_decoder_->failure();
    error_(reason ? reason : "invalid_packet_or_base");
    return;
  }
  if (final_chunk) {
    finish_live_(frame_id, touch_id);
  }
}

// Collect the 13-byte header, then point the decoder at its destination.
bool Receiver::start_live_(const uint8_t *data, size_t size, size_t &consumed) {
  const size_t take = std::min(size, HEADER_BYTES - header_have_);
  memcpy(header_bytes_ + header_have_, data, take);
  header_have_ += take;
  consumed = take;
  if (header_have_ < HEADER_BYTES || !parse_header(header_bytes_, presented_frame_, header_)) {
    return false;
  }
  check_panel_();
  const Rect &rect = header_.rect;
  is_small_ = rect.bytes() <= SMALL_BYTES;
  if (is_small_) {
    live_decoder_->begin(header_, small_, rect.width);
  } else {
    const uint32_t sync_started = panel_->micros();
    sync_bytes_ = prepare_back_(rect);
    sync_us_ = panel_->micros() - sync_started;
    live_decoder_->begin(header_, own_[back_] + offset_of(rect));
  }
  is_live_started_ = true;
  return true;
}

void Receiver::finish_live_(int frame_id, int touch_id) {
  const Rect &rect = header_.rect;
  const uint32_t draw_started = panel_->micros();
  bool is_changed = true;
  if (is_small_) {
    // An unchanged small patch (a heartbeat) acknowledges without a swap.
    alignas(4) uint8_t row[PANEL * 2];
    const size_t bytes = size_t(rect.width) * 2;
    is_changed = false;
    for (int line = 0; line < rect.height && !is_changed; ++line) {
      panel_->read(row, front_ + offset_of({rect.x, rect.y + line, rect.width, 1}), bytes);
      is_changed = memcmp(row, small_ + size_t(line) * bytes, bytes) != 0;
    }
    if (is_changed) {
      const uint32_t sync_started = panel_->micros();
      sync_bytes_ = prepare_back_(rect);
      sync_us_ = panel_->micros() - sync_started;
      for (int line = 0; line < rect.height; ++line) {
        live_writer_.write(own_[back_] + offset_of({rect.x, rect.y + line, rect.width, 1}),
                           small_ + size_t(line) * bytes, bytes);
      }
    }
  }
  uint32_t present_us = 0;
  if (is_changed) {
    const uint32_t present_started = panel_->micros();
    if (!header_.base) {
      // A full frame is a fresh start: the window goes with the swap.
      hide_window_(false);
    }
    present_back_(rect);
    present_us = panel_->micros() - present_started;
  }
  const uint32_t draw_us = panel_->micros() - draw_started + (is_small_ ? 0 : sync_us_);
  presented_frame_ = frame_id;
  presented_at_ = panel_->millis();
  is_offline_ = is_blank_pending_ = false;
  receiving_frame_ = 0;
  encoded_offset_ = header_have_ = 0;
  is_live_started_ = false;
  char event[120];
  snprintf(event, sizeof(event), "frame,%d,%d,0,%lu,%lu,%lu", frame_id, touch_id,
           static_cast<unsigned long>(panel_->overruns()), static_cast<unsigned long>(decode_us_),
           static_cast<unsigned long>(draw_us));
  panel_->publish(event);
  // timing,<frame>,<rect bytes>,<decode us>,<PSRAM write us>,<sync us>,<sync bytes>,
  //        <present us>,<first chunk to final chunk ms>,<free PSRAM bytes>
  char report[160];
  snprintf(report, sizeof(report), "timing,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", frame_id,
           static_cast<unsigned long>(rect.bytes()), static_cast<unsigned long>(decode_us_),
           static_cast<unsigned long>(live_writer_.us), static_cast<unsigned long>(sync_us_),
           static_cast<unsigned long>(sync_bytes_), static_cast<unsigned long>(present_us),
           static_cast<unsigned long>(panel_->millis() - first_chunk_at_),
           static_cast<unsigned long>(panel_->free_psram()));
  panel_->publish_timing(report);
}

// ---------------------------------------------------------------------------------------
// Slots and the scroll picture: `slot_chunk`, `scroll_chunk`, `show_slot`, `drop_slots`

void Receiver::slot_end_() {
  upload_.is_active = upload_.is_decoding = false;
  upload_.size = upload_.used = 0;
}

void Receiver::slot_finish_() {
  const int slot = upload_.slot;
  const bool is_ok = upload_.status == SLOT_OK;
  if (slot == SCROLL_TARGET) {
    scroll_frame_ = is_ok ? uint32_t(upload_.frame_id) : 0;
    if (is_ok) {
      scroll_height_ = upload_.height;
    }
    SlotReport &report = queue_report_();
    snprintf(report.event, sizeof(report.event), "scroll,%d,%d", upload_.frame_id, upload_.status);
    if (is_ok) {
      // scroll_timing,<frame>,<rect bytes>,<decode us>,<PSRAM write us>,
      //               <first chunk to decoded ms>,<free PSRAM bytes>
      snprintf(report.timing, sizeof(report.timing), "scroll_timing,%d,%lu,%lu,%lu,%lu,%lu",
               upload_.frame_id, static_cast<unsigned long>(slot_decoder_->header().rect.bytes()),
               static_cast<unsigned long>(upload_.decode_us),
               static_cast<unsigned long>(slot_writer_.us),
               static_cast<unsigned long>(panel_->millis() - upload_.first_chunk_at),
               static_cast<unsigned long>(panel_->free_psram()));
    }
    slot_end_();
    return;
  }
  slot_frames_[slot] = is_ok ? uint32_t(upload_.frame_id) : 0;
  SlotReport &report = queue_slot_report_(slot, upload_.frame_id, upload_.status);
  if (is_ok) {
    // slot_timing,<slot>,<frame>,<rect bytes>,<decode us>,<PSRAM write us>,
    //             <first chunk to decoded ms>,<free PSRAM bytes>
    snprintf(report.timing, sizeof(report.timing), "slot_timing,%d,%d,%lu,%lu,%lu,%lu,%lu", slot,
             upload_.frame_id, static_cast<unsigned long>(slot_decoder_->header().rect.bytes()),
             static_cast<unsigned long>(upload_.decode_us),
             static_cast<unsigned long>(slot_writer_.us),
             static_cast<unsigned long>(panel_->millis() - upload_.first_chunk_at),
             static_cast<unsigned long>(panel_->free_psram()));
  }
  slot_end_();
}

// Reports for Display Slot Events wait here and leave one per `step()`. ESPHome keeps only the
// newest state of an entity that has not been sent yet, so two reports in one call could lose
// one.
Receiver::SlotReport &Receiver::queue_report_() {
  if (report_count_ == REPORT_CAPACITY) {
    // Never expected (a few reports per action); keep the newest.
    report_first_ = (report_first_ + 1) % REPORT_CAPACITY;
    --report_count_;
  }
  SlotReport &report = reports_[(report_first_ + report_count_++) % REPORT_CAPACITY];
  report.event[0] = '\0';
  report.timing[0] = '\0';
  return report;
}

Receiver::SlotReport &Receiver::queue_slot_report_(int slot, int frame_id, int status) {
  SlotReport &report = queue_report_();
  snprintf(report.event, sizeof(report.event), "slot,%d,%d,%d", slot, frame_id, status);
  return report;
}

void Receiver::slot_error_(int slot, int frame_id, int status, bool final_chunk) {
  if (upload_.is_active && upload_.slot == slot && upload_.frame_id == frame_id) {
    // Remember the first failure; the final chunk reports it.
    if (upload_.status == SLOT_OK) {
      upload_.status = status;
    }
    upload_.is_decoding = false;
    upload_.size = upload_.used = 0;
    if (final_chunk) {
      slot_finish_();
    }
    return;
  }
  if (slot == SCROLL_TARGET) {
    // Any failure leaves the picture empty, so it is no longer shown either.
    hide_window_(false);
    scroll_frame_ = 0;
    if (final_chunk) {
      SlotReport &report = queue_report_();
      snprintf(report.event, sizeof(report.event), "scroll,%d,%d", frame_id, status);
    }
    return;
  }
  if (slot >= 0 && slot < slot_count_) {
    slot_frames_[slot] = 0;
  }
  if (final_chunk) {
    queue_slot_report_(slot, frame_id, status);
  }
}

// Decode the buffered chunk into the slot or picture, stopping after about `budget` bytes.
// True while some of it is left.
bool Receiver::slot_run_(size_t budget) {
  if (!upload_.is_active || !upload_.is_decoding) {
    return false;
  }
  const uint32_t started = panel_->micros();
  bool is_valid = true;
  if (upload_.header_have < HEADER_BYTES) {
    const size_t take = std::min(upload_.size - upload_.used, HEADER_BYTES - upload_.header_have);
    memcpy(upload_.header_bytes + upload_.header_have, upload_.data + upload_.used, take);
    upload_.header_have += take;
    upload_.used += take;
    if (upload_.header_have == HEADER_BYTES) {
      PatchHeader header;
      if (upload_.slot == SCROLL_TARGET) {
        // zlib and RLE only: a JPEG decodes in one piece, not in bounded idle steps.
        is_valid = parse_scroll_header(upload_.header_bytes, upload_.base_frame, scroll_height_,
                                       scroll_rows_, header);
        if (is_valid) {
          upload_.height = header.base ? scroll_height_ : header.rect.height;
          slot_decoder_->begin(header, scroll_picture_ + offset_of(header.rect));
        }
      } else {
        // Slots take zlib and RLE; a JPEG decodes in one piece, not in bounded idle steps.
        is_valid = parse_header(upload_.header_bytes, upload_.base_frame, header) &&
                   header.codec != CODEC_JPEG;
        if (is_valid) {
          slot_decoder_->begin(header, slots_[upload_.slot] + offset_of(header.rect));
        }
      }
    } else {
      is_valid = !upload_.is_final;
    }
  }
  auto status = StreamDecoder::Status::NEED_INPUT;
  if (is_valid && upload_.header_have == HEADER_BYTES) {
    size_t consumed = 0;
    status = slot_decoder_->feed(upload_.data + upload_.used, upload_.size - upload_.used,
                                 upload_.is_final, consumed, budget);
    upload_.used += consumed;
  }
  upload_.decode_us += panel_->micros() - started;
  if (!is_valid || status == StreamDecoder::Status::FAILED) {
    slot_error_(upload_.slot, upload_.frame_id, SLOT_INVALID_PACKET, upload_.is_final);
    return false;
  }
  if (status == StreamDecoder::Status::MORE) {
    return true;
  }
  upload_.is_decoding = false;
  upload_.size = upload_.used = 0;
  if (upload_.is_final) {
    slot_finish_();
  }
  return false;
}

void Receiver::slot_chunk(int slot, int frame_id, int format, int offset, bool final_chunk,
                          const char *encoded, size_t size) {
  // The previous chunk's bytes share the one upload buffer: finish them first.
  slot_run_(SIZE_MAX);
  if (slot < 0 || slot >= slot_count_) {
    // No such slot (and never the scroll picture's target).
    if (final_chunk) {
      queue_slot_report_(slot, frame_id, SLOT_UNAVAILABLE);
    }
    return;
  }
  if (!slot_decoder_) {
    slot_error_(slot, frame_id, SLOT_UNAVAILABLE, final_chunk);
    return;
  }
  upload_chunk_(slot, frame_id, format, offset, final_chunk, encoded, size);
}

void Receiver::scroll_chunk(int frame_id, int format, int offset, bool final_chunk,
                            const char *encoded, size_t size) {
  slot_run_(SIZE_MAX);
  if (!scroll_picture_ || !slot_decoder_) {
    slot_error_(SCROLL_TARGET, frame_id, SLOT_UNAVAILABLE, final_chunk);
    return;
  }
  upload_chunk_(SCROLL_TARGET, frame_id, format, offset, final_chunk, encoded, size);
}

// One chunk of an upload into a slot or (`target` SCROLL_TARGET) the scroll picture.
void Receiver::upload_chunk_(int target, int frame_id, int format, int offset, bool final_chunk,
                             const char *encoded, size_t size) {
  if (frame_id <= 0 || format != 4 || offset < 0 || size == 0 || size > 4000 || size % 4) {
    slot_error_(target, frame_id, SLOT_INVALID_CHUNK, final_chunk);
    return;
  }
  if (!offset) {
    // One upload at a time: a new one abandons an unfinished one, whose target stays empty.
    slot_end_();
    if (target == SCROLL_TARGET) {
      // The picture is rewritten in place: nothing may scan it out meanwhile.
      hide_window_(true);
      upload_.base_frame = scroll_frame_;
      upload_.height = scroll_height_;
      scroll_frame_ = 0;
    } else {
      check_panel_();
      if (front_ == slots_[target]) {
        release_slot_();
      }
      upload_.base_frame = slot_frames_[target];
      // The slot is rewritten in place, so it is empty until the upload completes.
      slot_frames_[target] = 0;
    }
    upload_.is_active = true;
    upload_.is_final = false;
    upload_.slot = target;
    upload_.frame_id = frame_id;
    upload_.status = SLOT_OK;
    upload_.encoded_offset = upload_.header_have = 0;
    upload_.decode_us = 0;
    upload_.first_chunk_at = panel_->millis();
    slot_writer_.us = 0;
  }
  if (!upload_.is_active || upload_.slot != target || upload_.frame_id != frame_id ||
      size_t(offset) != upload_.encoded_offset) {
    slot_error_(target, frame_id, SLOT_CHUNK_ORDER, final_chunk);
    return;
  }
  upload_.encoded_offset += size;
  upload_.is_final = final_chunk;
  if (upload_.status != SLOT_OK) {
    // Already failed: wait for the final chunk to report it.
    if (final_chunk) {
      slot_finish_();
    }
    return;
  }
  const size_t count =
      decode_base64_chunk(encoded, size, final_chunk, upload_.data, sizeof(upload_.data));
  if (!count) {
    slot_error_(target, frame_id, SLOT_INVALID_CHUNK, final_chunk);
    return;
  }
  upload_.size = count;
  upload_.used = 0;
  upload_.is_decoding = true;
  // Decode a bounded part now; `step()` finishes it between live frames.
  slot_run_(SLOT_BUDGET);
}

void Receiver::show_slot(int slot, int touch_id) {
  if (slot >= 0 && slot < slot_count_ && upload_.is_active && upload_.slot == slot &&
      upload_.is_final) {
    // Its final chunk has arrived; finish decoding it now.
    slot_run_(SIZE_MAX);
  }
  if (slot < 0 || slot >= slot_count_ || !slot_frames_[slot] || touch_id < 0) {
    panel_->publish("error,slot_unavailable");
    return;
  }
  // A live patch in flight was made against the picture this replaces.
  abandon_live_();
  check_panel_();
  // A slot is a whole other view: the window goes with the swap.
  hide_window_(false);
  const uint32_t started = panel_->micros();
  if (front_ != slots_[slot]) {
    show_(slots_[slot]);
    front_ = slots_[slot];
    // Neither own buffer is known to hold this picture.
    stale_[0] = stale_[1] = FULL_FRAME;
    settle_bytes_ = 0;
    settle_us_ = 0;
    is_sync_urgent_ = true;
  }
  const uint32_t draw_us = panel_->micros() - started;
  presented_frame_ = int(slot_frames_[slot]);
  presented_at_ = panel_->millis();
  is_offline_ = is_blank_pending_ = false;
  char event[120];
  snprintf(event, sizeof(event), "frame,%d,%d,0,%lu,0,%lu", presented_frame_, touch_id,
           static_cast<unsigned long>(panel_->overruns()), static_cast<unsigned long>(draw_us));
  panel_->publish(event);
}

void Receiver::drop_slots() {
  // A visible slot keeps its memory on screen; idle settling, or the next upload into it,
  // moves the scanout off it before anything rewrites it.
  if (upload_.is_active && upload_.is_final) {
    // Its final chunk is in, so it is owed an event.
    upload_.status = SLOT_DROPPED;
    slot_finish_();
  }
  slot_end_();
  for (int i = 0; i < SLOT_COUNT; ++i) {
    slot_frames_[i] = 0;
  }
  hide_window_(false);
  scroll_frame_ = 0;
}

// ---------------------------------------------------------------------------------------
// Idle work

void Receiver::blank_() {
  abandon_live_();
  // The offline marker replaces the whole picture, window included.
  hide_window_(false);
  panel_->blank();
  is_window_latched_ = false;
  attach_();
  // The blank was presented with a full copy: both own buffers hold it.
  stale_[0] = stale_[1] = {};
  is_blank_pending_ = is_sync_urgent_ = false;
}

bool Receiver::step() {
  if (report_count_) {
    // At most one slot report per call, and nothing else alongside it.
    const SlotReport &report = reports_[report_first_];
    panel_->publish_slot(report.event);
    if (report.timing[0]) {
      panel_->publish_timing(report.timing);
    }
    report_first_ = (report_first_ + 1) % REPORT_CAPACITY;
    --report_count_;
    return true;
  }
  const uint32_t now = panel_->millis();
  // A glide only moves the scanout's source; it costs no PSRAM time and runs every step.
  const bool is_gliding = glide_(now);
  if (receiving_frame_ && now - last_chunk_at_ > STALL_MS) {
    // A sender that stopped mid-frame must not hold back idle work or the offline blank.
    abandon_live_();
  }
  if (presented_frame_ && !is_offline_ && now - presented_at_ > OFFLINE_MS) {
    // Offline: touches stop and only a full frame is accepted, as before.
    presented_frame_ = 0;
    is_offline_ = true;
    if (is_live_writing_() && header_.base == 0) {
      // A full frame on its way keeps going, and the blank waits for it. The blank would
      // have to draw into the buffer it is decoding into.
      is_blank_pending_ = true;
    } else {
      blank_();
    }
  }
  if (is_blank_pending_ && !is_live_writing_()) {
    // The full frame failed or stalled before it could be shown.
    blank_();
  }
  if (receiving_frame_) {
    // A live frame is between chunks; it has the PSRAM to itself.
    return is_gliding || report_count_;
  }
  // After a slot is shown, synchronize at once; after a live frame, wait for a pause.
  const bool has_settle = needs_sync_() && (is_sync_urgent_ || slot_index_(front_) >= 0 ||
                                            now - last_chunk_at_ >= SYNC_DELAY_MS);
  const bool has_slot = upload_.is_active && upload_.is_decoding;
  if (!has_settle && !has_slot) {
    return is_gliding || report_count_;
  }
  // Alternate when both are waiting, so neither starves the other.
  is_slot_turn_ = has_slot && (!has_settle || !is_slot_turn_);
  if (is_slot_turn_) {
    slot_run_(SLOT_BUDGET);
  } else {
    check_panel_();
    settle_step_();
  }
  return is_gliding || report_count_ || has_settle || (upload_.is_active && upload_.is_decoding);
}

} // namespace esphome::presto_display
