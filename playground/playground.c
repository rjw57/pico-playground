#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "timing.pio.h"

// See http://martin.hinner.info/vga/pal.html
#define LINE_PERIOD_NS 64000      // Period of one line of video (ns)
#define LINES_PER_FRAME 625       // Number of lines in frame
#define DOTS_PER_LINE 640         // Dots per line (including invisible area)
#define HSYNC_WIDTH_NS 4700       // Line sync pulse width (ns)
#define FRONT_PORCH_WIDTH_NS 1650 // Front porch width (ns)
#define VISIBLE_WIDTH_NS 52000    // Visible area (ns)
#define SHORT_SYNC_WIDTH_NS 2350  // "Short" sync pulse width (ns)
#define LONG_SYNC_WIDTH_NS 27300  // "Long" sync pulse width (ns)

#define SYNC_GPIO_PIN 16                   // == pin 21
#define VIDEO_GPIO_PIN (SYNC_GPIO_PIN + 1) // == pin 22

#define BACK_PORCH_WIDTH_NS                                                    \
  (LINE_PERIOD_NS - VISIBLE_WIDTH_NS - FRONT_PORCH_WIDTH_NS - HSYNC_WIDTH_NS)
#define DOT_PERIOD_NS (LINE_PERIOD_NS / DOTS_PER_LINE) // Period of one dot (ns)

// Desired timing state machine clock frequency in hertz. Should be twice dot
// frequency.
#define TIMING_SM_FREQ (2 * DOTS_PER_LINE * (1e9 / LINE_PERIOD_NS))

static uint32_t visible_line[DOTS_PER_LINE >> 4];
static uint32_t blank_line[DOTS_PER_LINE >> 4];
static uint32_t long_long_line[DOTS_PER_LINE >> 4];
static uint32_t long_short_line[DOTS_PER_LINE >> 4];
static uint32_t short_long_line[DOTS_PER_LINE >> 4];
static uint32_t short_short_line[DOTS_PER_LINE >> 4];
static uint32_t *frame_lines[LINES_PER_FRAME];

static inline void timing_program_init(PIO pio, uint sm, uint offset) {
  pio_sm_config c = timing_program_get_default_config(offset);
  sm_config_set_out_pins(&c, SYNC_GPIO_PIN, 2);
  sm_config_set_out_shift(&c, true, true, 0);
  sm_config_set_clkdiv(&c, ((float)clock_get_hz(clk_sys)) / TIMING_SM_FREQ);
  pio_gpio_init(pio, SYNC_GPIO_PIN);
  pio_gpio_init(pio, VIDEO_GPIO_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm, SYNC_GPIO_PIN, 2, true);
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
}

int main() {
  // Check that the number of dots per line is a multiple of 16.
  static_assert((DOTS_PER_LINE & 0xf) == 0);
  static_assert((LINE_PERIOD_NS % DOT_PERIOD_NS) == 0);

  for (int i = 0, t = 0; i < DOTS_PER_LINE; i++, t += DOT_PERIOD_NS) {
    uint sync = 1, visible = 0;
    uint shift = (i & 0xf) << 1;

    if (t < HSYNC_WIDTH_NS) {
      sync = 0;
    }

    if ((t >= (HSYNC_WIDTH_NS + BACK_PORCH_WIDTH_NS)) &&
        (t < (HSYNC_WIDTH_NS + BACK_PORCH_WIDTH_NS + VISIBLE_WIDTH_NS))) {
      visible = 1;
    }

    visible_line[i >> 4] &= ~(0x3 << shift);
    visible_line[i >> 4] |= ((sync & 0x1) | ((visible & 0x1) << 1)) << shift;

    blank_line[i >> 4] &= ~(0x3 << shift);
    blank_line[i >> 4] |= (sync & 0x1) << shift;
  }

  for (int i = 0, j = DOTS_PER_LINE >> 1, t = 0; i < DOTS_PER_LINE >> 1;
       i++, j++, t += DOT_PERIOD_NS) {
    uint shift_i = (i & 0xf) << 1, shift_j = (j & 0xf) << 1;
    uint short_sync = 1, long_sync = 1;

    if (t < SHORT_SYNC_WIDTH_NS) {
      short_sync = 0;
    }

    if (t < LONG_SYNC_WIDTH_NS) {
      long_sync = 0;
    }

    long_long_line[i >> 4] &= ~(0x3 << shift_i);
    long_long_line[i >> 4] |= (long_sync & 0x1) << shift_i;
    long_long_line[j >> 4] &= ~(0x3 << shift_j);
    long_long_line[j >> 4] |= (long_sync & 0x1) << shift_j;

    long_short_line[i >> 4] &= ~(0x3 << shift_i);
    long_short_line[i >> 4] |= (long_sync & 0x1) << shift_i;
    long_short_line[j >> 4] &= ~(0x3 << shift_j);
    long_short_line[j >> 4] |= (short_sync & 0x1) << shift_j;

    short_long_line[i >> 4] &= ~(0x3 << shift_i);
    short_long_line[i >> 4] |= (short_sync & 0x1) << shift_i;
    short_long_line[j >> 4] &= ~(0x3 << shift_j);
    short_long_line[j >> 4] |= (long_sync & 0x1) << shift_j;

    short_short_line[i >> 4] &= ~(0x3 << shift_i);
    short_short_line[i >> 4] |= (short_sync & 0x1) << shift_i;
    short_short_line[j >> 4] &= ~(0x3 << shift_j);
    short_short_line[j >> 4] |= (short_sync & 0x1) << shift_j;
  }

  for (int i = 0; i < LINES_PER_FRAME; i++) {
    if (i < 2) {
      frame_lines[i] = long_long_line;
    } else if (i < 3) {
      frame_lines[i] = long_short_line;
    } else if (i < 5) {
      frame_lines[i] = short_short_line;
    } else if (i < 23) {
      frame_lines[i] = blank_line;
    } else if (i < 310) {
      frame_lines[i] = visible_line;
    } else if (i < 312) {
      frame_lines[i] = short_short_line;
    } else if (i < 313) {
      frame_lines[i] = short_long_line;
    } else if (i < 315) {
      frame_lines[i] = long_long_line;
    } else if (i < 317) {
      frame_lines[i] = short_short_line;
    } else if (i < 335) {
      frame_lines[i] = blank_line;
    } else if (i < 622) {
      frame_lines[i] = visible_line;
    } else {
      frame_lines[i] = short_short_line;
    }
  }

  PIO pio = pio0;
  uint timing_offset = pio_add_program(pio, &timing_program);
  uint timing_sm = pio_claim_unused_sm(pio, true);
  timing_program_init(pio, timing_sm, timing_offset);

  while (true) {
    for (int i = 0; i < LINES_PER_FRAME; i++) {
      uint32_t *l = frame_lines[i];
      for (int j = 0; j < (DOTS_PER_LINE >> 4); j++) {
        while (pio_sm_is_tx_fifo_full(pio, timing_sm)) {
        }
        pio_sm_put(pio, timing_sm, l[j]);
      }
    }
  }
}
