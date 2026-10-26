#include "presto_stream.h"
#include <algorithm>
#include <cstring>
#include "presto_jpeg.h"

namespace esphome::presto_display {

Rect unite(const Rect &a, const Rect &b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  const int left = std::min(a.x, b.x), top = std::min(a.y, b.y);
  const int right = std::max(a.x + a.width, b.x + b.width);
  const int bottom = std::max(a.y + a.height, b.y + b.height);
  return {left, top, right - left, bottom - top};
}

int subtract(const Rect &a, const Rect &b, Rect out[4]) {
  if (a.empty()) {
    return 0;
  }
  const int left = std::max(a.x, b.x), top = std::max(a.y, b.y);
  const int right = std::min(a.x + a.width, b.x + b.width);
  const int bottom = std::min(a.y + a.height, b.y + b.height);
  if (b.empty() || left >= right || top >= bottom) {
    out[0] = a;
    return 1;
  }
  int count = 0;
  if (top > a.y) {
    out[count++] = {a.x, a.y, a.width, top - a.y};
  }
  if (bottom < a.y + a.height) {
    out[count++] = {a.x, bottom, a.width, a.y + a.height - bottom};
  }
  if (left > a.x) {
    out[count++] = {a.x, top, left - a.x, bottom - top};
  }
  if (right < a.x + a.width) {
    out[count++] = {right, top, a.x + a.width - right, bottom - top};
  }
  return count;
}

static uint16_t read16(const uint8_t *p) {
  return uint16_t(p[0]) << 8 | p[1];
}

bool parse_header(const uint8_t *p, uint32_t current_frame, PatchHeader &header) {
  header.base = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  header.rect = {read16(p + 4), read16(p + 6), read16(p + 8), read16(p + 10)};
  header.codec = p[12];
  const Rect &r = header.rect;
  return r.width && r.height && r.x + r.width <= PANEL && r.y + r.height <= PANEL &&
         header.codec <= CODEC_JPEG && (!header.base || header.base == current_frame) &&
         (header.base || (!r.x && !r.y && r.width == PANEL && r.height == PANEL));
}

void StreamDecoder::begin(const PatchHeader &header, uint8_t *target, int stride) {
  header_ = header;
  target_ = target;
  stride_ = stride;
  expected_ = header.rect.bytes();
  produced_ = position_ = pending_at_ = pending_ = 0;
  is_failed_ = is_complete_ = false;
  failure_ = nullptr;
  inflate_status_ = TINFL_STATUS_HAS_MORE_OUTPUT;
  control_have_ = value_have_ = 0;
  literal_left_ = repeat_left_ = repeat_count_ = 0;
  is_repeat_pending_ = false;
  if (header.codec == CODEC_ZLIB) {
    tinfl_init(inflater_);
  } else if (header.codec == CODEC_JPEG && jpeg_) {
    jpeg_->begin();
  }
}

bool StreamDecoder::has_jpeg() const {
  return jpeg_ && jpeg_->is_ready();
}

// Write output bytes at the rectangle's stride. A full-width rectangle is contiguous.
void StreamDecoder::emit_(const uint8_t *data, size_t size) {
  const size_t row_bytes = size_t(header_.rect.width) * 2;
  const bool is_contiguous = header_.rect.width == stride_;
  while (size) {
    const size_t row = produced_ / row_bytes, column = produced_ % row_bytes;
    uint8_t *to = target_ + row * size_t(stride_) * 2 + column;
    const size_t count = is_contiguous ? size : std::min(size, row_bytes - column);
    writer_->write(to, data, count);
    data += count;
    size -= count;
    produced_ += count;
  }
}

// Write inflated bytes still held in the dictionary. True once none are left.
bool StreamDecoder::flush_(size_t &budget) {
  while (pending_) {
    if (!budget) {
      return false;
    }
    const size_t count = std::min(pending_, size_t(4096));
    emit_(dictionary_ + pending_at_, count);
    pending_at_ += count;
    pending_ -= count;
    budget -= std::min(budget, count);
  }
  return true;
}

StreamDecoder::Status StreamDecoder::feed(const uint8_t *data, size_t size, bool is_last,
                                          size_t &consumed, size_t budget) {
  consumed = 0;
  if (is_failed_) {
    return Status::FAILED;
  }
  if (is_complete_) {
    return size ? Status::FAILED : Status::COMPLETE;
  }
  Status status;
  if (header_.codec == CODEC_ZLIB) {
    status = feed_zlib_(data, size, is_last, consumed, budget);
  } else if (header_.codec == CODEC_RLE) {
    status = feed_rle_(data, size, is_last, consumed, budget);
  } else {
    status = feed_jpeg_(data, size, is_last, consumed);
  }
  is_failed_ = status == Status::FAILED;
  is_complete_ = status == Status::COMPLETE;
  return status;
}

StreamDecoder::Status StreamDecoder::feed_zlib_(const uint8_t *data, size_t size, bool is_last,
                                                size_t &consumed, size_t &budget) {
  for (;;) {
    if (!flush_(budget)) {
      return Status::MORE;
    }
    if (inflate_status_ == TINFL_STATUS_DONE) {
      // Trailing bytes, a short image, or a stream that ended before the final chunk.
      return produced_ == expected_ && consumed == size && is_last ? Status::COMPLETE
                                                                   : Status::FAILED;
    }
    if (inflate_status_ == TINFL_STATUS_NEEDS_MORE_INPUT && consumed == size) {
      inflate_status_ = TINFL_STATUS_HAS_MORE_OUTPUT;
      return is_last ? Status::FAILED : Status::NEED_INPUT;
    }
    if (!budget) {
      return Status::MORE;
    }
    size_t in_count = size - consumed;
    size_t out_count = TINFL_LZ_DICT_SIZE - position_;
    const uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (is_last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
    const auto status = tinfl_decompress(inflater_, data + consumed, &in_count, dictionary_,
                                         dictionary_ + position_, &out_count, flags);
    consumed += in_count;
    if (out_count > expected_ - produced_) {
      return Status::FAILED;
    }
    pending_at_ = position_;
    pending_ = out_count;
    position_ = (position_ + out_count) & (TINFL_LZ_DICT_SIZE - 1);
    if (status < 0 || (status == TINFL_STATUS_HAS_MORE_OUTPUT && !in_count && !out_count)) {
      return Status::FAILED;
    }
    inflate_status_ = status;
  }
}

StreamDecoder::Status StreamDecoder::feed_rle_(const uint8_t *data, size_t size, bool is_last,
                                               size_t &consumed, size_t &budget) {
  for (;;) {
    if (repeat_left_) {
      if (!budget) {
        return Status::MORE;
      }
      const size_t count = std::min(repeat_left_, sizeof(repeated_));
      emit_(repeated_, count);
      repeat_left_ -= count;
      budget -= std::min(budget, count);
      continue;
    }
    if (consumed == size) {
      break;
    }
    if (literal_left_) {
      if (!budget) {
        return Status::MORE;
      }
      const size_t count = std::min(literal_left_, size - consumed);
      emit_(data + consumed, count);
      consumed += count;
      literal_left_ -= count;
      budget -= std::min(budget, count);
      continue;
    }
    if (is_repeat_pending_) {
      while (value_have_ < 2 && consumed < size) {
        value_bytes_[value_have_++] = data[consumed++];
      }
      if (value_have_ < 2) {
        continue;
      }
      for (size_t i = 0; i < sizeof(repeated_); i += 2) {
        repeated_[i] = value_bytes_[0];
        repeated_[i + 1] = value_bytes_[1];
      }
      repeat_left_ = repeat_count_;
      is_repeat_pending_ = false;
      value_have_ = 0;
      continue;
    }
    while (control_have_ < 2 && consumed < size) {
      control_bytes_[control_have_++] = data[consumed++];
    }
    if (control_have_ < 2) {
      continue;
    }
    control_have_ = 0;
    const uint16_t control = uint16_t(control_bytes_[0]) | uint16_t(control_bytes_[1]) << 8;
    const size_t count = ((control & 0x7fff) + 1) * 2;
    if (count > expected_ - produced_) {
      return Status::FAILED;
    }
    if (control & 0x8000) {
      is_repeat_pending_ = true;
      repeat_count_ = count;
    } else {
      literal_left_ = count;
    }
  }
  if (!is_last) {
    return Status::NEED_INPUT;
  }
  return produced_ == expected_ && !literal_left_ && !is_repeat_pending_ && !control_have_
             ? Status::COMPLETE
             : Status::FAILED;
}

// Collect the body; decode it whole once the last bytes are in. The output budget does not
// apply: the decode runs in one piece, which is why only live frames take JPEG.
StreamDecoder::Status StreamDecoder::feed_jpeg_(const uint8_t *data, size_t size, bool is_last,
                                                size_t &consumed) {
  if (!has_jpeg()) {
    failure_ = "jpeg_unavailable";
    return Status::FAILED;
  }
  if (!jpeg_->append(data, size, writer_)) {
    failure_ = jpeg_reason(JpegResult::TOO_LARGE);
    return Status::FAILED;
  }
  consumed = size;
  if (!is_last) {
    return Status::NEED_INPUT;
  }
  const JpegResult result = jpeg_->decode(header_.rect, target_, stride_, writer_);
  if (result != JpegResult::OK) {
    failure_ = jpeg_reason(result);
    return Status::FAILED;
  }
  produced_ = expected_;
  return Status::COMPLETE;
}

} // namespace esphome::presto_display
