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

The native-resolution experiment keeps two full RGB565 frames in PSRAM and
prefetches rows via the paced XIP streaming FIFO into two 128-row SRAM banks
in eight-row batches. Timing words run from a cyclic SRAM
DMA table; pixel DMA follows a SRAM row-address table rather than an IRQ-updated
pointer. Frame swaps occur at vertical blank. Pixel DMA reads buffered rows.
The display core has a separate Arduino stack and no SysTick; display and DMA
receive bus priority. The inflater runs from RAM on RP2, reads through a 4 KiB
SRAM input window and writes through its 32 KiB SRAM dictionary. PSRAM copies
wait for outstanding row fetches and yield in row-sized chunks; base64 first
decodes into SRAM. Unchanged heartbeat pixels do not swap buffers.
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
