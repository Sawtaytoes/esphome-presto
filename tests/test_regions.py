"""Exercise the partial-frame double buffering (presto_regions.h) without a board."""

import ctypes
import pathlib
import subprocess
import tempfile
import unittest


class FrameRegionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        root = pathlib.Path(__file__).resolve().parents[1] / "components" / "presto_display"
        wrapper = pathlib.Path(cls.scratch.name) / "wrapper.cpp"
        wrapper.write_text("""#include "presto_regions.h"
#include <random>
using namespace esphome::presto_display;
extern "C" {
// Pieces of a outside b, written as x,y,w,h quadruples. Returns the piece count.
int subtract(const int *a, const int *b, int *out) {
  Rect pieces[4];
  const int count = subtract({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, pieces);
  for (int i = 0; i < count; ++i) {
    out[4 * i] = pieces[i].x;
    out[4 * i + 1] = pieces[i].y;
    out[4 * i + 2] = pieces[i].width;
    out[4 * i + 3] = pieces[i].height;
  }
  return count;
}
// Two framebuffers driven the way PrestoFrameDisplay drives them: each frame draws random
// rectangles into the back buffer, copies the stale areas from the front one, then swaps.
// Returns the first frame whose presented picture differs from the scene, or -1.
int simulate(unsigned seed, int frames, int size, int max_rects, int scroll_every) {
  std::mt19937 rng(seed);
  std::vector<int> scene(size * size, 0), buffers[2] = {scene, scene};
  int back = 0;
  FrameRegions regions;
  for (int frame = 1; frame <= frames; ++frame) {
    const int count = 1 + int(rng() % max_rects);
    for (int i = 0; i < count; ++i) {
      Rect rect;
      if (scroll_every && frame % scroll_every) {
        // A scroll: the same band every frame, drawn in strips as LVGL flushes it.
        rect = {0, size / 4 + i * 3, size, 3};
      } else {
        rect.x = int(rng() % size);
        rect.y = int(rng() % size);
        rect.width = 1 + int(rng() % (size - rect.x));
        rect.height = 1 + int(rng() % (size - rect.y));
      }
      if (rect.y + rect.height > size) rect.height = size - rect.y;
      for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
          scene[y * size + x] = frame * 1000 + i;
          buffers[back][y * size + x] = scene[y * size + x];
        }
      regions.add(rect);
    }
    for (const Rect &rect : regions.stale())
      for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x)
          buffers[back][y * size + x] = buffers[back ^ 1][y * size + x];
    regions.presented();
    if (buffers[back] != scene) return frame;
    back ^= 1;
  }
  return -1;
}
// Stale pixel count after drawing `count` strips over one previous frame of the same strips.
long stale_after_scroll(int strips) {
  FrameRegions regions;
  for (int frame = 0; frame < 2; ++frame) {
    for (int i = 0; i < strips; ++i) regions.add({0, 40 + 60 * i, 480, 60});
    if (frame == 0) regions.presented();
  }
  long total = 0;
  for (const Rect &rect : regions.stale()) total += long(rect.width) * rect.height;
  return total + long(regions.current().size()) * 1000000;
}
}
""")
        library = pathlib.Path(cls.scratch.name) / "regions.so"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-shared",
                "-fPIC",
                "-O2",
                # presto_stream.cpp links the JPEG decoder: no Arduino headers, no SSE path.
                "-D__LINUX__",
                "-DNO_SIMD",
                "-I",
                str(root),
                str(wrapper),
                str(root / "presto_stream.cpp"),
                str(root / "presto_jpeg.cpp"),
                str(root / "JPEGDEC.cpp"),
                str(root / "miniz_tinfl.c"),
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.native = ctypes.CDLL(str(library))
        cls.native.subtract.restype = ctypes.c_int
        cls.native.simulate.restype = ctypes.c_int
        cls.native.stale_after_scroll.restype = ctypes.c_long

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def subtract(self, a, b):
        quad = ctypes.c_int * 4
        out = (ctypes.c_int * 16)()
        count = self.native.subtract(quad(*a), quad(*b), out)
        return [tuple(out[4 * i : 4 * i + 4]) for i in range(count)]

    def test_subtract_leaves_exactly_the_outside(self):
        cases = [
            ((0, 0, 10, 10), (3, 3, 4, 4)),
            ((0, 0, 10, 10), (-5, -5, 30, 30)),
            ((0, 0, 10, 10), (20, 20, 5, 5)),
            ((0, 0, 10, 10), (0, 5, 10, 10)),
            ((2, 2, 6, 6), (0, 4, 4, 1)),
            ((0, 0, 10, 10), (0, 0, 0, 5)),
        ]
        for a, b in cases:
            with self.subTest(a=a, b=b):
                pieces = self.subtract(a, b)
                covered = []
                for x, y, w, h in pieces:
                    self.assertGreater(w, 0)
                    self.assertGreater(h, 0)
                    covered += [(px, py) for py in range(y, y + h) for px in range(x, x + w)]
                self.assertEqual(len(covered), len(set(covered)), "pieces overlap")
                ax, ay, aw, ah = a
                bx, by, bw, bh = b
                expected = {
                    (px, py)
                    for py in range(ay, ay + ah)
                    for px in range(ax, ax + aw)
                    if not (bx <= px < bx + bw and by <= py < by + bh)
                }
                self.assertEqual(set(covered), expected)

    def test_every_presented_frame_matches_the_scene(self):
        for seed in range(40):
            with self.subTest(seed=seed):
                self.assertEqual(self.native.simulate(seed, 60, 24, 6, 0), -1)

    def test_scrolling_between_other_changes_stays_correct(self):
        for seed in range(20):
            with self.subTest(seed=seed):
                self.assertEqual(self.native.simulate(seed, 80, 48, 5, 7), -1)

    def test_a_steady_scroll_copies_nothing(self):
        # Seven 60-row strips merge into one rectangle, and the band it redraws every frame
        # leaves nothing stale.
        self.assertEqual(self.native.stale_after_scroll(7), 1000000)


if __name__ == "__main__":
    unittest.main()
