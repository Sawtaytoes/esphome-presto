"""Drive the actual scroll window (picture upload, show and hide, the scanout's batch planner,
and the board's own drag) on the host."""

import ctypes
import random
import struct
import tempfile
import unittest
import zlib

from test_pixels import rle_encode
from test_receiver import FULL, PANEL, Board, chunks, compile_receiver, crop, packet, view

ROW = PANEL * 2
ROWS = 1200
PRESS, MOVE, RELEASE = 0, 1, 2
TOUCH_TAKEN, TOUCH_WINDOW = 1, 2
PICTURE_ROW = 100000


def scroll_packet(base, rect, pixels, codec=0):
    body = zlib.compress(pixels) if codec == 0 else rle_encode(pixels)
    return struct.pack(">IHHHHB", base, *rect, codec) + body


def tall(rng, height):
    """A list-like picture: one solid band per 40 rows, a noisy block in a few of them."""
    rows = []
    for band in range(0, height, 40):
        count = min(40, height - band)
        pixels = bytearray(rng.randbytes(2) * (PANEL * count))
        if band % 120 == 0:
            pixels[: ROW * min(count, 8)] = rng.randbytes(ROW * min(count, 8))
        rows.append(bytes(pixels))
    return b"".join(rows)


def ints(count):
    return (ctypes.c_int * count)()


class ScrollBoard(Board):
    def scroll(self, frame_id, data, between=None):
        for offset, is_final, piece in chunks(data):
            self.scroll_chunk(frame_id, 4, offset, is_final, piece, len(piece))
            if between and not is_final:
                between()

    def contact(self, phase, x, y):
        out = ints(4)
        self.touch(phase, x, y, out)
        return {"is_forwarded": bool(out[0]), "phase": out[1], "flags": out[2], "offset": out[3]}

    def window(self):
        out = ints(4)
        self.latched_window(out)
        return tuple(out)

    def glass(self):
        return ctypes.string_at(self.composite(), PANEL * PANEL * 2)

    def scroll_pixels(self, height):
        return ctypes.string_at(self.scroll_memory(), height * ROW)


class ScrollTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        cls.native = compile_receiver(cls.scratch.name)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def setUp(self):
        self.board = ScrollBoard(self.native, scroll_rows=ROWS)
        self.rng = random.Random(self._testMethodName)
        self.image = view(self.rng)
        self.board.live(1, packet(0, FULL, self.image))
        self.assertEqual(self.board.new_events()[0].split(",")[:2], ["frame", "1"])

    def tearDown(self):
        self.assertEqual(self.board.visible_writes(), 0, "a write reached a visible row")
        self.assertEqual(self.board.double_publishes(), 0, "one call published twice")
        self.assertEqual(self.board.untracked_differences(), 0, "stale tracking is wrong")
        self.board.close()

    def upload(self, frame_id, picture, base=0, rect=None, codec=0):
        rect = rect or (0, 0, PANEL, len(picture) // ROW)
        x, y, width, height = rect
        pixels = b"".join(
            picture[(y + row) * ROW + x * 2 : (y + row) * ROW + (x + width) * 2]
            for row in range(height)
        )
        self.board.scroll(frame_id, scroll_packet(base, rect, pixels, codec))
        self.board.idle()
        return self.board.new_slot_events()

    def show(self, frame_id, top, bottom, offset):
        self.board.scroll_show(frame_id, top, bottom, offset)
        self.board.idle()
        events = self.board.new_slot_events()
        self.board.frame_start()
        return events

    def expected_glass(self, picture, top, bottom, offset):
        glass = bytearray(self.board.picture())
        glass[top * ROW : bottom * ROW] = picture[offset * ROW : (offset + bottom - top) * ROW]
        return bytes(glass)

    def shown_picture(self, height=900, frame_id=50, top=64, bottom=416, offset=0):
        picture = tall(self.rng, height)
        self.assertEqual(self.upload(frame_id, picture), [f"scroll,{frame_id},0"])
        self.assertEqual(
            self.show(frame_id, top, bottom, offset),
            [f"scroll_window,{frame_id},0,{top},{bottom},{offset}"],
        )
        return picture

    # Packing and the scanout's batch planner

    def test_a_window_survives_packing(self):
        out = ints(4)
        for window in [(1, 64, 416, 0), (1, 0, 480, 3615), (1, 8, 16, 131071), (0, 0, 0, 0)]:
            self.native.round_trip_window(*window, out)
            self.assertEqual(tuple(out), window)
        # A hidden window packs to nothing, whatever its fields say.
        self.native.round_trip_window(False, 64, 416, 9, out)
        self.assertEqual(tuple(out), (0, 0, 0, 0))

    def test_batches_take_the_window_from_the_next_frame_start(self):
        latch = self.native.latch_create()
        try:
            rows = list(range(0, PANEL, 8))
            self.assertEqual([self.native.plan_row(latch, row, True) for row in rows], rows)
            self.native.latch_request(latch, True, 64, 416, 300)
            # The rest of this frame still reads the framebuffer.
            self.assertEqual(
                [self.native.plan_row(latch, row, True) for row in rows[20:]], rows[20:]
            )
            frame = [self.native.plan_row(latch, row, True) for row in rows]
            self.assertEqual(
                frame,
                [row if row < 64 or row >= 416 else PICTURE_ROW + row - 64 + 300 for row in rows],
            )
            # A change mid-frame waits for the next frame, so one frame never mixes windows.
            self.native.latch_request(latch, True, 64, 416, 10)
            self.assertEqual(
                [self.native.plan_row(latch, row, True) for row in rows[1:]], frame[1:]
            )
            self.assertEqual(self.native.plan_row(latch, 64, True), PICTURE_ROW + 300)
            self.assertEqual(self.native.plan_row(latch, 0, True), 0)
            self.assertEqual(self.native.plan_row(latch, 64, True), PICTURE_ROW + 10)
            self.assertEqual(self.native.plan_row(latch, 408, True), PICTURE_ROW + 354)
            # A frame swap carries its window at once.
            self.native.latch_request(latch, False, 0, 0, 0)
            count = self.native.latch_count(latch)
            self.native.latch_publish_requested(latch)
            self.assertEqual(self.native.latch_count(latch), count + 1)
            self.assertEqual(self.native.plan_row(latch, 64, True), 64)
            # Without a picture the window maps nothing.
            self.native.latch_request(latch, True, 64, 416, 10)
            self.native.plan_row(latch, 0, False)
            self.assertEqual(self.native.plan_row(latch, 64, False), 64)
        finally:
            self.native.latch_destroy(latch)

    # Upload, show, and hide

    def test_upload_show_scroll_and_hide(self):
        self.assertEqual(self.board.codecs(), f"zlib,rle,jpeg,scroll={ROWS}".encode())
        picture = tall(self.rng, 900)
        before = self.board.picture()
        self.assertEqual(self.upload(7, picture), ["scroll,7,0"])
        self.assertEqual(self.board.scroll_pixels(900), picture)
        self.assertEqual((self.board.scroll_frame(), self.board.scroll_height()), (7, 900))
        self.assertEqual(self.board.glass(), before, "an upload never changes the glass")
        self.assertTrue(any(line.startswith("scroll_timing,7,") for line in self.board.timings()))
        self.assertEqual(self.show(7, 64, 416, 120), ["scroll_window,7,0,64,416,120"])
        self.assertEqual(self.board.window(), (1, 64, 416, 120))
        self.assertEqual(self.board.glass(), self.expected_glass(picture, 64, 416, 120))
        # The framebuffers are untouched: the window is a scanout mapping, not a copy.
        self.assertEqual(self.board.picture(), before)
        # The offset is clamped to the picture.
        self.assertEqual(self.show(7, 64, 416, 5000), ["scroll_window,7,0,64,416,548"])
        self.assertEqual(self.board.glass(), self.expected_glass(picture, 64, 416, 548))
        self.board.scroll_hide()
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])
        self.board.frame_start()
        self.assertEqual(self.board.glass(), before)
        # A hide is always answered.
        self.board.scroll_hide()
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])
        self.assertEqual(self.board.new_events(), [], "nothing reached Display Events")

    def test_the_window_follows_live_frames_underneath(self):
        picture = self.shown_picture(offset=40)
        rect = (0, 0, PANEL, 48)
        image = bytes(self.rng.randbytes(2)) * (PANEL * 48) + self.image[48 * ROW :]
        self.board.live(2, packet(1, rect, crop(image, rect)))
        self.board.new_events()
        self.assertEqual(self.board.window(), (1, 64, 416, 40))
        self.assertEqual(self.board.picture(), image)
        self.assertEqual(self.board.glass(), self.expected_glass(picture, 64, 416, 40))
        self.assertEqual(self.board.new_slot_events(), [])

    def test_refused_shows_leave_the_window_alone(self):
        self.shown_picture(height=400, frame_id=9, top=64, bottom=384, offset=4)
        cases = [
            ((8, 64, 384, 0), "scroll_window,8,4,0,0,0"),  # not the picture's frame
            ((9, 60, 384, 0), "scroll_window,9,3,0,0,0"),  # top off the 8-row grid
            ((9, 64, 380, 0), "scroll_window,9,3,0,0,0"),  # bottom off the grid
            ((9, 0, 480, 0), "scroll_window,9,3,0,0,0"),  # taller than the picture
            ((9, 64, 64, 0), "scroll_window,9,3,0,0,0"),  # empty
            ((9, 64, 488, 0), "scroll_window,9,3,0,0,0"),  # past the panel
            ((0, 64, 384, 0), "scroll_window,0,4,0,0,0"),
        ]
        for arguments, event in cases:
            with self.subTest(arguments=arguments):
                self.assertEqual(self.show(*arguments), [event])
                self.assertEqual(self.board.window(), (1, 64, 384, 4))

    def test_invalid_uploads(self):
        picture = tall(self.rng, 600)
        self.assertEqual(self.upload(3, picture), ["scroll,3,0"])
        rect = (10, 20, 30, 40)
        cases = [
            # A patch against a frame the picture does not hold.
            (4, scroll_packet(2, rect, bytes(30 * 40 * 2)), "scroll,4,3"),
            # A full picture taller than the memory.
            (5, scroll_packet(0, (0, 0, PANEL, ROWS + 8), bytes(ROW)), "scroll,5,3"),
            # A full picture must be the whole width.
            (6, scroll_packet(0, (0, 0, 240, 100), bytes(240 * 200)), "scroll,6,3"),
            # A JPEG decodes in one piece, so the picture refuses it.
            (7, struct.pack(">IHHHHB", 0, 0, 0, PANEL, 16, 2) + b"\xff\xd8", "scroll,7,3"),
            # A truncated body.
            (8, scroll_packet(0, (0, 0, PANEL, 100), picture[: 100 * ROW])[:-5], "scroll,8,3"),
        ]
        for frame_id, data, event in cases:
            with self.subTest(event=event):
                self.board.scroll(frame_id, data)
                self.board.idle()
                self.assertEqual(self.board.new_slot_events(), [event])
                self.assertEqual(self.board.scroll_frame(), 0)
        # Out of order and bad chunks.
        self.board.scroll_chunk(9, 4, 4000, True, b"AAAA", 4)
        self.board.scroll_chunk(9, 5, 0, True, b"AAAA", 4)
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll,9,2", "scroll,9,1"])
        self.assertEqual(self.board.new_events(), [])

    def test_a_patch_rewrites_part_of_the_picture(self):
        picture = tall(self.rng, 700)
        self.assertEqual(self.upload(3, picture), ["scroll,3,0"])
        rect = (0, 610, PANEL, 90)
        changed = bytearray(picture)
        changed[610 * ROW : 700 * ROW] = self.rng.randbytes(90 * ROW)
        self.assertEqual(self.upload(4, bytes(changed), base=3, rect=rect, codec=1), ["scroll,4,0"])
        self.assertEqual(self.board.scroll_pixels(700), bytes(changed))
        self.assertEqual(self.board.scroll_height(), 700)
        # A patch below the picture's height is refused.
        self.board.scroll(5, scroll_packet(4, (0, 690, PANEL, 20), bytes(20 * ROW)))
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll,5,3"])

    def test_an_upload_hides_the_window_and_waits_before_writing(self):
        self.shown_picture()
        releases = self.board.releases()
        picture = tall(self.rng, 500)
        events = self.upload(60, picture)
        self.assertEqual(events, ["scroll_window,0,0,0,0,0", "scroll,60,0"])
        self.assertEqual(self.board.releases(), releases + 1)
        self.assertEqual(self.board.window(), (0, 0, 0, 0))
        # The window waits for a show of the new picture.
        self.assertEqual(self.show(50, 64, 416, 0), ["scroll_window,50,4,0,0,0"])
        self.assertEqual(self.show(60, 64, 416, 0), ["scroll_window,60,0,64,416,0"])
        self.assertEqual(self.board.glass(), self.expected_glass(picture, 64, 416, 0))

    def test_an_upload_after_a_hide_that_was_swapped_past_does_not_wait(self):
        self.shown_picture()
        self.board.slot(0, 70, packet(0, FULL, self.image))
        self.board.idle()
        self.board.new_slot_events()
        self.board.show_slot(0, 0)
        self.board.new_events()
        releases = self.board.releases()
        self.upload(71, tall(self.rng, 500))
        self.assertEqual(self.board.releases(), releases)

    def test_a_slot_a_full_frame_a_drop_and_the_offline_blank_hide_the_window(self):
        self.shown_picture()
        # A partial live patch keeps the window.
        rect = (0, 0, 8, 8)
        self.board.live(2, packet(1, rect, bytes(128)))
        self.board.new_events()
        self.assertEqual(self.board.window()[0], 1)
        # A full live frame takes it away with its swap.
        self.board.live(3, packet(0, FULL, self.image))
        self.board.new_events()
        self.assertEqual(self.board.window(), (0, 0, 0, 0))
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])
        # A slot show does too.
        self.assertEqual(self.show(50, 64, 416, 0), ["scroll_window,50,0,64,416,0"])
        self.board.slot(1, 80, packet(0, FULL, view(self.rng)))
        self.board.idle()
        self.board.new_slot_events()
        self.board.show_slot(1, 0)
        self.assertEqual(self.board.new_events()[0].split(",")[:2], ["frame", "80"])
        self.assertEqual(self.board.window(), (0, 0, 0, 0))
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])
        # drop_slots empties the picture too.
        self.assertEqual(self.show(50, 64, 416, 0), ["scroll_window,50,0,64,416,0"])
        self.board.drop_slots()
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])
        self.assertEqual(self.board.scroll_frame(), 0)
        self.assertEqual(self.show(50, 64, 416, 0), ["scroll_window,50,4,0,0,0"])
        # The offline blank replaces the whole glass.
        self.assertEqual(self.upload(51, tall(self.rng, 600)), ["scroll,51,0"])
        self.assertEqual(self.show(51, 64, 416, 0), ["scroll_window,51,0,64,416,0"])
        self.board.advance(7100)
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])
        self.assertEqual(self.board.window(), (0, 0, 0, 0))
        self.assertEqual(self.board.glass()[:2], b"\xf8\x00")

    def test_a_slot_upload_abandons_an_unfinished_picture(self):
        data = scroll_packet(0, (0, 0, PANEL, 900), self.rng.randbytes(900 * ROW))
        pieces = chunks(data)
        offset, is_final, piece = pieces[0]
        self.board.scroll_chunk(5, 4, offset, is_final, piece, len(piece))
        self.board.slot(2, 6, packet(0, FULL, self.image))
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["slot,2,6,0"])
        self.assertEqual(self.board.scroll_frame(), 0)
        offset, is_final, piece = pieces[1]
        self.board.scroll_chunk(5, 4, offset, is_final, piece, len(piece))
        self.assertEqual(self.board.scroll_frame(), 0)

    def test_a_board_without_the_picture(self):
        board = ScrollBoard(self.native, scroll_rows=0)
        try:
            self.assertEqual(board.codecs(), b"zlib,rle,jpeg")
            board.live(1, packet(0, FULL, self.image))
            board.scroll(3, scroll_packet(0, (0, 0, PANEL, 100), bytes(100 * ROW)))
            board.scroll_show(3, 64, 416, 0)
            board.idle()
            self.assertEqual(board.new_slot_events(), ["scroll,3,4", "scroll_window,3,4,0,0,0"])
            self.assertEqual(board.contact(PRESS, 100, 100)["flags"], 0)
            self.assertTrue(board.contact(MOVE, 100, 160)["is_forwarded"])
            self.assertEqual(board.visible_writes(), 0)
        finally:
            board.close()

    # The board's own drag

    def test_a_tap_on_the_window_reaches_castkit(self):
        self.shown_picture(offset=30)
        self.assertEqual(
            self.board.contact(PRESS, 200, 200),
            {"is_forwarded": True, "phase": PRESS, "flags": TOUCH_WINDOW, "offset": 30},
        )
        self.assertTrue(self.board.contact(MOVE, 203, 207)["is_forwarded"])
        self.assertEqual(self.board.contact(RELEASE, 203, 207)["phase"], RELEASE)
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), [])
        self.assertEqual(self.board.window(), (1, 64, 416, 30))

    def test_a_vertical_drag_is_taken_over_and_reported_at_rest(self):
        picture = self.shown_picture(offset=100)
        self.board.contact(PRESS, 200, 300)
        self.board.advance(20)
        self.assertTrue(self.board.contact(MOVE, 201, 292)["is_forwarded"])
        self.board.advance(20)
        # 12 rows up: the board takes the contact, and the picture follows from the press.
        self.assertEqual(
            self.board.contact(MOVE, 202, 288),
            {
                "is_forwarded": True,
                "phase": RELEASE,
                "flags": TOUCH_WINDOW | TOUCH_TAKEN,
                "offset": 112,
            },
        )
        requests = self.board.window_requests()
        for y in (260, 220, 180):
            self.board.advance(20)
            self.assertFalse(self.board.contact(MOVE, 202, y)["is_forwarded"])
        self.assertEqual(self.board.window_requests(), requests + 3)
        self.board.frame_start()
        self.assertEqual(self.board.window(), (1, 64, 416, 220))
        self.assertEqual(self.board.glass(), self.expected_glass(picture, 64, 416, 220))
        # Resting before the release: no glide.
        self.board.advance(200)
        self.assertFalse(self.board.contact(RELEASE, 0, 0)["is_forwarded"])
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_at,50,220"])
        self.assertEqual(self.board.new_events(), [])
        # The next press carries the offset, so CastKit can match it before a tap.
        self.assertEqual(self.board.contact(PRESS, 10, 10)["offset"], 220)

    def test_the_drag_stops_at_both_ends(self):
        self.shown_picture(height=600, offset=10)
        self.board.contact(PRESS, 100, 100)
        self.board.contact(MOVE, 100, 200)
        self.board.frame_start()
        self.assertEqual(self.board.window()[3], 0)
        self.board.contact(MOVE, 100, 0)
        self.board.contact(MOVE, 100, -400)
        self.board.frame_start()
        self.assertEqual(self.board.window()[3], 600 - 352)
        self.board.advance(300)
        self.board.contact(RELEASE, 0, 0)
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_at,50,248"])

    def test_sideways_drags_and_presses_outside_stay_with_castkit(self):
        self.shown_picture()
        self.board.contact(PRESS, 100, 200)
        self.assertTrue(self.board.contact(MOVE, 116, 204)["is_forwarded"])
        # Once sideways, always CastKit's, even when it turns vertical.
        for y in (240, 300):
            self.assertEqual(self.board.contact(MOVE, 116, y)["flags"], TOUCH_WINDOW)
        self.assertEqual(self.board.contact(RELEASE, 0, 0)["phase"], RELEASE)
        for y in (30, 440):
            self.board.contact(PRESS, 100, y)
            self.assertTrue(self.board.contact(MOVE, 100, y + 30)["is_forwarded"])
            self.board.contact(RELEASE, 0, 0)
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), [])
        self.assertEqual(self.board.window()[3], 0)

    def fling(self, start=300, step=-40):
        self.board.contact(PRESS, 200, start)
        y = start
        for _ in range(5):
            self.board.advance(16)
            y += step
            self.board.contact(MOVE, 200, y)
        self.board.advance(16)
        self.assertFalse(self.board.contact(RELEASE, 0, 0)["is_forwarded"])

    def offset(self):
        self.board.frame_start()
        return self.board.window()[3]

    def test_a_fling_glides_to_rest(self):
        self.shown_picture(height=ROWS, offset=0)
        self.fling(step=-10)
        released = self.offset()
        offsets = []
        for _ in range(200):
            self.board.advance(16)
            self.board.step()
            offsets.append(self.offset())
            if self.board.slot_event_count() > self.board.slot_seen:
                break
        self.assertGreater(offsets[-1], released + 100, offsets)
        self.assertLess(offsets[-1], ROWS - 352, "the glide stopped by itself")
        self.assertEqual(offsets, sorted(offsets), "a glide never turns back")
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), [f"scroll_at,50,{offsets[-1]}"])

    def test_a_press_during_a_glide_stops_it_and_is_never_a_tap(self):
        self.shown_picture(height=ROWS)
        self.fling()
        for _ in range(3):
            self.board.advance(16)
            self.board.step()
        stopped = self.offset()
        self.assertFalse(self.board.contact(PRESS, 200, 200)["is_forwarded"])
        for _ in range(5):
            self.board.advance(16)
            self.board.step()
        self.assertEqual(self.offset(), stopped)
        self.assertFalse(self.board.contact(RELEASE, 0, 0)["is_forwarded"])
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), [f"scroll_at,50,{stopped}"])

    def test_a_press_outside_the_window_during_a_glide_is_castkits(self):
        self.shown_picture(height=ROWS)
        self.fling()
        for _ in range(3):
            self.board.advance(16)
            self.board.step()
        stopped = self.offset()
        # A tap on the header above the window: forwarded, and the glide rests where it is.
        press = self.board.contact(PRESS, 200, 30)
        self.assertTrue(press["is_forwarded"])
        self.assertEqual((press["phase"], press["flags"]), (PRESS, TOUCH_WINDOW))
        self.assertEqual(press["offset"], stopped)
        # A vertical move from there never scrolls the list.
        self.assertTrue(self.board.contact(MOVE, 200, 80)["is_forwarded"])
        for _ in range(5):
            self.board.advance(16)
            self.board.step()
        self.assertEqual(self.offset(), stopped)
        self.assertTrue(self.board.contact(RELEASE, 0, 0)["is_forwarded"])
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), [f"scroll_at,50,{stopped}"])

    def test_a_glide_stops_at_the_end(self):
        self.shown_picture(height=500, offset=0)
        self.fling()
        for _ in range(100):
            self.board.advance(16)
            self.board.step()
        self.board.frame_start()
        self.assertEqual(self.board.window()[3], 500 - 352)
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_at,50,148"])

    def test_a_hide_mid_drag_ends_it_without_a_report(self):
        self.shown_picture()
        self.board.contact(PRESS, 200, 300)
        self.assertEqual(self.board.contact(MOVE, 200, 250)["flags"], TOUCH_WINDOW | TOUCH_TAKEN)
        self.board.scroll_hide()
        # The rest of the contact goes to CastKit, which cancelled it and ignores it.
        self.assertTrue(self.board.contact(MOVE, 200, 200)["is_forwarded"])
        self.assertTrue(self.board.contact(RELEASE, 0, 0)["is_forwarded"])
        self.board.idle()
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0"])

    def test_dragging_during_an_upload_never_shows_the_picture(self):
        self.shown_picture()
        self.board.contact(PRESS, 200, 300)
        self.board.contact(MOVE, 200, 250)
        moves = iter(range(240, 0, -4))
        picture = tall(self.rng, 900)
        self.board.scroll(
            61,
            scroll_packet(0, (0, 0, PANEL, 900), picture),
            between=lambda: self.board.contact(MOVE, 200, next(moves)),
        )
        self.board.idle()
        self.assertEqual(self.board.window(), (0, 0, 0, 0))
        self.assertEqual(self.board.new_slot_events(), ["scroll_window,0,0,0,0,0", "scroll,61,0"])
        self.assertEqual(self.board.scroll_pixels(900), picture)


if __name__ == "__main__":
    unittest.main()
