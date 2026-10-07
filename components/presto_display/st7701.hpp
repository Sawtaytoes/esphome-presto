#pragma once

#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "hardware/irq.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace pimoroni {
  constexpr uint PIN_UNUSED = 255;
  enum Rotation { ROTATE_0, ROTATE_90, ROTATE_180, ROTATE_270 };
  struct SPIPins { spi_inst_t *spi; uint cs; uint sck; uint mosi; uint miso; uint dc; uint bl; };

  /// ST7701S display driver
  /// See datasheet:
  /// https://focuslcds.com/wp-content/uploads/Drivers/ST7701S.pdf
  /// See ESPHome implementation:
  /// https://github.com/esphome/esphome/blob/dev/esphome/components/st7701s
  class ST7701 {
    // DMA scans SRAM while the complete frames remain in PSRAM.
    alignas(4) uint16_t scanlines[4][480];
    uint16_t width;
    uint16_t height;
    Rotation rotation;
    spi_inst_t *spi = spi1;

    //--------------------------------------------------
    // Variables
    //--------------------------------------------------
  private:

    // interface pins with our standard defaults where appropriate
    uint spi_cs;
    uint spi_sck;
    uint spi_dat;
    uint lcd_bl;
    uint parallel_sm;
    uint timing_sm;
    uint palette_sm;
    PIO st_pio;
    uint parallel_offset;
    uint timing_offset;
    uint palette_offset;
    uint st_dma;
    uint st_dma2;
    int st_dma3 = -1;
    int st_dma4 = -1;

    uint d0 = 1; // First pin of 18-bit parallel interface
    uint hsync  = 19;
    uint vsync  = 20;
    uint lcd_de = 21;
    uint lcd_dot_clk = 22;

    static const uint32_t SPI_BAUD = 8'000'000;
    static const uint32_t BACKLIGHT_PWM_TOP = 6200;

    // Default after power-on or reset is 0
    //uint8_t madctl = 0;

  public:
    // Parallel init
    ST7701(uint16_t width, uint16_t height, Rotation rotation, SPIPins control_pins, uint16_t* framebuffer, uint32_t* palette = nullptr,
      uint d0=1, uint hsync=19, uint vsync=20, uint lcd_de = 21, uint lcd_dot_clk = 22);
    virtual ~ST7701() {}

    void init();
    void cleanup();
    void set_backlight(uint8_t brightness);


    // The format is an 18-bit value: RGB566, followed by the final bit of red.
    // It is MSB aligned, i.e. the top bit of red is in the MSB.
    uint32_t get_encoded_palette_entry(uint8_t entry) const { return palette[entry]; }

    void set_framebuffer(uint16_t* next_fb) {
      requested_framebuffer = next_fb;
      next_framebuffer.store(next_fb, std::memory_order_release);
    }

    void wait_for_vsync();

    // Only to be called by ISR
    void drive_timing();
    void handle_end_of_line();

    void set_rotation(Rotation rotate);

  private:
    void common_init();
    void command(uint8_t command, size_t len = 0, const char *data = NULL) const;
    void command_bkx_disable() const;
    void command_bk0_enable() const;
    void command_bk1_enable() const;
    void command_bk3_enable() const;

    void start_line_xfer();
    void start_frame_xfer();

    // Timing status
    uint16_t timing_row = 0;
    uint16_t timing_phase = 0;
    uint16_t *requested_framebuffer = nullptr;
    std::atomic<uint16_t *> presented_framebuffer{nullptr};

    uint16_t* framebuffer;
    std::atomic<uint16_t *> next_framebuffer{nullptr};

    uint32_t* palette = nullptr;

    uint16_t* next_line_addr;
    int display_row = 0;
    int row_shift = 0;
    int fill_row = 0;
  };

}