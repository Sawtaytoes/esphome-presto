"""Exercise the actual tap-to-wake latch without a board or ESPHome runtime."""

import ctypes
import pathlib
import subprocess
import tempfile
import unittest

DROP, WAKE, FORWARD = 0, 1, 2
PRESS, MOVE, RELEASE = 0, 1, 2


class WakeLatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        root = pathlib.Path(__file__).resolve().parents[1] / "components" / "presto_display"
        wrapper = pathlib.Path(cls.scratch.name) / "wrapper.cpp"
        wrapper.write_text("""#include "presto_wake.h"
using esphome::presto_display::WakeLatch;
extern "C" {
void *create() { return new WakeLatch; }
void destroy(void *latch) { delete static_cast<WakeLatch *>(latch); }
void set_backlight(void *latch, int percent) { static_cast<WakeLatch *>(latch)->set_backlight(percent); }
void set_wake(void *latch, int percent, int seconds) { static_cast<WakeLatch *>(latch)->set_wake(percent, seconds); }
int touch(void *latch, int phase, unsigned now) { return int(static_cast<WakeLatch *>(latch)->touch(phase, now)); }
bool expire(void *latch, unsigned now) { return static_cast<WakeLatch *>(latch)->expire(now); }
int brightness(void *latch) { return static_cast<WakeLatch *>(latch)->brightness(); }
}
""")
        library = pathlib.Path(cls.scratch.name) / "wake.so"
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
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.native = ctypes.CDLL(str(library))
        cls.native.create.restype = ctypes.c_void_p
        for name in ("destroy", "set_backlight", "set_wake", "touch", "expire", "brightness"):
            getattr(cls.native, name).argtypes = [ctypes.c_void_p] + {
                "destroy": [],
                "set_backlight": [ctypes.c_int],
                "set_wake": [ctypes.c_int, ctypes.c_int],
                "touch": [ctypes.c_int, ctypes.c_uint32],
                "expire": [ctypes.c_uint32],
                "brightness": [],
            }[name]
        cls.native.expire.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def setUp(self):
        self.latch = self.native.create()
        self.addCleanup(self.native.destroy, self.latch)

    def dark(self, wake=2, seconds=30):
        self.native.set_wake(self.latch, wake, seconds)
        self.native.set_backlight(self.latch, 0)

    def touch(self, phase, now):
        return self.native.touch(self.latch, phase, now)

    def test_touches_before_the_first_controls_are_dropped(self):
        self.native.set_wake(self.latch, 2, 30)
        self.assertEqual(self.touch(PRESS, 0), DROP)
        self.assertEqual(self.native.brightness(self.latch), 0)

    def test_a_lit_panel_forwards_every_touch(self):
        self.native.set_wake(self.latch, 2, 30)
        self.native.set_backlight(self.latch, 40)
        self.assertEqual([self.touch(p, 0) for p in (PRESS, MOVE, RELEASE)], [FORWARD] * 3)
        self.assertEqual(self.native.brightness(self.latch), 40)

    def test_a_dark_press_wakes_and_swallows_its_contact_through_release(self):
        self.dark()
        self.assertEqual(self.touch(PRESS, 1000), WAKE)
        self.assertEqual(self.native.brightness(self.latch), 2)
        self.assertEqual(self.touch(MOVE, 1100), DROP)
        self.assertEqual(self.touch(RELEASE, 1200), DROP)
        self.assertEqual(self.touch(PRESS, 2000), FORWARD)

    def test_the_wake_window_lapses_once_and_a_later_touch_restarts_it(self):
        self.dark(seconds=30)
        self.touch(PRESS, 0)
        self.touch(RELEASE, 100)
        self.assertFalse(self.native.expire(self.latch, 30099))
        self.assertEqual(self.touch(PRESS, 20000), FORWARD)
        self.assertFalse(self.native.expire(self.latch, 49999))
        self.assertTrue(self.native.expire(self.latch, 50000))
        self.assertFalse(self.native.expire(self.latch, 50001))
        self.assertEqual(self.native.brightness(self.latch), 0)
        self.assertEqual(self.touch(PRESS, 60000), WAKE)

    def test_a_held_finger_keeps_the_panel_awake(self):
        self.dark(seconds=30)
        self.touch(PRESS, 0)
        self.assertFalse(self.native.expire(self.latch, 90000))
        self.touch(RELEASE, 90000)
        self.assertFalse(self.native.expire(self.latch, 119999))
        self.assertTrue(self.native.expire(self.latch, 120000))

    def test_zero_wake_brightness_keeps_the_old_dark_behavior(self):
        self.dark(wake=0)
        self.assertEqual([self.touch(p, 0) for p in (PRESS, MOVE, RELEASE)], [DROP] * 3)
        self.assertEqual(self.native.brightness(self.latch), 0)

    def test_controls_turning_the_panel_on_end_the_wake(self):
        self.dark()
        self.touch(PRESS, 0)
        self.touch(RELEASE, 10)
        self.native.set_backlight(self.latch, 50)
        self.assertEqual(self.native.brightness(self.latch), 50)
        self.assertFalse(self.native.expire(self.latch, 60000))
        self.native.set_backlight(self.latch, 0)
        self.assertEqual(self.touch(PRESS, 60000), WAKE)

    def test_the_window_survives_the_millisecond_counter_wrapping(self):
        self.dark(seconds=30)
        self.touch(PRESS, 0xFFFFF000)
        self.touch(RELEASE, 0xFFFFF000)
        self.assertFalse(self.native.expire(self.latch, 1000))
        self.assertTrue(self.native.expire(self.latch, 0xFFFFF000 + 30000 - 2**32))


if __name__ == "__main__":
    unittest.main()
