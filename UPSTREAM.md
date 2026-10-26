The ST7701 PIO/DMA driver and PIO programs derive from Pimoroni Presto,
MIT license, commit b96a8cbe3ff0b22213141e3d3810e8217a288b89.
Source: https://github.com/pimoroni/presto/tree/b96a8cbe3ff0b22213141e3d3810e8217a288b89/drivers/st7701

Adaptations: remove PicoGraphics inheritance/conversion; use ESPHome's generated
PIO include paths; ESPHome writes double-buffered native RGB565 in PSRAM.
The controller initialization and PIO scanout remain upstream-derived.


The bounded zlib inflater is miniz 3.1.2, MIT license, commit
77d0dce8627735138c51770d1799a1ef48f2117d:
https://github.com/richgel999/miniz/tree/77d0dce8627735138c51770d1799a1ef48f2117d
Its source headers retain upstream content alongside `MINIZ-LICENSE`.
`miniz_tinfl.c` adds a conditional RP2 RAM-section annotation to the inflater;
the decoding algorithm is unchanged. `miniz_export.h` supplies the static-build export macro.

The baseline JPEG decoder (codec 2) is JPEGDEC 1.8.5 by Larry Bank, BitBank Software, Inc.,
Apache License 2.0, commit 0e879642e5e68624687f9ade16d799a091298ec8:
https://github.com/bitbank2/JPEGDEC/tree/0e879642e5e68624687f9ade16d799a091298ec8
`JPEGDEC.h` is upstream `src/JPEGDEC.h`, unchanged, with `JPEGDEC-LICENSE`. `JPEGDEC.cpp`
is upstream `src/JPEGDEC.cpp` with its `#include "jpeg.inl"` replaced by the verbatim text of
`src/jpeg.inl`, between two marked comment blocks. ESPHome does not copy a `.inl` file into a
build, and it includes every `.h` of a component into `main.cpp`, where the definitions in
`jpeg.inl` would be duplicated, so the file cannot stay separate. Deleting the marked block
and restoring the include line gives the upstream file byte for byte. The ESP32-S3
assembly, the Cortex-M4 SIMD header, and `JPEGDisplay` are not vendored; none is built for
the RP2350. Host tests define
`__LINUX__` (standard headers instead of Arduino) and `NO_SIMD` (no SSE), so they run the same
portable C path as the board.

The native-resolution experiment keeps two full RGB565 frames in PSRAM and
prefetches rows via the paced XIP streaming FIFO into two 128-row SRAM banks
in eight-row batches. Timing words run from a cyclic SRAM
DMA table; pixel DMA follows a SRAM row-address table rather than an IRQ-updated
pointer. Frame swaps occur at vertical blank. Pixel DMA reads buffered rows.
The display core has a separate Arduino stack and no SysTick; display and DMA
receive bus priority. The inflater runs from RAM on RP2 and writes through its
32 KiB SRAM dictionary; each base64 chunk decodes into SRAM and is inflated as it
arrives straight into the back framebuffer or a frame slot. PSRAM copies wait for
outstanding row fetches and yield in row-sized chunks. Unchanged heartbeat pixels
do not swap buffers. The scanout may also read a frame slot directly; the swap is
the same vertical-blank pointer swap.
Touch polling remains active during the paced copies.
The enlarged 240×240 workaround is not accepted for production sharpness.
Direct PSRAM DMA is avoided: QMI bus stalls also stall SRAM DMA channels.
The XIP auxiliary FIFO is paced by DREQ_XIP_STREAM so pixel/timing DMA can
continue while QMI fetches a row batch. Shared doorbell
handlers bracket Arduino flash lockout to pause scanout safely.
OTA examples stop scanout before writing flash and reboot after upload errors.
Encrypted native-resolution frame transfer and OTA pass. Native changing music
artwork was physically accepted as steady with row-fetch gating; earlier ungated
candidates still jumped. Gesture response tuning remains in the CastKit worker.
These observations do not establish stability for every downstream workload.

DMA/PSRAM stall reference: https://github.com/micropython/micropython/issues/18471
