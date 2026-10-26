"""Drive the actual C++ receiver (live frames, frame slots, idle sync) on the host."""

import base64
import ctypes
import pathlib
import random
import struct
import subprocess
import tempfile
import unittest
import zlib

from test_pixels import rle_encode

PANEL = 480
FRAME = PANEL * PANEL * 2
FULL = (0, 0, PANEL, PANEL)
SIGNATURES = {
    "create": ([ctypes.c_int], ctypes.c_void_p),
    "destroy": ([ctypes.c_void_p], None),
    "frame_chunk": (
        [ctypes.c_void_p] + [ctypes.c_int] * 4 + [ctypes.c_bool, ctypes.c_char_p, ctypes.c_size_t],
        None,
    ),
    "slot_chunk": (
        [ctypes.c_void_p] + [ctypes.c_int] * 4 + [ctypes.c_bool, ctypes.c_char_p, ctypes.c_size_t],
        None,
    ),
    "show_slot": ([ctypes.c_void_p, ctypes.c_int, ctypes.c_int], None),
    "drop_slots": ([ctypes.c_void_p], None),
    "step": ([ctypes.c_void_p], ctypes.c_bool),
    "advance": ([ctypes.c_void_p, ctypes.c_uint], None),
    "event_count": ([ctypes.c_void_p], ctypes.c_int),
    "event": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_char_p),
    "timing_count": ([ctypes.c_void_p], ctypes.c_int),
    "timing": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_char_p),
    "visible": ([ctypes.c_void_p], ctypes.c_void_p),
    "framebuffer": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_void_p),
    "slot_memory": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_void_p),
    "slot_frame": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_uint),
    "presented_frame": ([ctypes.c_void_p], ctypes.c_int),
    "touch_frame": ([ctypes.c_void_p], ctypes.c_int),
    "is_settling": ([ctypes.c_void_p], ctypes.c_bool),
    "is_slot_visible": ([ctypes.c_void_p], ctypes.c_bool),
    "shows": ([ctypes.c_void_p], ctypes.c_int),
    "written": ([ctypes.c_void_p], ctypes.c_size_t),
}


def packet(base, rect, pixels, codec=0):
    body = zlib.compress(pixels) if codec == 0 else rle_encode(pixels)
    return struct.pack(">IHHHHB", base, *rect, codec) + body


def chunks(data):
    """The worker's chunking: base64 of the whole packet, 4,000 characters per call."""
    encoded = base64.b64encode(data)
    pieces = [encoded[i : i + 4000] for i in range(0, len(encoded), 4000)]
    return [(i * 4000, i == len(pieces) - 1, piece) for i, piece in enumerate(pieces)]


def crop(image, rect):
    x, y, width, height = rect
    return b"".join(
        bytes(image[((y + row) * PANEL + x) * 2 : ((y + row) * PANEL + x + width) * 2])
        for row in range(height)
    )


def paste(image, rect, pixels):
    x, y, width, height = rect
    image = bytearray(image)
    for row in range(height):
        start = ((y + row) * PANEL + x) * 2
        image[start : start + width * 2] = pixels[row * width * 2 : (row + 1) * width * 2]
    return bytes(image)


def view(rng, noise=64):
    """A UI-like frame: solid panels, plus one noisy block so uploads span several chunks."""
    image = bytes(rng.randbytes(2)) * (PANEL * PANEL)
    for _ in range(5):
        rect = random_rect(rng)
        image = paste(image, rect, rng.randbytes(2) * (rect[2] * rect[3]))
    if noise:
        x, y = rng.randrange(PANEL - noise), rng.randrange(PANEL - noise)
        image = paste(image, (x, y, noise, noise), rng.randbytes(noise * noise * 2))
    return image


def random_rect(rng):
    x, y = rng.randrange(PANEL), rng.randrange(PANEL)
    return x, y, rng.randint(1, PANEL - x), rng.randint(1, PANEL - y)


class Board:
    def __init__(self, native, slot_count=6):
        self.native = native
        self.handle = native.create(slot_count)
        self.seen = 0

    def close(self):
        self.native.destroy(self.handle)

    def __getattr__(self, name):
        function = getattr(self.native, name)
        return lambda *arguments: function(self.handle, *arguments)

    def new_events(self):
        count = self.event_count()
        events = [self.event(i).decode() for i in range(self.seen, count)]
        self.seen = count
        return events

    def timings(self):
        return [self.timing(i).decode() for i in range(self.timing_count())]

    def picture(self):
        return ctypes.string_at(self.visible(), FRAME)

    def slot_pixels(self, slot):
        return ctypes.string_at(self.slot_memory(slot), FRAME)

    def own(self, index):
        return ctypes.string_at(self.framebuffer(index), FRAME)

    def live(self, frame_id, data, touch_id=0, between=None):
        for offset, is_final, piece in chunks(data):
            self.frame_chunk(frame_id, touch_id, 4, offset, is_final, piece, len(piece))
            if between and not is_final:
                between()

    def slot(self, slot, frame_id, data, between=None):
        for offset, is_final, piece in chunks(data):
            self.slot_chunk(slot, frame_id, 4, offset, is_final, piece, len(piece))
            if between and not is_final:
                between()

    def idle(self, limit=10000):
        for _ in range(limit):
            if not self.step():
                return
        raise AssertionError("idle work did not finish")


class ReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        tests = pathlib.Path(__file__).resolve().parent
        root = tests.parent / "components" / "presto_display"
        library = pathlib.Path(cls.scratch.name) / "receiver.so"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-shared",
                "-fPIC",
                "-O2",
                "-I",
                str(root),
                str(tests / "receiver_harness.cpp"),
                str(root / "presto_receiver.cpp"),
                str(root / "presto_stream.cpp"),
                str(root / "miniz_tinfl.c"),
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.native = ctypes.CDLL(str(library))
        for name, (arguments, result) in SIGNATURES.items():
            function = getattr(cls.native, name)
            function.argtypes = arguments
            function.restype = result

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def setUp(self):
        self.board = Board(self.native)
        self.rng = random.Random(self._testMethodName)

    def tearDown(self):
        self.board.close()

    def assert_ack(self, events, frame_id, touch_id):
        self.assertEqual(len(events), 1, events)
        fields = events[0].split(",")
        self.assertEqual(len(fields), 7, events)
        self.assertEqual(fields[:4], ["frame", str(frame_id), str(touch_id), "0"])

    def show_live(self, frame_id, image, base=0, rect=FULL, codec=0, touch_id=0):
        self.board.live(frame_id, packet(base, rect, crop(image, rect), codec), touch_id)
        self.assert_ack(self.board.new_events(), frame_id, touch_id)
        self.assertEqual(self.board.picture(), image)

    def upload(self, slot, frame_id, image, base=0, rect=FULL, codec=0, between=None):
        before = self.board.picture()
        self.board.slot(slot, frame_id, packet(base, rect, crop(image, rect), codec), between)
        self.board.idle()
        self.assertEqual(self.board.new_events(), [f"slot,{slot},{frame_id},0"])
        self.assertEqual(self.board.slot_pixels(slot), image)
        self.assertEqual(self.board.slot_frame(slot), frame_id)
        # A slot upload never touches the picture.
        self.assertEqual(self.board.picture(), before)

    # Live frames (unchanged worker contract)

    def test_live_full_frame_patch_and_unchanged_heartbeat(self):
        first = view(self.rng)
        self.show_live(1, first, touch_id=9)
        rect = (40, 300, 400, 120)
        second = paste(first, rect, self.rng.randbytes(400 * 120 * 2))
        self.show_live(2, second, base=1, rect=rect, codec=1)
        shows = self.board.shows()
        # An unchanged 1x1 heartbeat acknowledges without a swap.
        self.show_live(3, second, base=2, rect=(0, 0, 1, 1))
        self.assertEqual(self.board.shows(), shows)
        timing = self.board.timings()[-1].split(",")
        self.assertEqual(timing[0], "timing")
        self.assertEqual(len(timing), 10)

    def test_lazy_sync_copies_nothing_for_a_repeated_region(self):
        image = view(self.rng)
        self.show_live(1, image)
        rect = (296, 128, 95, 120)
        for frame_id in range(2, 8):
            image = paste(image, rect, self.rng.randbytes(95 * 120 * 2))
            self.show_live(frame_id, image, base=frame_id - 1, rect=rect)
            # timing,<frame>,<rect bytes>,<decode>,<write>,<sync us>,<sync bytes>,...
            sync_bytes = int(self.board.timings()[-1].split(",")[6])
            # The first patch after a full frame fills in the rest of its buffer once.
            self.assertEqual(sync_bytes, FRAME - 95 * 120 * 2 if frame_id == 2 else 0)

    def test_live_errors_keep_the_picture(self):
        image = view(self.rng)
        self.show_live(1, image)
        rect = (10, 10, 100, 100)
        patch = packet(7, rect, bytes(100 * 100 * 2))
        self.board.live(2, patch)
        self.assertEqual(self.board.new_events(), ["error,invalid_packet_or_base"])
        self.board.frame_chunk(3, 0, 4, 4000, True, b"AAAA", 4)
        self.assertEqual(self.board.new_events(), ["error,chunk_order_or_size"])
        self.board.frame_chunk(3, 0, 4, 0, True, b"A=AA", 4)
        self.assertEqual(self.board.new_events(), ["error,invalid_base64"])
        self.board.frame_chunk(3, 0, 5, 0, True, b"AAAA", 4)
        self.assertEqual(self.board.new_events(), ["error,invalid_chunk"])
        self.assertEqual(self.board.picture(), image)
        self.assertEqual(self.board.presented_frame(), 1)

    def test_a_failed_large_patch_is_resynchronized(self):
        image = view(self.rng)
        self.show_live(1, image)
        rect = (0, 0, 480, 200)
        broken = packet(1, rect, self.rng.randbytes(480 * 200 * 2))[:-9]
        self.board.live(2, broken)
        self.assertEqual(self.board.new_events(), ["error,invalid_packet_or_base"])
        self.assertEqual(self.board.picture(), image)
        small = (5, 300, 10, 10)
        image = paste(image, small, self.rng.randbytes(200))
        self.show_live(3, image, base=1, rect=small)

    def test_offline_blank_after_seven_seconds(self):
        self.show_live(1, view(self.rng))
        self.board.advance(7001)
        self.board.step()
        self.assertEqual(self.board.presented_frame(), 0)
        self.assertEqual(self.board.touch_frame(), 0)
        self.assertEqual(self.board.picture()[:2], b"\xf8\x00")
        self.assertEqual(self.board.own(0), self.board.own(1))
        self.show_live(2, view(self.rng))

    # Frame slots

    def test_upload_full_and_patch_without_touching_the_picture(self):
        shown = view(self.rng)
        self.show_live(1, shown)
        shows = self.board.shows()
        cached = view(self.rng, noise=200)
        self.upload(0, 100, cached)
        rect = (100, 50, 300, 30)
        patched = paste(cached, rect, self.rng.randbytes(300 * 30 * 2))
        self.upload(0, 101, patched, base=100, rect=rect, codec=1)
        self.assertEqual(self.board.shows(), shows)
        self.assertEqual(self.board.presented_frame(), 1)

    def test_slot_event_waits_for_the_idle_decode_to_finish(self):
        self.show_live(1, view(self.rng))
        # A solid frame is one small chunk but 460,800 B of output: more than one budget.
        self.board.slot(2, 50, packet(0, FULL, b"\x12\x34" * (PANEL * PANEL)))
        self.assertEqual(self.board.new_events(), [])
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,2,50,0"])

    def test_show_slot_acknowledges_with_the_slot_frame(self):
        self.show_live(1, view(self.rng))
        cached = view(self.rng)
        self.upload(3, 200, cached)
        self.board.show_slot(3, 17)
        self.assert_ack(self.board.new_events(), 200, 17)
        self.assertEqual(self.board.picture(), cached)
        self.assertTrue(self.board.is_slot_visible())
        self.assertEqual(self.board.presented_frame(), 200)
        self.assertEqual(self.board.touch_frame(), 200)
        # The next live patch is made against the slot's frame.
        rect = (0, 0, 480, 40)
        image = paste(cached, rect, self.rng.randbytes(480 * 40 * 2))
        self.show_live(2, image, base=200, rect=rect)
        self.assertEqual(self.board.slot_pixels(3), cached)
        rect = (200, 200, 50, 50)
        image = paste(image, rect, self.rng.randbytes(50 * 50 * 2))
        self.show_live(3, image, base=2, rect=rect)

    def test_show_slot_finishes_a_pending_final_chunk(self):
        self.show_live(1, view(self.rng))
        solid = b"\x00\x1f" * (PANEL * PANEL)
        self.board.slot(1, 60, packet(0, FULL, solid))
        self.board.show_slot(1, 4)
        events = self.board.new_events()
        self.assertEqual(events[0], "slot,1,60,0")
        self.assert_ack(events[1:], 60, 4)
        self.assertEqual(self.board.picture(), solid)

    def test_slot_stays_after_showing_and_swipes_back_and_forth(self):
        home = view(self.rng)
        self.show_live(1, home)
        left, right = view(self.rng), view(self.rng)
        self.upload(0, 10, left)
        self.upload(1, 11, right)
        self.upload(2, 12, home)
        frame_id = 100
        for turn in range(12):
            slot = self.rng.choice([0, 1, 2])
            expected = {0: left, 1: right, 2: home}[slot]
            self.board.show_slot(slot, turn)
            self.assert_ack(self.board.new_events(), 10 + slot, turn)
            self.assertEqual(self.board.picture(), expected)
            # Some swipes are followed by a live patch, some by idle time, some by neither.
            if turn % 3 == 0:
                rect = random_rect(self.rng)
                expected = paste(expected, rect, self.rng.randbytes(rect[2] * rect[3] * 2))
                frame_id += 1
                self.show_live(frame_id, expected, base=10 + slot, rect=rect)
            if turn % 2 == 0:
                for _ in range(self.rng.randrange(0, 80)):
                    self.board.step()
                self.assertEqual(self.board.picture(), expected)
        self.board.idle()
        self.assertFalse(self.board.is_settling())
        self.assertFalse(self.board.is_slot_visible())
        for slot, image in enumerate([left, right, home]):
            self.assertEqual(self.board.slot_pixels(slot), image)
            self.assertEqual(self.board.slot_frame(slot), 10 + slot)

    def test_settling_leaves_both_framebuffers_equal_to_the_picture(self):
        self.show_live(1, view(self.rng))
        cached = view(self.rng)
        self.upload(4, 30, cached)
        self.board.show_slot(4, 0)
        self.board.new_events()
        self.assertTrue(self.board.is_settling())
        self.board.idle()
        self.assertFalse(self.board.is_slot_visible())
        self.assertEqual(self.board.own(0), cached)
        self.assertEqual(self.board.own(1), cached)
        self.assertTrue(self.board.timings()[-1].startswith("settled,"))
        # Settled: a small patch needs no copy at all.
        rect = (1, 2, 90, 90)
        image = paste(cached, rect, self.rng.randbytes(90 * 90 * 2))
        self.show_live(2, image, base=30, rect=rect)
        self.assertEqual(self.board.timings()[-1].split(",")[6], "0")

    def test_invalid_slots_and_ids(self):
        image = view(self.rng)
        self.show_live(1, image)
        for slot in (-1, 6, 2):
            self.board.show_slot(slot, 0)
            self.assertEqual(self.board.new_events(), ["error,slot_unavailable"])
        self.board.show_slot(0, -1)
        self.assertEqual(self.board.new_events(), ["error,slot_unavailable"])
        self.assertEqual(self.board.picture(), image)
        self.assertEqual(self.board.presented_frame(), 1)
        full = packet(0, FULL, image)
        self.board.slot(6, 5, full)
        self.assertEqual(self.board.new_events(), ["slot,6,5,4"])
        self.board.slot(0, 0, full)
        self.assertEqual(self.board.new_events(), ["slot,0,0,1"])
        # A patch needs the slot's current frame as its base.
        self.board.slot(1, 7, packet(99, (0, 0, 2, 2), bytes(8)))
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,1,7,3"])
        # Base 0 must be a full frame.
        self.board.slot(1, 8, packet(0, (0, 0, 2, 2), bytes(8)))
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,1,8,3"])
        # A gap in the chunks fails the upload; the final chunk reports it.
        pieces = chunks(packet(0, FULL, view(self.rng, noise=150)))
        self.assertGreater(len(pieces), 2)
        for index, (offset, is_final, piece) in enumerate(pieces):
            if index != 1:
                self.board.slot_chunk(2, 9, 4, offset, is_final, piece, len(piece))
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,2,9,2"])
        self.assertEqual(self.board.slot_frame(2), 0)
        # A corrupt body fails and leaves the slot empty.
        self.upload(2, 20, view(self.rng))
        bad = bytearray(packet(20, (0, 0, 40, 40), self.rng.randbytes(3200)))
        bad[-3] ^= 0xFF
        self.board.slot(2, 21, bytes(bad))
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,2,21,3"])
        self.assertEqual(self.board.slot_frame(2), 0)
        self.board.show_slot(2, 0)
        self.assertEqual(self.board.new_events(), ["error,slot_unavailable"])
        self.assertEqual(self.board.picture(), image)

    def test_live_frame_interleaved_with_a_slot_upload(self):
        image = view(self.rng)
        self.show_live(1, image)
        cached = view(self.rng, noise=180)
        pieces = chunks(packet(0, FULL, cached))
        self.assertGreater(len(pieces), 4)
        frame_id = 1
        for index, (offset, is_final, piece) in enumerate(pieces):
            self.board.slot_chunk(5, 300, 4, offset, is_final, piece, len(piece))
            if index % 2 == 0:
                # A whole live patch between two slot chunks, with its own chunks interleaved.
                rect = random_rect(self.rng)
                image = paste(image, rect, self.rng.randbytes(rect[2] * rect[3] * 2))
                frame_id += 1
                self.board.live(
                    frame_id,
                    packet(frame_id - 1, rect, crop(image, rect)),
                    between=lambda: self.board.step(),
                )
                self.assert_ack(self.board.new_events(), frame_id, 0)
                self.assertEqual(self.board.picture(), image)
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,5,300,0"])
        self.assertEqual(self.board.slot_pixels(5), cached)
        self.assertEqual(self.board.picture(), image)

    def test_upload_into_the_visible_slot_keeps_the_picture(self):
        self.show_live(1, view(self.rng))
        first = view(self.rng)
        self.upload(0, 40, first)
        self.board.show_slot(0, 0)
        self.board.new_events()
        replacement = view(self.rng)
        self.upload(0, 41, replacement)
        self.assertFalse(self.board.is_slot_visible())
        self.assertEqual(self.board.picture(), first)
        self.board.show_slot(0, 1)
        self.assert_ack(self.board.new_events(), 41, 1)
        self.assertEqual(self.board.picture(), replacement)

    def test_visible_slot_upload_during_a_live_frame(self):
        self.show_live(1, view(self.rng))
        first = view(self.rng)
        self.upload(0, 40, first)
        self.board.show_slot(0, 0)
        self.board.new_events()
        # A large live patch is half received when the visible slot is rewritten.
        rect = (0, 0, 480, 300)
        image = paste(first, rect, self.rng.randbytes(480 * 300 * 2))
        live = chunks(packet(40, rect, crop(image, rect)))
        for offset, is_final, piece in live[:2]:
            self.board.frame_chunk(2, 0, 4, offset, is_final, piece, len(piece))
        replacement = view(self.rng)
        self.board.slot(0, 41, packet(0, FULL, replacement))
        self.assertEqual(self.board.picture(), first)
        for offset, is_final, piece in live[2:]:
            self.board.frame_chunk(2, 0, 4, offset, is_final, piece, len(piece))
        events = self.board.new_events()
        self.assertEqual(events[-1].split(",")[:2], ["frame", "2"])
        self.assertEqual(self.board.picture(), image)
        self.board.idle()
        self.assertEqual(self.board.new_events(), ["slot,0,41,0"])
        self.assertEqual(self.board.slot_pixels(0), replacement)

    def test_drop_slots(self):
        self.show_live(1, view(self.rng))
        cached = view(self.rng)
        self.upload(0, 70, cached)
        self.upload(1, 71, view(self.rng))
        self.board.show_slot(0, 0)
        self.board.new_events()
        self.board.drop_slots()
        self.assertEqual(self.board.picture(), cached)
        for slot in range(6):
            self.assertEqual(self.board.slot_frame(slot), 0)
            self.board.show_slot(slot, 0)
            self.assertEqual(self.board.new_events(), ["error,slot_unavailable"])
        # An unfinished upload is abandoned; its later chunks fail.
        pieces = chunks(packet(0, FULL, view(self.rng, noise=150)))
        self.board.slot_chunk(2, 80, 4, *pieces[0][:2], pieces[0][2], len(pieces[0][2]))
        self.board.drop_slots()
        for offset, is_final, piece in pieces[1:]:
            self.board.slot_chunk(2, 80, 4, offset, is_final, piece, len(piece))
        self.assertEqual(self.board.new_events(), ["slot,2,80,2"])
        # A final chunk still decoding is reported as dropped.
        self.board.slot(3, 81, packet(0, FULL, b"\x00\x00" * (PANEL * PANEL)))
        self.board.drop_slots()
        self.assertEqual(self.board.new_events(), ["slot,3,81,5"])
        self.assertEqual(self.board.picture(), cached)
        self.board.idle()
        self.assertFalse(self.board.is_slot_visible())
        self.assertEqual(self.board.picture(), cached)

    def test_random_sequences_keep_every_picture_exact(self):
        rng = self.rng
        picture = view(rng)
        next_id = presented = 1
        self.show_live(presented, picture)
        slots = {}
        for _ in range(250):
            action = rng.random()
            if action < 0.35:
                rect = FULL if rng.random() < 0.2 else random_rect(rng)
                pixels = rng.randbytes(2) * (rect[2] * rect[3])
                if rng.random() < 0.5:
                    pixels = rng.randbytes(rect[2] * rect[3] * 2)
                picture = paste(picture, rect, pixels)
                base = 0 if rect == FULL else presented
                next_id += 1
                presented = next_id
                self.show_live(presented, picture, base=base, rect=rect, codec=rng.randrange(2))
            elif action < 0.55:
                slot = rng.randrange(6)
                if slot in slots and rng.random() < 0.5:
                    rect = random_rect(rng)
                    image = paste(slots[slot][1], rect, rng.randbytes(rect[2] * rect[3] * 2))
                    base = slots[slot][0]
                else:
                    rect, image, base = FULL, view(rng, noise=rng.choice([0, 64, 160])), 0
                next_id += 1
                self.upload(
                    slot,
                    next_id,
                    image,
                    base=base,
                    rect=rect,
                    codec=rng.randrange(2),
                    between=lambda: [self.board.step() for _ in range(rng.randrange(3))],
                )
                slots[slot] = (next_id, image)
            elif action < 0.8:
                slot = rng.randrange(6)
                self.board.show_slot(slot, 3)
                if slot in slots:
                    presented, picture = slots[slot]
                    self.assert_ack(self.board.new_events(), presented, 3)
                else:
                    self.assertEqual(self.board.new_events(), ["error,slot_unavailable"])
            elif action < 0.85:
                self.board.drop_slots()
                slots = {}
            else:
                for _ in range(rng.randrange(1, 60)):
                    self.board.step()
            self.assertEqual(self.board.picture(), picture)
            self.assertEqual(self.board.presented_frame(), presented)
            for slot, (slot_frame, image) in slots.items():
                self.assertEqual(self.board.slot_frame(slot), slot_frame)
                self.assertEqual(self.board.slot_pixels(slot), image)


if __name__ == "__main__":
    unittest.main()
