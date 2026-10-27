#include "presto_jpeg.h"
#include <algorithm>
#include <cstring>
#include <new>
#include "JPEGDEC.h"

namespace esphome::presto_display {

const char *jpeg_reason(JpegResult result) {
  switch (result) {
  case JpegResult::OK:
    return "ok";
  case JpegResult::TOO_LARGE:
    return "jpeg_too_large";
  case JpegResult::SIZE_MISMATCH:
    return "jpeg_size_mismatch";
  case JpegResult::UNSUPPORTED:
    return "jpeg_unsupported";
  case JpegResult::INVALID:
  default:
    return "jpeg_invalid";
  }
}

static int32_t read_callback(JPEGFILE *file, uint8_t *to, int32_t length) {
  const int32_t count = static_cast<JpegDecoder *>(file->fHandle)->read_at_(file->iPos, to, length);
  file->iPos += count;
  return count;
}

static int32_t seek_callback(JPEGFILE *file, int32_t position) {
  file->iPos = std::clamp(position, int32_t(0), std::max(file->iSize - 1, int32_t(0)));
  return file->iPos;
}

static int draw_callback(JPEGDRAW *draw) {
  return static_cast<JpegDecoder *>(draw->pUser)
      ->draw_(draw->x, draw->y, draw->iWidth, draw->iWidthUsed, draw->iHeight, draw->pPixels);
}

bool JpegDecoder::allocate() {
  if (!codec_) {
    codec_ = new (std::nothrow) JPEGDEC;
  }
  if (!band_) {
    band_ = new (std::nothrow) uint16_t[size_t(PANEL) * BAND_ROWS];
  }
  return codec_ != nullptr;
}

bool JpegDecoder::append(const uint8_t *data, size_t size, PixelWriter *writer) {
  if (!is_ready() || size > capacity_ - size_) {
    return false;
  }
  if (size) {
    writer->write(buffer_ + size_, data, size);
    size_ += size;
  }
  return true;
}

bool JpegDecoder::read_(size_t at, uint8_t *to, size_t length) {
  if (at > size_ || length > size_ - at) {
    return false;
  }
  reader_->read(to, buffer_ + at, length);
  return true;
}

int32_t JpegDecoder::read_at_(int32_t position, uint8_t *to, int32_t length) {
  if (position < 0 || length <= 0 || size_t(position) >= size_) {
    return 0;
  }
  const size_t count = std::min(size_t(length), size_ - size_t(position));
  reader_->read(to, buffer_ + position, count);
  return int32_t(count);
}

// Walk the markers up to the scan, then the entropy-coded data up to EOI. Accepts exactly one
// baseline scan of 8-bit, three-component, interleaved YCbCr with luma sampled 1x1, 2x1, or
// 2x2 and chroma 1x1, with its tables, and nothing after EOI.
JpegResult JpegDecoder::inspect(JpegInfo &info) {
  info = {};
  uint8_t bytes[16];
  if (size_ < 4 || !read_(0, bytes, 2) || bytes[0] != 0xFF || bytes[1] != 0xD8 ||
      !read_(size_ - 2, bytes, 2) || bytes[0] != 0xFF || bytes[1] != 0xD9) {
    return JpegResult::INVALID;
  }
  bool has_frame = false, has_huffman = false, has_quantization = false;
  uint8_t ids[3]{};
  size_t at = 2;
  for (;;) {
    // A marker is 0xFF (any number of fill bytes) and a code.
    uint8_t marker = 0;
    if (!read_(at, &marker, 1) || marker != 0xFF) {
      return JpegResult::INVALID;
    }
    while (marker == 0xFF) {
      if (!read_(++at, &marker, 1)) {
        return JpegResult::INVALID;
      }
    }
    ++at;
    if (!read_(at, bytes, 2)) {
      return JpegResult::INVALID;
    }
    const size_t length = size_t(bytes[0]) << 8 | bytes[1];
    if (length < 2 || at + length > size_) {
      return JpegResult::INVALID;
    }
    const size_t body = at + 2, body_size = length - 2;
    at += length;
    if (marker == 0xC0) {
      // Baseline frame: precision, height, width, component count, then three per component.
      if (has_frame || body_size < 6 || !read_(body, bytes, 6)) {
        return JpegResult::INVALID;
      }
      if (bytes[0] != 8 || bytes[5] != 3) {
        return JpegResult::UNSUPPORTED;
      }
      info.height = int(bytes[1]) << 8 | bytes[2];
      info.width = int(bytes[3]) << 8 | bytes[4];
      uint8_t components[9];
      if (body_size != 6 + sizeof(components) || !read_(body + 6, components, sizeof(components))) {
        return JpegResult::INVALID;
      }
      for (int i = 0; i < 3; ++i) {
        const uint8_t sampling = components[i * 3 + 1];
        const bool is_supported =
            i ? sampling == 0x11 : sampling == 0x11 || sampling == 0x21 || sampling == 0x22;
        if (!is_supported) {
          return JpegResult::UNSUPPORTED;
        }
        if (components[i * 3 + 2] > 3) {
          return JpegResult::INVALID;
        }
        ids[i] = components[i * 3];
      }
      info.sampling = components[1];
      has_frame = true;
    } else if (marker == 0xC4) {
      has_huffman = body_size > 0;
    } else if (marker == 0xDB) {
      has_quantization = body_size > 0;
    } else if (marker == 0xDD) {
      if (body_size != 2) {
        return JpegResult::INVALID;
      }
    } else if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
      // Application data and comments are skipped.
    } else if (marker == 0xDA) {
      if (!has_frame || !has_huffman || !has_quantization || !body_size || !read_(body, bytes, 1)) {
        return JpegResult::INVALID;
      }
      // One interleaved scan of all three components. Fewer is a multi-scan image.
      if (bytes[0] != 3) {
        return JpegResult::UNSUPPORTED;
      }
      if (body_size != 10 || !read_(body + 1, bytes, 9)) {
        return JpegResult::INVALID;
      }
      for (int i = 0; i < 3; ++i) {
        const uint8_t tables = bytes[i * 2 + 1];
        if (bytes[i * 2] != ids[i] || (tables >> 4) > 1 || (tables & 0x0F) > 1) {
          return JpegResult::INVALID;
        }
      }
      // Spectral selection 0..63 and no successive approximation: not progressive.
      if (bytes[6] != 0 || bytes[7] != 63 || bytes[8] != 0) {
        return JpegResult::UNSUPPORTED;
      }
      break;
    } else if ((marker >= 0xC1 && marker <= 0xCF) || (marker >= 0xDC && marker <= 0xDF)) {
      // Extended, progressive, lossless, hierarchical, or arithmetic coding, and DNL.
      return JpegResult::UNSUPPORTED;
    } else {
      // SOI, EOI, RSTn, or a reserved code where a segment belongs.
      return JpegResult::INVALID;
    }
  }
  // Entropy-coded data: only stuffed zeros and restart markers until the final EOI.
  bool is_marker = false;
  uint8_t block[256];
  while (at < size_) {
    const size_t count = std::min(sizeof(block), size_ - at);
    read_(at, block, count);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t byte = block[i];
      if (!is_marker) {
        is_marker = byte == 0xFF;
      } else if (byte == 0xD9) {
        return at + i + 1 == size_ ? JpegResult::OK : JpegResult::INVALID;
      } else if (byte == 0xDA || byte == 0xC4 || byte == 0xDB || byte == 0xDD) {
        // A second scan, or tables between scans.
        return JpegResult::UNSUPPORTED;
      } else if (byte == 0x00 || (byte >= 0xD0 && byte <= 0xD7)) {
        is_marker = false;
      } else if (byte != 0xFF) {
        return JpegResult::INVALID;
      }
    }
    at += count;
  }
  return JpegResult::INVALID;
}

JpegResult JpegDecoder::decode(const Rect &rect, uint8_t *target, int stride, PixelWriter *writer) {
  if (!is_ready()) {
    return JpegResult::INVALID;
  }
  JpegInfo info;
  const JpegResult result = inspect(info);
  if (result != JpegResult::OK) {
    return result;
  }
  if (info.width != rect.width || info.height != rect.height) {
    return JpegResult::SIZE_MISMATCH;
  }
  rect_ = rect;
  target_ = target;
  stride_ = stride;
  writer_ = writer;
  drawn_ = 0;
  is_outside_ = false;
  band_width_ = 0;
  if (!codec_->open(this, int(size_), nullptr, read_callback, seek_callback, draw_callback)) {
    return JpegResult::INVALID;
  }
  codec_->setPixelType(RGB565_BIG_ENDIAN);
  codec_->setUserPointer(this);
  bool is_ok = codec_->getWidth() == rect.width && codec_->getHeight() == rect.height &&
               codec_->getJPEGType() == JPEG_MODE_BASELINE;
  is_ok = is_ok && codec_->decode(0, 0, 0);
  codec_->close();
  flush_band_();
  // Every pixel of the rectangle exactly once, and nothing outside it.
  is_ok = is_ok && !is_outside_ && drawn_ == size_t(rect.width) * size_t(rect.height);
  return is_ok ? JpegResult::OK : JpegResult::INVALID;
}

// One block of pixels, `pitch` apart, of which the first `width` columns are in the image.
bool JpegDecoder::draw_(int x, int y, int pitch, int width, int height, const uint16_t *pixels) {
  if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > pitch || x + width > rect_.width ||
      y + height > rect_.height) {
    is_outside_ = true;
    return false;
  }
  drawn_ += size_t(width) * size_t(height);
  // Strips arrive left to right along one MCU row. Anything else is written as it comes.
  if (band_width_ && (y != band_y_ || height != band_height_ || x != band_width_)) {
    flush_band_();
  }
  if (band_ && height <= BAND_ROWS && x == band_width_) {
    if (!band_width_) {
      band_y_ = y;
      band_height_ = height;
    }
    for (int row = 0; row < height; ++row) {
      memcpy(band_ + size_t(row) * size_t(rect_.width) + size_t(x), pixels + size_t(row) * pitch,
             size_t(width) * 2);
    }
    band_width_ = x + width;
    if (band_width_ == rect_.width) {
      flush_band_();
    }
    return true;
  }
  const size_t bytes = size_t(width) * 2;
  for (int row = 0; row < height; ++row) {
    uint8_t *to = target_ + (size_t(y + row) * size_t(stride_) + size_t(x)) * 2;
    writer_->write(to, reinterpret_cast<const uint8_t *>(pixels + size_t(row) * pitch), bytes);
  }
  return true;
}

void JpegDecoder::flush_band_() {
  if (!band_width_) {
    return;
  }
  const uint8_t *from = reinterpret_cast<const uint8_t *>(band_);
  uint8_t *to = target_ + (size_t(band_y_) * size_t(stride_)) * 2;
  if (band_width_ == rect_.width && rect_.width == stride_) {
    // Full-width rows are one contiguous run in the framebuffer.
    writer_->write(to, from, size_t(rect_.width) * size_t(band_height_) * 2);
  } else {
    for (int row = 0; row < band_height_; ++row) {
      writer_->write(to + size_t(row) * size_t(stride_) * 2,
                     from + size_t(row) * size_t(rect_.width) * 2, size_t(band_width_) * 2);
    }
  }
  band_width_ = 0;
}

} // namespace esphome::presto_display
