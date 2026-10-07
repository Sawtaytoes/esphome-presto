"""Exercise the actual C++ decoder without a board or ESPHome runtime."""

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
        wrapper.write_text("""#include "presto_pixels.h"
extern "C" bool decode(const unsigned char *packet, size_t size, unsigned base,
                       unsigned char *output) {
  tinfl_decompressor inflater;
  return esphome::presto_display::decode_pixels(packet, size, base, output, &inflater);
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
                str(root / "presto_pixels.cpp"),
                str(root / "miniz_tinfl.c"),
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.decode = ctypes.CDLL(str(library)).decode
        cls.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p]
        cls.decode.restype = ctypes.c_bool

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

    def test_rejects_missing_header(self):
        for size in range(14):
            self.assertFalse(self.decoded(bytes(size))[0])


if __name__ == "__main__":
    unittest.main()
