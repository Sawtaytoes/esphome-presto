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
host tests, but have not yet been measured on a panel.

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

**Events and entities.** ESPHome sends only the newest state of a text sensor that
it has not sent yet, so when the link is slow an earlier state on the same entity
is lost. The receiver therefore keeps each kind of report on its own entity, and
no single action or loop step publishes two states on one entity:

| Entity (example name) | Option | States |
| --- | --- | --- |
| Display Events | `events_id` (required) | `frame,…` and `error,…` acknowledgements, `touch,…`, `wake,…`, `wake_controls,…`, `controls,…` |
| Display Slot Events | `slot_events_id` | `slot,<slot>,<frame_id>,<status>`, at most one per loop step |
| Display Timing | `timing_id` | `timing,…`, `slot_timing,…`, `settled,…` |

Without `slot_events_id`, slot uploads still work but report nothing, and CastKit
needs those reports to use slots. Touch reads continue during long PSRAM copies
inside an action, so a `touch,…` can still share a call with that action's
acknowledgement on Display Events; the acknowledgement is published last.

The slots take 2,764,800 bytes of the 8 MiB PSRAM, beside the two framebuffers
(921,600 bytes). CastKit detects slot support by the presence of the `show_slot`
action, so an older CastKit keeps working with this firmware.

The optional `timing_id` text sensor ("Display Timing" in the example) reports
`timing,<frame>,<rect bytes>,<decode µs>,<PSRAM write µs>,<sync µs>,<sync
bytes>,<present µs>,<first to final chunk ms>,<free PSRAM bytes>` for each live
frame, `slot_timing,<slot>,<frame>,<rect bytes>,<decode µs>,<PSRAM write µs>,<first
chunk to decoded ms>,<free PSRAM bytes>` for each upload, and `settled,<bytes>,<µs>`
when the framebuffers match the picture again after a slot was shown.

The receiver's frame logic (`presto_receiver.cpp`, `presto_stream.cpp`) has no
ESPHome or Pico types; `tests/test_receiver.py` and `tests/test_pixels.py`
compile and drive that exact code on the host.

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

The receiver and decoder tests compile and execute the actual C++/miniz code on
the host; they require `g++`. CI compiles all three firmware examples and checks the
project's Python and C++ formatting. Vendored code retains upstream formatting.
See [UPSTREAM.md](UPSTREAM.md) for licenses, pinned sources and adaptations.

Contributions are welcome, especially scanout that remains
stable under Wi-Fi, touch, OTA and changing content. Please include hardware
results with horizontal and vertical patterns; solid vertical bars alone can
hide a row-order or vertical-timing fault.
