"""Exercise the actual C++ streaming decoder without a board or ESPHome runtime."""

import ctypes
import pathlib
import random
import struct
import subprocess
import tempfile
import unittest
import zlib


class PixelDecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        root = pathlib.Path(__file__).resolve().parents[1] / "components" / "presto_display"
        wrapper = pathlib.Path(cls.scratch.name) / "wrapper.cpp"
        wrapper.write_text("""#include "presto_stream.h"
#include <cstring>
#include <random>
using namespace esphome::presto_display;
struct Memory : PixelWriter {
  void write(uint8_t *to, const uint8_t *from, size_t size) override { memcpy(to, from, size); }
};
// Decode a whole packet into a compact width x height buffer, feeding the body in random
// chunks of 1..max_chunk bytes with an output budget per call (SIZE_MAX for none).
extern "C" bool decode_chunked(const unsigned char *packet, size_t size, unsigned base,
                               unsigned char *output, size_t max_chunk, size_t budget,
                               unsigned seed) {
  static tinfl_decompressor inflater;
  static uint8_t dictionary[TINFL_LZ_DICT_SIZE];
  Memory memory;
  StreamDecoder decoder(&inflater, dictionary, &memory);
  PatchHeader header;
  if (size < HEADER_BYTES || !parse_header(packet, base, header)) {
    return false;
  }
  decoder.begin(header, output, header.rect.width);
  std::mt19937 rng(seed);
  size_t offset = HEADER_BYTES;
  auto status = StreamDecoder::Status::NEED_INPUT;
  while (offset < size) {
    const size_t count = std::min(size - offset, size_t(1 + rng() % max_chunk));
    const bool is_last = offset + count == size;
    size_t used = 0;
    do {
      size_t consumed = 0;
      status = decoder.feed(packet + offset + used, count - used, is_last, consumed, budget);
      used += consumed;
    } while (status == StreamDecoder::Status::MORE);
    if (status == StreamDecoder::Status::FAILED) {
      return false;
    }
    offset += count;
  }
  return status == StreamDecoder::Status::COMPLETE;
}
extern "C" bool decode(const unsigned char *packet, size_t size, unsigned base,
                       unsigned char *output) {
  return decode_chunked(packet, size, base, output, size, SIZE_MAX, 1);
}
""")
        library = pathlib.Path(cls.scratch.name) / "pixels.so"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-shared",
                "-fPIC",
                "-O2",
                "-I",
                str(root),
                str(wrapper),
                str(root / "presto_stream.cpp"),
                str(root / "miniz_tinfl.c"),
                "-o",
                str(library),
            ],
            check=True,
        )
        native = ctypes.CDLL(str(library))
        cls.decode = native.decode
        cls.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p]
        cls.decode.restype = ctypes.c_bool
        cls.decode_chunked = native.decode_chunked
        cls.decode_chunked.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_uint32,
        ]
        cls.decode_chunked.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def packet(self, body, base=7, x=2, y=3, width=2, height=2, codec=0):
        return struct.pack(">IHHHHB", base, x, y, width, height, codec) + body

    def decoded(self, packet, base=7):
        # Guard bytes catch writes outside the maximum decoded frame allocation.
        output = ctypes.create_string_buffer(b"\xaa" * (480 * 480 * 2 + 16))
        accepted = self.decode(packet, len(packet), base, output)
        self.assertEqual(output.raw[480 * 480 * 2 :][:16], b"\xaa" * 16)
        return accepted, output.raw

    def test_valid_zlib_keeps_rgb565_byte_order(self):
        pixels = bytes.fromhex("f80007e0001fffff")
        accepted, output = self.decoded(self.packet(zlib.compress(pixels)))
        self.assertTrue(accepted)
        self.assertEqual(output[:8], pixels)

    def test_full_frame_without_a_base(self):
        pixels = b"\x12\x34" * (480 * 480)
        accepted, output = self.decoded(
            self.packet(
                zlib.compress(pixels),
                base=0,
                x=0,
                y=0,
                width=480,
                height=480,
            )
        )
        self.assertTrue(accepted)
        self.assertEqual(output[: len(pixels)], pixels)

    def test_large_artwork_wraps_input_windows_and_output_dictionary(self):
        # Incompressible pixels force more than one input window; repetitions
        # also require back-references across dictionary wraps.
        rng = random.Random(123)
        block = rng.randbytes(30000)
        pixels = (block * 16)[: 480 * 480 * 2]
        body = zlib.compress(pixels)
        self.assertGreater(len(body), 4096)
        packet = self.packet(body, base=0, x=0, y=0, width=480, height=480)
        accepted, output = self.decoded(packet)
        self.assertTrue(accepted)
        self.assertEqual(output[: len(pixels)], pixels)
        self.assertFalse(self.decoded(packet[:-1])[0])

    def test_rle_repeat_and_literal_runs(self):
        body = struct.pack("<H", 0x8001) + b"\x12\x34" + struct.pack("<H", 1) + b"\x56\x78\x9a\xbc"
        accepted, output = self.decoded(self.packet(body, codec=1))
        self.assertTrue(accepted)
        self.assertEqual(output[:8], bytes.fromhex("1234123456789abc"))

    def test_rejects_wrong_base_and_partial_unbased_frames(self):
        body = zlib.compress(bytes(8))
        self.assertFalse(self.decoded(self.packet(body), base=8)[0])
        self.assertFalse(self.decoded(self.packet(body, base=0))[0])

    def test_rejects_invalid_rectangles_and_codecs(self):
        body = zlib.compress(bytes(8))
        for settings in [
            {"x": 479},
            {"y": 479},
            {"width": 0},
            {"height": 0},
            {"width": 65535},
            {"codec": 9},
        ]:
            with self.subTest(settings=settings):
                self.assertFalse(self.decoded(self.packet(body, **settings))[0])

    def test_rejects_zlib_corruption_wrong_length_trailing_or_truncated_data(self):
        body = zlib.compress(bytes(8))
        for invalid in [
            body[:-1],
            body + b"tail",
            body[:-1] + bytes([body[-1] ^ 1]),
            zlib.compress(bytes(10)),
            zlib.compress(bytes(6)),
        ]:
            with self.subTest(body=invalid):
                self.assertFalse(self.decoded(self.packet(invalid))[0])

    def test_rejects_rle_overflow_truncation_and_short_output(self):
        for invalid in [
            b"\xff\xff\x12\x34",
            b"\x01",
            b"\x01\x80\x12",
            b"\x00\x80\x12\x34",
            b"\x03\x00\x12\x34",
        ]:
            with self.subTest(body=invalid):
                self.assertFalse(self.decoded(self.packet(invalid, codec=1))[0])

    def chunked(self, packet, max_chunk, budget, seed, base=7):
        output = ctypes.create_string_buffer(b"\xaa" * (480 * 480 * 2 + 16))
        accepted = self.decode_chunked(
            packet, len(packet), base, output, max_chunk, budget or (2**64 - 1), seed
        )
        self.assertEqual(output.raw[480 * 480 * 2 :][:16], b"\xaa" * 16)
        return accepted, output.raw

    def test_any_chunking_and_budget_gives_the_same_pixels(self):
        # Chunks of up to 3,000 B are what the receiver sees; 7-byte chunks straddle every
        # RLE control word and run value; a small budget splits every chunk's output.
        rng = random.Random(7)
        pixels = bytearray(b"\x12\x34" * (480 * 480))
        for _ in range(40):
            start = rng.randrange(0, len(pixels) - 2000, 2)
            pixels[start : start + 2000] = rng.randbytes(2000)
        pixels = bytes(pixels)
        rle = rle_encode(pixels)
        for codec, body in [(0, zlib.compress(pixels)), (1, rle)]:
            packet = self.packet(body, base=0, x=0, y=0, width=480, height=480, codec=codec)
            for max_chunk, budget in [(3000, 0), (7, 0), (3000, 1), (3000, 5000), (911, 960)]:
                with self.subTest(codec=codec, max_chunk=max_chunk, budget=budget):
                    accepted, output = self.chunked(packet, max_chunk, budget, max_chunk + budget)
                    self.assertTrue(accepted)
                    self.assertEqual(output[: len(pixels)], pixels)
                    self.assertFalse(self.chunked(packet[:-1], max_chunk, budget, 3)[0])
                    self.assertFalse(self.chunked(packet + b"\0\0", max_chunk, budget, 3)[0])

    def test_rejects_missing_header(self):
        for size in range(14):
            self.assertFalse(self.decoded(bytes(size))[0])


def rle_encode(pixels):
    """CastKit's RLE: little-endian control words; bit 15 repeats one pixel."""
    out = bytearray()
    words = [pixels[i : i + 2] for i in range(0, len(pixels), 2)]
    i = 0
    while i < len(words):
        run = 1
        while i + run < len(words) and run < 0x8000 and words[i + run] == words[i]:
            run += 1
        if run > 1:
            out += struct.pack("<H", 0x8000 | (run - 1)) + words[i]
            i += run
            continue
        start = i
        while i < len(words) and i - start < 0x8000:
            if i + 1 < len(words) and words[i + 1] == words[i]:
                break
            i += 1
        i = max(i, start + 1)
        out += struct.pack("<H", i - start - 1) + b"".join(words[start:i])
    return bytes(out)


if __name__ == "__main__":
    unittest.main()
