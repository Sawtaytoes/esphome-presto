The ST7701 PIO/DMA driver and PIO programs derive from Pimoroni Presto,
MIT license, commit b96a8cbe3ff0b22213141e3d3810e8217a288b89.
Source: https://github.com/pimoroni/presto/tree/b96a8cbe3ff0b22213141e3d3810e8217a288b89/drivers/st7701

Adaptations: remove PicoGraphics inheritance/conversion; use ESPHome's generated
PIO include paths; ESPHome writes double-buffered native RGB565 in PSRAM.
The controller initialization and PIO scanout remain upstream-derived.
