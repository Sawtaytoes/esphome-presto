"""Codec 2 (baseline JPEG patches) through the actual C++ receiver, on the host.

Every image here is synthetic: gradients, color bars, solid colors, smooth texture, and noise.
JPEG is lossy, so decoded pixels are compared with Pillow's (libjpeg's) decode of the same
file, within a tolerance. The luma of the two decodes matches to RGB565 precision. Chroma is
compared as the mean over each subsampling block: JPEGDEC replicates a subsampled chroma
sample over its block where libjpeg interpolates between neighbors, so per pixel the two
differ at every chroma edge while each block's mean stays close.
"""

import io
import random
import struct
import tempfile
import unittest

from PIL import Image
from test_receiver import (
    FULL,
    JPEG_BODY_LIMIT,
    SYNC_DELAY_MS,
    Board,
    chunks,
    compile_receiver,
    crop,
    packet,
    paste,
    view,
)

CAMERA = (0, 60, 480, 360)
# Pillow's `subsampling`: 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0, and the chroma block of each.
SAMPLINGS = {"4:4:4": 0, "4:2:2": 1, "4:2:0": 2}
BLOCKS = {"4:4:4": (1, 1), "4:2:2": (2, 1), "4:2:0": (2, 2)}
# In 8-bit units against libjpeg's decode. RGB565 quantization alone is a mean of about 1.5
# in luma; measured means are 1.3 to 1.6 in luma and 0.2 to 2.9 in block chroma. Where a
# saturated color clips, the chroma difference reaches the luma of single pixels, so the
# luma bound is on the 99.9th percentile: one wrong 8x8 block in a 480x360 patch exceeds it.
LUMA_MEAN_TOLERANCE = 2.0
LUMA_RARE_TOLERANCE = 24
CHROMA_MEAN_TOLERANCE = 4.0


def gradient(width, height):
    image = Image.new("RGB", (width, height))
    image.putdata(
        [
            (
                x * 255 // max(width - 1, 1),
                y * 255 // max(height - 1, 1),
                (x + y) * 255 // (width + height),
            )
            for y in range(height)
            for x in range(width)
        ]
    )
    return image


def color_bars(width, height):
    colors = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0)]
    colors += [(255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    image = Image.new("RGB", (width, height))
    image.putdata([colors[x * 8 // width] for _ in range(height) for x in range(width)])
    return image


def noise(width, height, seed=1):
    return Image.frombytes(
        "RGB", (width, height), random.Random(seed).randbytes(width * height * 3)
    )


def texture(width, height, seed=1):
    """Smooth color texture: coarse noise scaled up, closer to a camera frame than noise."""
    return noise(width // 8 + 1, height // 8 + 1, seed).resize((width, height), Image.BICUBIC)


def encode(image, quality=75, sampling="4:2:0", **options):
    output = io.BytesIO()
    image.save(output, "JPEG", quality=quality, subsampling=SAMPLINGS[sampling], **options)
    return output.getvalue()


def jpeg_packet(base, rect, jpeg):
    return struct.pack(">IHHHHB", base, *rect, 2) + jpeg


def triples(image):
    data = image.convert("RGB").tobytes()
    return [tuple(data[at : at + 3]) for at in range(0, len(data), 3)]


def to_rgb565(image):
    """Big-endian RGB565 rows, the framebuffer's byte order."""
    output = bytearray()
    for red, green, blue in triples(image):
        value = (red >> 3) << 11 | (green >> 2) << 5 | blue >> 3
        output += bytes((value >> 8, value & 0xFF))
    return bytes(output)


def expand(pixels):
    """Big-endian RGB565 to 8-bit RGB triples, by bit replication."""
    for at in range(0, len(pixels), 2):
        value = pixels[at] << 8 | pixels[at + 1]
        red, green, blue = value >> 11, value >> 5 & 0x3F, value & 0x1F
        yield red << 3 | red >> 2, green << 2 | green >> 4, blue << 3 | blue >> 2


def to_ycbcr(red, green, blue):
    return (
        0.299 * red + 0.587 * green + 0.114 * blue,
        -0.1687 * red - 0.3313 * green + 0.5 * blue,
        0.5 * red - 0.4187 * green - 0.0813 * blue,
    )


def fidelity(pixels, jpeg, width, sampling):
    """Luma mean and 99.9th-percentile error, and block-mean chroma error, against libjpeg."""
    decoded = [to_ycbcr(*pixel) for pixel in expand(pixels)]
    expected = [to_ycbcr(*pixel) for pixel in triples(Image.open(io.BytesIO(jpeg)))]
    assert len(decoded) == len(expected)
    luma = [abs(a[0] - b[0]) for a, b in zip(decoded, expected, strict=True)]
    block_width, block_height = BLOCKS[sampling]
    height = len(decoded) // width
    chroma, blocks = 0.0, 0
    for y in range(0, height - block_height + 1, block_height):
        for x in range(0, width - block_width + 1, block_width):
            block = [
                (y + dy) * width + x + dx for dy in range(block_height) for dx in range(block_width)
            ]
            for channel in (1, 2):
                difference = sum(decoded[i][channel] - expected[i][channel] for i in block)
                chroma += abs(difference) / len(block)
                blocks += 1
    rare = sorted(luma)[len(luma) * 999 // 1000]
    return sum(luma) / len(luma), rare, chroma / max(blocks, 1)


def with_comment_padding(jpeg, size):
    """Pad a JPEG to exactly `size` bytes with comment segments after SOI."""
    padding = bytearray()
    left = size - len(jpeg)
    while left:
        # A segment is at least 4 bytes (marker and length); never leave 1 to 3 over.
        body = min(left - 4, 65533)
        if 0 < left - (body + 4) < 4:
            body -= 4
        padding += b"\xff\xfe" + struct.pack(">H", body + 2) + bytes(body)
        left -= body + 4
    return jpeg[:2] + bytes(padding) + jpeg[2:]


def segment(jpeg, marker):
    """Offset of the first segment with `marker` (e.g. 0xC0) before the scan data."""
    at = 2
    while True:
        assert jpeg[at] == 0xFF
        if jpeg[at + 1] == marker:
            return at
        if jpeg[at + 1] == 0xDA:
            raise AssertionError(f"no 0x{marker:02X} segment")
        at += 2 + struct.unpack(">H", jpeg[at + 2 : at + 4])[0]


def without(jpeg, marker):
    """The JPEG with every segment that has `marker` removed."""
    while True:
        try:
            at = segment(jpeg, marker)
        except AssertionError:
            return jpeg
        jpeg = jpeg[:at] + jpeg[at + 2 + struct.unpack(">H", jpeg[at + 2 : at + 4])[0] :]


class JpegPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        cls.native = compile_receiver(cls.scratch.name)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def setUp(self):
        self.board = Board(self.native)
        self.rng = random.Random(self._testMethodName)

    def tearDown(self):
        self.assertEqual(self.board.visible_writes(), 0, "a write reached the visible buffer")
        self.assertEqual(self.board.double_publishes(), 0, "one call published twice")
        self.assertEqual(self.board.untracked_differences(), 0, "stale tracking is wrong")
        self.board.close()

    def show_full(self, frame_id=1):
        image = view(self.rng)
        self.board.live(frame_id, packet(0, FULL, image))
        self.assert_ack(frame_id)
        return image

    def assert_ack(self, frame_id, touch_id=0):
        events = self.board.new_events()
        self.assertEqual(len(events), 1, events)
        fields = events[0].split(",")
        self.assertEqual(len(fields), 7, events)
        self.assertEqual(fields[:4], ["frame", str(frame_id), str(touch_id), "0"])

    def send_jpeg(self, frame_id, base, rect, jpeg):
        self.board.live(frame_id, jpeg_packet(base, rect, jpeg))

    def assert_rejected(self, frame_id, base, rect, jpeg, reason):
        before = self.board.picture()
        presented = self.board.presented_frame()
        self.send_jpeg(frame_id, base, rect, jpeg)
        events = self.board.new_events()
        self.assertTrue(events, "no answer")
        self.assertEqual(events[0], f"error,{reason}")
        self.assertEqual(self.board.picture(), before)
        self.assertEqual(self.board.presented_frame(), presented)

    def assert_patch(self, before, rect, jpeg, sampling="4:2:0", is_chroma_checked=True):
        """The rectangle holds the JPEG within tolerance; every other byte is unchanged."""
        picture = self.board.picture()
        self.assertEqual(paste(picture, rect, crop(before, rect)), before, "outside changed")
        luma_mean, luma_rare, chroma_mean = fidelity(crop(picture, rect), jpeg, rect[2], sampling)
        report = (luma_mean, luma_rare, chroma_mean)
        self.assertLessEqual(luma_mean, LUMA_MEAN_TOLERANCE, report)
        self.assertLessEqual(luma_rare, LUMA_RARE_TOLERANCE, report)
        if is_chroma_checked:
            self.assertLessEqual(chroma_mean, CHROMA_MEAN_TOLERANCE, report)
        return picture

    def test_advertises_jpeg_only_with_its_buffer(self):
        self.assertEqual(self.board.codecs(), b"zlib,rle,jpeg")
        plain = Board(self.native, jpeg_capacity=0)
        try:
            self.assertEqual(plain.codecs(), b"zlib,rle")
            image = view(self.rng)
            plain.live(1, packet(0, FULL, image))
            plain.new_events()
            plain.live(2, jpeg_packet(1, (8, 8, 64, 48), encode(gradient(64, 48))))
            self.assertEqual(plain.new_events(), ["error,jpeg_unavailable"])
            self.assertEqual(plain.picture(), image)
            self.assertEqual(plain.visible_writes(), 0)
        finally:
            plain.close()

    def test_camera_patch_replaces_exactly_its_rectangle(self):
        before = self.show_full()
        frame_id = 1
        for name, source in [
            ("gradient", gradient(480, 360)),
            ("bars", color_bars(480, 360)),
            ("texture", texture(480, 360)),
        ]:
            for sampling in SAMPLINGS:
                with self.subTest(image=name, sampling=sampling):
                    jpeg = encode(source, sampling=sampling)
                    self.send_jpeg(frame_id + 1, frame_id, CAMERA, jpeg)
                    frame_id += 1
                    self.assert_ack(frame_id)
                    before = self.assert_patch(before, CAMERA, jpeg, sampling)

    def test_a_camera_patch_reaches_psram_in_whole_rows(self):
        # Every PSRAM write pays the scanout gate and the pacing pause, so a 4:2:0 patch
        # must not go out as JPEGDEC's 128-pixel strips: 1572 calls for this patch before, 155 now.
        before = self.show_full()
        jpeg = encode(texture(480, 360), sampling="4:2:0")
        calls = self.board.write_calls()
        self.send_jpeg(2, 1, CAMERA, jpeg)
        self.assert_ack(2)
        self.assertLess(self.board.write_calls() - calls, 200)
        self.assert_patch(before, CAMERA, jpeg)

    def test_a_partial_width_patch_is_exact_around_its_rectangle(self):
        before = self.show_full()
        rect = (40, 24, 200, 72)
        jpeg = encode(gradient(200, 72), sampling="4:2:0")
        self.send_jpeg(2, 1, rect, jpeg)
        self.assert_ack(2)
        self.assert_patch(before, rect, jpeg)

    def test_noise_patch(self):
        before = self.show_full()
        rect = (120, 160, 240, 160)
        frame_id = 1
        for sampling in SAMPLINGS:
            with self.subTest(sampling=sampling):
                jpeg = encode(noise(240, 160, seed=frame_id), sampling=sampling)
                self.send_jpeg(frame_id + 1, frame_id, rect, jpeg)
                frame_id += 1
                self.assert_ack(frame_id)
                # Noise has a chroma edge at every sample, where the two upsamplers differ
                # most; its chroma is compared only without subsampling.
                is_chroma_checked = sampling == "4:4:4"
                before = self.assert_patch(before, rect, jpeg, sampling, is_chroma_checked)

    def test_odd_sizes_and_small_patches(self):
        before = self.show_full()
        frame_id = 1
        # Edges that cut through MCUs, a 1x1, and patches small enough to decode via SRAM.
        for rect in [(377, 441, 101, 37), (0, 0, 1, 1), (200, 13, 30, 30), (5, 400, 17, 80)]:
            for sampling in SAMPLINGS:
                with self.subTest(rect=rect, sampling=sampling):
                    jpeg = encode(gradient(rect[2], rect[3]), quality=90, sampling=sampling)
                    self.send_jpeg(frame_id + 1, frame_id, rect, jpeg)
                    frame_id += 1
                    self.assert_ack(frame_id)
                    before = self.assert_patch(before, rect, jpeg, sampling)

    def test_byte_order_matches_codec_0(self):
        before = self.show_full()
        rect = (100, 100, 64, 32)
        frame_id = 1
        for color in [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (40, 120, 200)]:
            with self.subTest(color=color):
                jpeg = encode(Image.new("RGB", rect[2:], color), quality=95, sampling="4:4:4")
                self.send_jpeg(frame_id + 1, frame_id, rect, jpeg)
                frame_id += 1
                self.assert_ack(frame_id)
                before = self.assert_patch(before, rect, jpeg, "4:4:4")
                # The same color as codec 0 would send it: within one RGB565 step per channel.
                expected = next(expand(to_rgb565(Image.new("RGB", (1, 1), color))))
                decoded = crop(before, rect)[:2]
                for got, want in zip(next(expand(decoded)), expected, strict=True):
                    self.assertLessEqual(abs(got - want), 8, (decoded.hex(), color))

    def test_restart_markers_and_application_segments(self):
        before = self.show_full()
        source = texture(160, 96)
        rect = (300, 200, 160, 96)
        plain = encode(source)
        self.send_jpeg(2, 1, rect, plain)
        self.assert_ack(2)
        before = self.assert_patch(before, rect, plain)
        # Restart markers and a comment change the file, not the decoded pixels.
        marked = encode(source, restart_marker_blocks=3, comment=b"synthetic test card")
        self.assertIn(b"\xff\xdd", marked)
        self.assertIn(b"\xff\xd0", marked)
        self.send_jpeg(3, 2, rect, marked)
        self.assert_ack(3)
        self.assertEqual(self.board.picture(), before)

    def test_size_mismatch_is_rejected(self):
        self.show_full()
        jpeg = encode(gradient(200, 100))
        for rect in [(10, 10, 200, 101), (10, 10, 199, 100), (0, 0, 100, 200)]:
            with self.subTest(rect=rect):
                self.assert_rejected(2, 1, rect, jpeg, "jpeg_size_mismatch")
        # The base is still frame 1.
        self.send_jpeg(2, 1, (10, 10, 200, 100), jpeg)
        self.assert_ack(2)

    def test_truncated_and_corrupt_bodies_are_rejected(self):
        self.show_full()
        rect = (40, 40, 240, 160)
        jpeg = encode(noise(240, 160))
        scan = segment(jpeg, 0xDA)
        frame = segment(jpeg, 0xC0)
        middle = (scan + len(jpeg)) // 2
        cases = {
            "truncated": jpeg[: len(jpeg) // 2],
            "no end of image": jpeg[:-2],
            "one byte": jpeg[:1],
            "random bytes": self.rng.randbytes(4000),
            "trailing bytes": jpeg + b"\x00\x00",
            "data after end of image": jpeg[:-2] + b"\xff\xd9\x12\x34\xff\xd9",
            "bad marker in the scan": jpeg[:middle] + b"\xff\x55" + jpeg[middle + 2 :],
            "frame length": jpeg[: frame + 3] + b"\x09" + jpeg[frame + 4 :],
            "no Huffman tables": without(jpeg, 0xC4),
            "no quantization tables": without(jpeg, 0xDB),
            "no start of image": b"\x00\x00" + jpeg[2:],
        }
        for name, body in cases.items():
            with self.subTest(name):
                self.assert_rejected(2, 1, rect, body, "jpeg_invalid")
        self.send_jpeg(2, 1, rect, jpeg)
        self.assert_ack(2)

    def test_oversize_body_is_rejected_during_transfer(self):
        image = self.show_full()
        rect = (0, 0, 480, 480)
        jpeg = encode(noise(480, 480), quality=100, sampling="4:4:4")
        self.assertGreater(len(jpeg), JPEG_BODY_LIMIT)
        pieces = chunks(jpeg_packet(1, rect, jpeg))
        answered_at = None
        for index, (offset, is_final, piece) in enumerate(pieces):
            self.board.frame_chunk(2, 0, 4, offset, is_final, piece, len(piece))
            events = self.board.new_events()
            if events and answered_at is None:
                self.assertEqual(events, ["error,jpeg_too_large"])
                answered_at = index
        # Rejected as soon as the body passed the limit, long before the final chunk.
        self.assertIsNotNone(answered_at)
        self.assertLess(answered_at, len(pieces) - 1)
        self.assertEqual(self.board.picture(), image)
        self.assertEqual(self.board.presented_frame(), 1)

    def test_body_at_the_limit_is_accepted(self):
        before = self.show_full()
        rect = (0, 0, 64, 64)
        jpeg = encode(gradient(64, 64))
        exact = with_comment_padding(jpeg, JPEG_BODY_LIMIT)
        self.assertEqual(len(exact), JPEG_BODY_LIMIT)
        self.send_jpeg(2, 1, rect, exact)
        self.assert_ack(2)
        self.assert_patch(before, rect, jpeg)
        over = with_comment_padding(jpeg, JPEG_BODY_LIMIT + 4)
        self.assert_rejected(3, 2, rect, over, "jpeg_too_large")

    def test_unsupported_encodings_are_rejected(self):
        self.show_full()
        rect = (16, 16, 128, 96)
        source = gradient(128, 96)
        baseline = encode(source)
        frame = segment(baseline, 0xC0)

        def frame_marker(code):
            return baseline[: frame + 1] + bytes([code]) + baseline[frame + 2 :]

        cases = {
            "progressive": encode(source, progressive=True),
            "arithmetic": frame_marker(0xC9),
            "extended sequential": frame_marker(0xC1),
            "lossless": frame_marker(0xC3),
            "12-bit": baseline[: frame + 4] + b"\x0c" + baseline[frame + 5 :],
            "grayscale": encode(source.convert("L")),
        }
        for name, body in cases.items():
            with self.subTest(name):
                self.assert_rejected(2, 1, rect, body, "jpeg_unsupported")
        self.send_jpeg(2, 1, rect, baseline)
        self.assert_ack(2)

    def test_slots_reject_jpeg(self):
        image = self.show_full()
        jpeg = encode(gradient(480, 480))
        self.board.slot(0, 70, jpeg_packet(0, FULL, jpeg))
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["slot,0,70,3"])
        self.assertEqual(self.board.slot_frame(0), 0)
        self.board.show_slot(0, 0)
        self.assertEqual(self.board.new_events(), ["error,slot_unavailable"])
        self.assertEqual(self.board.picture(), image)

    def test_jpeg_and_zlib_patches_chain_and_settle(self):
        picture = self.show_full()
        frame_id = 1
        for turn in range(6):
            if turn % 2 == 0:
                rect = CAMERA if turn == 0 else (64, 32 * turn, 320, 200)
                jpeg = encode(texture(rect[2], rect[3], seed=turn))
                self.send_jpeg(frame_id + 1, frame_id, rect, jpeg)
                frame_id += 1
                self.assert_ack(frame_id)
                picture = self.assert_patch(picture, rect, jpeg)
            else:
                rect = (100 + turn, 300, 200, 150)
                picture = paste(picture, rect, self.rng.randbytes(200 * 150 * 2))
                self.board.live(frame_id + 1, packet(frame_id, rect, crop(picture, rect)))
                frame_id += 1
                self.assert_ack(frame_id)
                self.assertEqual(self.board.picture(), picture)
        self.board.advance(SYNC_DELAY_MS)
        self.board.idle()
        self.assertFalse(self.board.is_settling())
        self.assertEqual(self.board.own(0), picture)
        self.assertEqual(self.board.own(1), picture)


if __name__ == "__main__":
    unittest.main()
