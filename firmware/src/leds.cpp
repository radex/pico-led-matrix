#include <Arduino.h>
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "hardware/pio.h"

#include "leds.h"
#include "leds.pio.h"

PIO leds_pio = pio0;
uint pusher_sm = 255; // invalid
uint delay_sm = 255; // invalid
uint row_sm = 255; // invalid

// NOTE: RCLK, SRCLK capture on *rising* edge
inline void pulsePin(uint8_t pin) {
  gpio_put(pin, HIGH);
   // there are glitches without this (maybe just due to breadboard...)
  asm volatile("nop \n nop \n nop");
  gpio_put(pin, LOW);
}

void clearShiftReg(uint8_t srclk, uint8_t srclr) {
  gpio_put(srclr, LOW);
  pulsePin(srclk);
  gpio_put(srclr, HIGH);
}

inline void outputEnable(uint8_t pin, bool enable) {
  gpio_put(pin, !enable);
}

// we have COLOR_BITS-bit color depth, so 2^COLOR_BITS levels of brightness
// we go from phase 0 to phase (COLOR_BITS-1)
uint8_t brightnessPhase = 0;

// delays in nanoseconds
#define NS_TO_DELAY(ns) ns / NS_PER_CYCLE
uint32_t brightnessPhaseDelays[COLOR_BITS] = {
  NS_TO_DELAY(50),
  NS_TO_DELAY(100),
  NS_TO_DELAY(200),
  NS_TO_DELAY(500),
  NS_TO_DELAY(1500),
  NS_TO_DELAY(6000),
  NS_TO_DELAY(20000),
  NS_TO_DELAY(60000),
};

// NOTE: Alignment required to allow 4-byte reads
uint8_t framebuffer[ROW_COUNT * COL_COUNT]  __attribute__((aligned(32))) = {0};

// Framebuffer encoded for fast PIO pixel pushing
// There's one buffer for each of pixel's bit indices (aka brightness phases),
// Then for each row (laid out bottom to top), we have:
// - one 32-bit word per horizontal (column) module:
//     20 pixels = 24 shift register stages (4 placeholders), 7 unused bits,
//     1 bit to indicate end of row
// - one word for selecting (shifting) a row:
//     1 bit (LSB) to indicate start of frame (1) or not (0)
//     remaining bits to indicate a number of shift register pulses
//     (again, 24 shift register stages per 20 rows, so there are placeholders)
uint32_t ledBuffer[8][ROW_COUNT * (COL_MODULES + 1)] = {0};
bool ledBufferReady = false;

void leds_set_framebuffer(uint8_t *buffer) {
  // TODO: Use a separate buffer, then copy to ledsBuffer to avoid tearing
  for (int bi = 0; bi < 8; bi++) {
    uint8_t bitPosition = 1 << bi;

    for (int yModule = 0; yModule < ROW_MODULES; yModule++) {
      for (int moduleY = 0; moduleY < 20; moduleY++) {
        auto y = yModule * 20 + moduleY;

        auto bufferYOffset = (ROW_COUNT - 1 - y) * COL_COUNT;
        auto outputYOffset = y * (COL_MODULES + 1);

        // set data for a given row
        for (int xModule = 0; xModule < COL_MODULES; xModule++) {
          auto bufferXOffset = bufferYOffset + xModule * 20;
          uint32_t sample = 0;

          for (int x = 0; x < 20; x++) {
            // insert placeholders for unused stages
            // (before pixels 0, 6, 13)
            if (x == 0 || x == 6 || x == 13) {
              sample >>= 1;
            }
            uint8_t px = buffer[bufferXOffset + x];
            bool bit = px & bitPosition;
            sample = (sample >> 1) | (bit ? 0x80000000 : 0);
          }
          // insert placeholder for unused last stage (after pixel 19)
          sample >>=1;
          // shift to LSB position
          sample >>=8;
          // MSB=1 indicates end of row
          if (xModule == COL_MODULES - 1) {
            sample |= 0x80000000;
          }

          ledBuffer[bi][outputYOffset + xModule] = sample;
        }

        // set row shifting data
        bool firstRow = y == (ROW_COUNT - 1);
        uint32_t rowPulses = 1;

        if (moduleY == 0) {
          rowPulses++;
        }

        if (moduleY == 7 || moduleY == 14 || (moduleY == 0 && yModule != 0)) {
          rowPulses++;
        }

        uint32_t rowData = firstRow | (rowPulses << 1);
        ledBuffer[bi][outputYOffset + COL_MODULES] = rowData;
      }
    }
  }
  ledBufferReady = true;

  // copy to framebuffer
  // TODO: mutex? double buffer? or something...
  memcpy(framebuffer, buffer, ROW_COUNT * COL_COUNT);
}

void leds_init() {
  memset(framebuffer, 0, sizeof(framebuffer));

  // disable output
  outputEnable(COL_OE, false);
  outputEnable(ROW_OE, false);

  // set up col pins
  pinMode(COL_SER, OUTPUT);
  pinMode(COL_OE, OUTPUT);
  outputEnable(ROW_OE, false);
  // pinMode(COL_RCLK, OUTPUT);
  pinMode(RCLK, OUTPUT);
  pinMode(COL_SRCLK, OUTPUT);
  pinMode(COL_SRCLR, OUTPUT);

  // set up row pins
  pinMode(ROW_SER, OUTPUT);
  pinMode(ROW_OE, OUTPUT);
  outputEnable(ROW_OE, false);
  // pinMode(ROW_RCLK, OUTPUT);
  pinMode(ROW_SRCLK, OUTPUT);
  pinMode(ROW_SRCLR, OUTPUT);

  // clear output - cols
  clearShiftReg(COL_SRCLK, COL_SRCLR);
  pulsePin(RCLK);
  outputEnable(COL_OE, true); // this is fine, because we control OE via rows only

  // clear output - rows
  clearShiftReg(ROW_SRCLK, ROW_SRCLR);
  pulsePin(RCLK);
}

void leds_disable() {
  outputEnable(ROW_OE, false);
}

void main2() {
  // where we're going, we don't need no interrupts
  noInterrupts();
  while (true) {
    leds_render();
  }
}

void leds_initPusher();
void leds_initRowSelector();
void leds_initDelay();

void leds_initRenderer() {
  leds_initPusher();
  leds_initRowSelector();
  leds_initDelay();
  multicore_reset_core1();
  multicore_launch_core1(main2);
}

void leds_render() {
  if (!ledBufferReady) {
    return;
  }

  auto buffer = ledBuffer[brightnessPhase];
  auto delayData = brightnessPhaseDelays[brightnessPhase];

  // The correct data to push onto PIO has been precomputed by leds_set_framebuffer
  // So we only need to move the buffer onto PIO TX FIFOs to keep them full
  // TODO: Rewrite this to be interrupt-based. Should be relatively easy to always keep PIO
  // full via interrupts and free up most of the core's time to other tasks
  for (uint8_t y = 0; y < ROW_COUNT; y++) {
    // set row data
    for (uint8_t x = 0; x < COL_MODULES; x++) {
      auto pxValues = *buffer++;
      pio_sm_put_blocking(leds_pio, pusher_sm, pxValues);
    }

    // set row selection data
    auto rowSelData = *buffer++;
    pio_sm_put_blocking(leds_pio, row_sm, rowSelData);

    // set delay data
    pio_sm_put_blocking(leds_pio, delay_sm, delayData);
  }

  // next brightness phase
  brightnessPhase = (brightnessPhase + 1) % COLOR_BITS;
}

void leds_initPusher() {
  PIO pio = leds_pio;
  uint sm = pio_claim_unused_sm(pio, true);
  pusher_sm = sm;

  uint offset = pio_add_program(pio, &leds_px_pusher_program);

  pio_sm_config config = leds_px_pusher_program_get_default_config(offset);
  sm_config_set_clkdiv_int_frac(&config, 1, 0);

  // Shift OSR to the right, autopull
  sm_config_set_out_shift(&config, true, true, 32);

  // Use FIFO join to create a longer TX FIFO
  // NOTE: This is not needed for the other SMs, as the px pusher will always be the bottleneck
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

  // Set OUT (data) pin, connect to pad, set as output
  sm_config_set_out_pins(&config, COL_SER, 1);
  pio_gpio_init(pio, COL_SER);
  pio_sm_set_consecutive_pindirs(pio, sm, COL_SER, 1, true);

  // data is inverted
  gpio_set_outover(COL_SER, GPIO_OVERRIDE_INVERT);

  // Set sideset (SRCLK) pin, connect to pad, set as output
  sm_config_set_sideset_pins(&config, COL_SRCLK);
  pio_gpio_init(pio, COL_SRCLK);
  pio_sm_set_consecutive_pindirs(pio, sm, COL_SRCLK, 1, true);

  // Set SET (RCLK) pin, connect to pad, set as output
  sm_config_set_set_pins(&config, RCLK, 1);
  pio_gpio_init(pio, RCLK);
  pio_sm_set_consecutive_pindirs(pio, sm, RCLK, 1, true);

  // Load our configuration, and jump to the start of the program
  pio_sm_init(pio, sm, offset, &config);
  pio_sm_set_enabled(pio, sm, true);
}

void leds_initRowSelector() {
  PIO pio = leds_pio;
  uint sm = pio_claim_unused_sm(pio, true);
  row_sm = sm;

  uint offset = pio_add_program(pio, &leds_row_selector_program);

  pio_sm_config config = leds_row_selector_program_get_default_config(offset);
  sm_config_set_clkdiv_int_frac(&config, 1, 0);

  // Shift OSR to the right, autopull
  sm_config_set_out_shift(&config, true, true, 32);

  // Set OUT and SET (data) pin, connect to pad, set as output
  sm_config_set_out_pins(&config, ROW_SER, 1);
  sm_config_set_set_pins(&config, ROW_SER, 1);
  pio_gpio_init(pio, ROW_SER);
  pio_sm_set_consecutive_pindirs(pio, sm, ROW_SER, 1, true);

  // Set sideset (SRCLK) pin, connect to pad, set as output
  sm_config_set_sideset_pins(&config, ROW_SRCLK);
  pio_gpio_init(pio, ROW_SRCLK);
  pio_sm_set_consecutive_pindirs(pio, sm, ROW_SRCLK, 1, true);

  // Load our configuration, and jump to the start of the program
  pio_sm_init(pio, sm, offset, &config);
  pio_sm_set_enabled(pio, sm, true);
}

void leds_initDelay() {
  PIO pio = leds_pio;
  uint sm = pio_claim_unused_sm(pio, true);
  delay_sm = sm;

  uint offset = pio_add_program(pio, &leds_delay_program);

  pio_sm_config config = leds_delay_program_get_default_config(offset);
  sm_config_set_clkdiv_int_frac(&config, 1, 0);

  // Shift OSR to the right, autopull
  sm_config_set_out_shift(&config, true, true, 32);

  // Set sideset (OE) pin, connect to pad, set as output
  sm_config_set_sideset_pins(&config, ROW_OE);
  pio_gpio_init(pio, ROW_OE);
  pio_sm_set_consecutive_pindirs(pio, sm, ROW_OE, 1, true);

  // Load our configuration, and jump to the start of the program
  pio_sm_init(pio, sm, offset, &config);
  pio_sm_set_enabled(pio, sm, true);
}
