# ESPHome for Pimoroni Presto

An external ESPHome component for the Pimoroni Presto: RP2350, ST7701 display,
capacitive touch, backlight and seven rear RGB LEDs. The board uses ESPHome's
native Wi-Fi, encrypted API and OTA updates. Tested with ESPHome 2026.9.1.

The display uses a native **480×480 RGB565 canvas** and PSRAM framebuffers.
The paced XIP streaming FIFO fetches rows ahead into SRAM buffers; pixel and
timing DMA read only SRAM. Row addresses are supplied by a SRAM DMA table, and frame swaps occur at vertical blank. Direct PSRAM
DMA reads are avoided because they can stall unrelated time-critical channels.

**Experimental:** the current revision was physically confirmed sharp and steady
with changing music artwork at native resolution. Static color patterns and touch
during scanout also passed. Encrypted frame transfer and wireless OTA are verified.
Gesture latency is still being tuned in CastKit's browser worker; this is not a
claim that every application or workload has been validated. The streaming
receiver and frame slots compile for RP2350 with ESPHome 2026.10.0 and pass the
host tests, but have not yet been measured on a panel. The same is true of JPEG patches
(codec 2), which compile with ESPHome 2026.9.1 and pass the host tests, and of the scroll
window.

## Try the hardware

Clone the repository, then:

```sh
esphome compile examples/hardware-test.yaml
```

The test has color bars and horizontal markers so vertical timing faults are
visible. It logs touch coordinates and exposes backlight and ambient-light
controls. Use `examples/display-test.yaml` for a display-only configuration.
The first installation needs USB boot mode; a network-enabled installation can
then use OTA.

For another ESPHome configuration, use this external source:

```yaml
external_components:
  - source: github://Sawtaytoes/esphome-presto@main
    components: [presto_display]
display:
  - platform: presto_display
    id: presto_lcd
    lambda: |-
      it.fill(Color(0, 0, 0));
      it.filled_rectangle(20, 20, 200, 200, Color(255, 255, 255));
```

Copy the `rp2` and flash-size settings from an example. The driver reserves PIO1
and display DMA channels; the rear lights use PIO2. Touch owns I2C1 on GPIO30/31,
with IRQ on GPIO32 and address 0x48. Do not add a second owner of that bus.
Native ESPHome touch coordinates are in the 480×480 logical canvas.

## CastKit receiver

`examples/castkit.yaml` supplies the encrypted API actions for CastKit's remote
browser worker. Create a private `examples/secrets.yaml` with `wifi_ssid`,
`wifi_password` and a unique base64 `api_encryption_key`; it is ignored by Git.
OTA inherits that encryption key and requires encrypted uploads.

Configure the CastKit worker with `transport: esphome-presto`, the actual board
host and MAC, its display manifest URL, and the matching encryption key in a
private secrets file. See [CastKit's remote display guide](https://github.com/Sawtaytoes/castkit/blob/master/docs/remote-display.md).
Keep **`api.batch_delay: 0ms`**: acknowledgements and touch contacts must not be
coalesced. The firmware's light entities are internal in this example because
CastKit supplies the user controls; the hardware example exposes native lights.

Arduino gives the display core its own stack, separate from network/decode work.
Touch polling continues during every PSRAM copy, and each copy waits for the
scanout's row prefetch, writes at most 960 bytes, then leaves the PSRAM bus idle
for 50 µs (`PRESTO_WRITE_BATCH`, `PRESTO_PACE_US`). That pacing is what made
native-resolution artwork steady; change it only with on-panel measurements.

### Live frames

The receiver validates each bounded base64 chunk, its order, and the patch's
rectangle and acknowledged base. It inflates every chunk as it arrives straight
into the back framebuffer, through a 32 KiB SRAM dictionary allocated once, so
there is no PSRAM staging copy. The visible buffer is never written: incomplete,
out-of-order, corrupt, or oversized frames are rejected with `error,<reason>` and
the picture does not change. Only a complete frame is presented, at vertical
blank, and acknowledged with `frame,<frame>,<touch>,0,<overruns>,<decode
µs>,<draw µs>`. Unchanged heartbeat pixels acknowledge without a swap. Lost
connections recover with a full frame. After seven seconds without a presented
frame, the board disables touch, accepts only a full frame, and shows an offline
marker. A full frame that is still arriving then keeps going and the marker waits
for it; the marker appears only if that frame fails. A live frame that receives no
chunk for three seconds is abandoned, so a sender that stops part way never holds
back the offline marker or the idle work below.

**Wire format.** A packet is a big-endian `>IHHHHB` header (base frame id, x, y,
width, height, codec) and a body, base64-encoded and sent in chunks of at most 4,000
characters. The codec is `0` for zlib or `1` for CastKit RLE of big-endian RGB565 rows,
or `2` for a baseline JPEG. A base of `0` needs the full 480×480 rectangle; any other base
must be the frame on screen.

### JPEG patches (codec 2)

A camera picture compresses about five times smaller as a JPEG than as zlib, so a
codec 2 patch is the one to use for camera frames. It travels through `frame_chunk`
exactly like codecs 0 and 1, with the same base and rectangle checks and the same
acknowledgement, and its pixels replace exactly the header's rectangle in the back
buffer. The rules:

- One baseline scan of 8-bit YCbCr, with chroma 4:4:4, 4:2:2, or 4:2:0. Restart markers,
  application segments, and comments are accepted. Progressive, arithmetic, lossless,
  grayscale, and multi-scan files are not.
- The JPEG's width and height must equal the rectangle's.
- The compressed body is at most 131,072 bytes (128 KiB). A q75 480×360 camera frame is
  about 30 KB.
- Live frames only. A `slot_chunk` with codec 2 reports status `3`.

The body is collected in a 128 KiB PSRAM buffer allocated once at boot, through the same
paced writer as every other PSRAM write, and read back behind the prefetch gate. On the
final chunk the receiver checks it marker by marker, then decodes it with JPEGDEC
(about 18 KB of SRAM state, allocated once), which hands back blocks of big-endian RGB565
that are clipped to the rectangle. JPEGDEC's blocks are at most 2048 pixels (a 4:2:0 MCU row
arrives as 128-pixel strips), and every paced write pays the prefetch gate and the pause
however short it is, so one MCU row of the rectangle is gathered in a 15 KB SRAM band first
and written as whole rows (one contiguous run for a full-width patch). A 480x360 camera patch
went from 1572 write calls to 155, and its PSRAM write time on a board from about 161 ms. The
decode runs in one piece inside the final chunk's action; touch polling continues during its
paced writes. The `<decode µs>` of the acknowledgement includes it. JPEGDEC repeats each
subsampled chroma sample over its block instead of interpolating, so a 4:2:0 color edge is
slightly blockier than in a desktop decoder; luma is unaffected.

A rejected JPEG answers one of these, and the picture does not change:

| Event | Cause |
| --- | --- |
| `error,jpeg_too_large` | The body passed 131,072 bytes. Sent as soon as it does, before the final chunk. |
| `error,jpeg_size_mismatch` | The image is not exactly the rectangle's width and height. |
| `error,jpeg_unsupported` | Progressive, arithmetic, lossless, 12-bit, grayscale, multi-scan, or another chroma layout. |
| `error,jpeg_invalid` | Malformed or truncated, no end-of-image marker at the end, bytes after it, missing tables, or a decoder failure. |
| `error,jpeg_unavailable` | The board could not allocate the JPEG memory at boot. |

As with every failed frame, later chunks of the same frame then answer
`error,chunk_order_or_size`.

The optional `codecs_id` text sensor ("Display Codecs" in the example) holds the live
codecs this firmware decodes: `zlib,rle,jpeg`, or `zlib,rle` when the JPEG memory could
not be allocated. CastKit sends JPEG only when that entity exists and lists `jpeg`, so a
board with older firmware keeps getting zlib.

After a swap, the new back framebuffer differs from the picture only where the
last frame drew. Idle time copies that area from the visible framebuffer in small
paced steps, starting 150 ms after the last live chunk, so a continuous stream is
not slowed and a later small patch (a clock tick) copies nothing. A patch that
arrives before the copy is done copies only the area that is still stale and that
the patch does not cover. Idle work stops whenever a live frame is arriving.

### Frame slots

Six slots (`0`–`5`) each hold a full 480×480 frame in PSRAM, so CastKit can send
the likely next views ahead of time and show one instantly on a swipe:

- `slot_chunk(slot, frame_id, format, offset, final_chunk, image_data)` uses the
  same packet and chunking as `frame_chunk`. The packet's base must be `0` with a
  full 480×480 rectangle, or the slot's current frame id (a patch against the
  slot). After the final chunk is decoded the board reports
  `slot,<slot>,<frame_id>,<status>` on Display Slot Events: `0` is success; `1` a bad chunk or base64,
  `2` a missing or out-of-order chunk, `3` a bad header, base, or body, `4` no
  such slot, and `5` dropped while decoding. Any nonzero status leaves the slot
  empty. An upload never changes the picture, decodes in bounded pieces between
  live frames, and keeps its place when a live frame arrives in the middle of it.
  One upload runs at a time; starting another abandons the unfinished one.
- `show_slot(slot, touch_id)` scans the slot out from the next vertical blank
  (a pointer swap, no copy) and acknowledges like a live frame, with the slot's
  frame id: `frame,<slot frame>,<touch>,0,<overruns>,0,<draw µs>`. The next live
  patch uses that frame as its base. An empty, unfinished, or unknown slot answers
  `error,slot_unavailable` and the picture does not change. A slot keeps its
  content after it is shown, so swiping back and forth shows it again.
- `drop_slots()` empties every slot. CastKit calls it when it connects, so a slot
  from an earlier session is never shown.

After a slot is shown, idle time copies it into both framebuffers in bounded
pieces, then moves the scanout back onto a framebuffer holding the same picture.
A live patch that arrives first still works: it copies only what it needs. An
upload into the slot that is on screen first moves the scanout off it.

### Scroll window

A list that scrolls is drawn ahead by CastKit as one tall picture, and a drag on it moves
the scanout over that picture on the board alone: no capture, no transfer, and no PSRAM
writes while the finger moves
([decision](docs/decisions/2026-10-26-touch-scrolling-moves-the-scanout-over-a-picture-drawn-ahead.md)).

- `scroll_chunk(frame_id, format, offset, final_chunk, image_data)` uploads the picture with
  the same packet and chunking as `slot_chunk`, zlib or RLE only (a JPEG reports `3`). The
  packet's base is `0` with `x` 0, `y` 0, width 480 and any height up to the capacity, or the
  picture's current frame id with a rectangle inside its height (a patch). Rows are 480
  pixels apart. The board reports `scroll,<frame_id>,<status>` on Display Slot Events, with
  the slot status codes. An upload never changes the picture on the glass, but it first hides
  the window and waits two frames, so nothing writes into rows the scanout may read.
- `scroll_show(frame_id, top, bottom, scroll_y)` scans panel rows `[top, bottom)` out of the
  picture, from its row `scroll_y` (clamped to the picture), from the next frame. `top` and
  `bottom` are multiples of 8, and the window is no taller than the picture. The board
  reports `scroll_window,<frame_id>,0,<top>,<bottom>,<scroll_y>`, or
  `scroll_window,<frame_id>,<status>,0,0,0` when it refuses (`4` no such picture, `3` bad
  rows) and nothing changes.
- `scroll_hide()` scans every row from the framebuffer again and reports
  `scroll_window,0,0,0,0,0`. The window also goes away, with the same report, on a slot
  show, a full live frame (base `0`), the offline blank, `drop_slots`, and any new upload.
  `drop_slots` empties the picture too.

The framebuffers are not touched: live patches keep updating them under the window, and
the next live frame's base is unchanged.

**On-board drag.** A contact that starts inside the window is reported as usual, so a tap
still reaches the page. When it has moved 12 rows vertically, and more vertically than
sideways, the board takes it over: that report goes out as a release with flag `1`, which
CastKit treats as a cancel, and nothing more of the contact is reported. The window then
follows the finger from the press, clamped to the picture. A sideways drag stays CastKit's.
A quick release glides on and slows with friction; a press on the window during the glide
stops it and is never a tap, and a press outside the window ends the glide and goes to CastKit. When the picture comes to rest the board reports `scroll_at,<frame_id>,<scroll_y>`
on Display Slot Events, and CastKit scrolls its page to match.

A touch report is `touch,<sequence>,<phase>,<x>,<y>,<flags>,<frame>,<offset>`. Flag `1` is a
contact the board took over; flag `2` means a window is shown and `<offset>` is its row in the
picture. Without a window both fields are `0`, as before.

**Scanout.** The prefetch fetches 8 rows per batch; a batch inside the window reads
`picture + 480 * (row - top + scroll_y)`. The window is one packed 32-bit word that the
display core latches at the first batch of each frame (row 0), so one frame never mixes two
windows, and a framebuffer swap carries the window its first rows were copied with. The
batch planner and the drag are `presto_scroll.h`, tested on the host by `tests/test_scroll.py`.

**Events and entities.** ESPHome sends only the newest state of a text sensor that
it has not sent yet, so when the link is slow an earlier state on the same entity
is lost. The receiver therefore keeps each kind of report on its own entity, and
no single action or loop step publishes two states on one entity:

| Entity (example name) | Option | States |
| --- | --- | --- |
| Display Events | `events_id` (required) | `frame,…` and `error,…` acknowledgements, `touch,…`, `wake,…`, `wake_controls,…`, `controls,…` |
| Display Slot Events | `slot_events_id` | `slot,<slot>,<frame_id>,<status>`, `scroll,…`, `scroll_window,…`, and `scroll_at,…`, at most one per loop step |
| Display Timing | `timing_id` | `timing,…`, `slot_timing,…`, `scroll_timing,…`, `settled,…` |
| Display Codecs | `codecs_id` | `zlib,rle,jpeg,scroll=<rows>`, published once at boot |

Without `slot_events_id`, slot uploads still work but report nothing, and CastKit
needs those reports to use slots. Touch reads continue during long PSRAM copies
inside an action, so a `touch,…` can still share a call with that action's
acknowledgement on Display Events; the acknowledgement is published last.

The slots take 2,764,800 bytes of the 8 MiB PSRAM, beside the two framebuffers
(921,600 bytes) and the JPEG buffer (131,072 bytes). The scroll picture is allocated after
them, once, with as many 960-byte rows as fit while 1 MiB of PSRAM stays free, at most 4,096
and none below 480 (about 3,600 rows today). `Display Codecs` lists it as `scroll=<rows>`;
without it the board lists no `scroll=` and refuses every scroll upload and show with status
`4`. CastKit detects slot support by the presence of the `show_slot` action and scroll
support by `scroll_show` and `scroll=`, so an older CastKit keeps working with this firmware.

The optional `timing_id` text sensor ("Display Timing" in the example) reports
`timing,<frame>,<rect bytes>,<decode µs>,<PSRAM write µs>,<sync µs>,<sync
bytes>,<present µs>,<first to final chunk ms>,<free PSRAM bytes>` for each live
frame, `slot_timing,<slot>,<frame>,<rect bytes>,<decode µs>,<PSRAM write µs>,<first
chunk to decoded ms>,<free PSRAM bytes>` for each upload, and `settled,<bytes>,<µs>`
when the framebuffers match the picture again after a slot was shown.

The receiver's frame logic (`presto_receiver.cpp`, `presto_stream.cpp`,
`presto_jpeg.cpp`, `presto_scroll.h`) has no ESPHome or Pico types; `tests/test_receiver.py`,
`tests/test_scroll.py`, `tests/test_jpeg.py`, and `tests/test_pixels.py` compile and drive
that exact code, with the vendored miniz and JPEGDEC, on the host.

At zero backlight brightness the panel is dark but still listens for a press.
The first press lights it at the wake level CastKit sends through
`set_wake_controls` and is swallowed through its release, so it never presses
what is under the finger. Later touches reach the page normally and restart the
wake window; when it lapses (30 seconds by default, never while a finger is
down) the panel goes dark again. A wake level of zero, or a CastKit that never
sends one, keeps the old behavior: no touch at zero brightness. The receiver
reports `wake,1`, `wake,0` and `wake_controls,<percent>,<seconds>` on Display
Events. The latch is `presto_wake.h`, tested on the host by `tests/test_wake.py`.

## Development

```sh
python -m unittest discover -s tests
esphome compile examples/display-test.yaml
esphome compile examples/hardware-test.yaml
esphome compile examples/castkit.yaml
```

The receiver and decoder tests compile and execute the actual C++, miniz, and JPEGDEC
code on the host; they require `g++`, and the JPEG tests require Pillow
(`pip install pillow`) to encode their synthetic images. CI compiles all three firmware examples and checks the
project's Python and C++ formatting. Vendored code retains upstream formatting.
See [UPSTREAM.md](UPSTREAM.md) for licenses, pinned sources and adaptations.

Contributions are welcome, especially scanout that remains
stable under Wi-Fi, touch, OTA and changing content. Please include hardware
results with horizontal and vertical patterns; solid vertical bars alone can
hide a row-order or vertical-timing fault.
