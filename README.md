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
claim that every application or workload has been validated.

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
The receiver uses a SRAM input window and dictionary for large compressed images,
then copies bounded chunks to PSRAM with scanout service gaps. Touch polling
continues during these copies. Unchanged heartbeat pixels acknowledge without
swapping framebuffers.

The receiver assembles bounded base64 chunks, validates the rectangle and its
acknowledged base, and decompresses into a separate scratch buffer. Incomplete,
out-of-order, corrupt or oversized frames do not reach the displayed buffer.
Only a complete frame is presented and acknowledged. Lost connections recover
with a full frame. After seven seconds without a presented frame, the board
shows an offline marker and disables touch. Touch is also disabled at zero
backlight brightness.

## Development

```sh
python -m unittest discover -s tests
esphome compile examples/display-test.yaml
esphome compile examples/hardware-test.yaml
esphome compile examples/castkit.yaml
```

The decoder tests compile and execute the actual C++/miniz decoder on the host;
they require `g++`. CI compiles all three firmware examples and checks the
project's Python and C++ formatting. Vendored code retains upstream formatting.
See [UPSTREAM.md](UPSTREAM.md) for licenses, pinned sources and adaptations.

Contributions are welcome, especially scanout that remains
stable under Wi-Fi, touch, OTA and changing content. Please include hardware
results with horizontal and vertical patterns; solid vertical bars alone can
hide a row-order or vertical-timing fault.
