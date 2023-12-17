#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "timing.pio.h"

// TV signal timing. See http://martin.hinner.info/vga/pal.html. We repeatedly send the first field
// which is sometimes known as "240p". (Or the PAL equivalent of "272p".) This works out as a
// resolution of 352 x 272.
#define LINE_PERIOD_NS 64000                               // Period of one line of video (ns)
#define LINES_PER_FIELD 310                                // Number of lines in a *field*
#define HSYNC_WIDTH_NS 4700                                // Line sync pulse width (ns)
#define HORIZ_OVERSCAN_NS 4000                             // Horizontal overscan (ns)
#define VERT_OVERSCAN_LINES 8                              // Vertical overscan (lines per *field*)
#define VERT_VISIBLE_START_LINE 23 + VERT_OVERSCAN_LINES   // Start line of visible data (0-based)
#define VISIBLE_LINES_PER_FIELD 272                        // Number of visible lines per field
#define FRONT_PORCH_WIDTH_NS (1650 + HORIZ_OVERSCAN_NS)    // Front porch width (ns)
#define VISIBLE_WIDTH_NS (52000 - (2 * HORIZ_OVERSCAN_NS)) // Visible area (ns)
#define SHORT_SYNC_WIDTH_NS 2350                           // "Short" sync pulse width (ns)
#define LONG_SYNC_WIDTH_NS 27300                           // "Long" sync pulse width (ns)

#define DOTS_PER_LINE 512 // Dots per line (including invisible area)

// Implied back porch period.
#define BACK_PORCH_WIDTH_NS                                                                        \
  (LINE_PERIOD_NS - VISIBLE_WIDTH_NS - FRONT_PORCH_WIDTH_NS - HSYNC_WIDTH_NS)

// Implied dot period and dots per visible line.
#define DOT_PERIOD_NS (LINE_PERIOD_NS / DOTS_PER_LINE)
#define VISIBLE_DOTS_PER_LINE (VISIBLE_WIDTH_NS / DOT_PERIOD_NS)

#define SYNC_GPIO_PIN 16                   // GPIO pin for SYNC signal == pin 21
#define VIDEO_GPIO_PIN (SYNC_GPIO_PIN + 1) // == pin 22

// Desired timing state machine clock frequency in hertz. Should be equal to the
// dot frequency.
#define TIMING_SM_FREQ (DOTS_PER_LINE * (1e9 / LINE_PERIOD_NS))

static uint32_t visible_line[DOTS_PER_LINE >> 4];
static uint32_t visible_line_b[DOTS_PER_LINE >> 4];
static uint32_t blank_line[DOTS_PER_LINE >> 4];
static uint32_t long_long_line[DOTS_PER_LINE >> 4];
static uint32_t long_short_line[DOTS_PER_LINE >> 4];
static uint32_t short_long_line[DOTS_PER_LINE >> 4];
static uint32_t short_short_line[DOTS_PER_LINE >> 4];
static uint32_t *frame_lines[LINES_PER_FIELD];

static inline void timing_program_init(PIO pio, uint sm, uint offset);

int main() {
  // Check that all periods are an integer multiple of character period
  static_assert(LINE_PERIOD_NS % DOT_PERIOD_NS == 0);

  // Check that the number of *visible* dots per line is a multiple of 8.
  static_assert((VISIBLE_DOTS_PER_LINE & 0x7) == 0);

  // Check that the number of *visible* lines per field is a multiple of 8.
  static_assert((VISIBLE_LINES_PER_FIELD & 0x7) == 0);

  for (int i = 0, t = 0; i < DOTS_PER_LINE; i++, t += DOT_PERIOD_NS) {
    uint sync = 1, visible = 0, visible_b = 0;
    uint shift = (i & 0xf) << 1;

    if (t < HSYNC_WIDTH_NS) {
      sync = 0;
    }

    if ((t >= (HSYNC_WIDTH_NS + BACK_PORCH_WIDTH_NS)) &&
        (t < (HSYNC_WIDTH_NS + BACK_PORCH_WIDTH_NS + VISIBLE_WIDTH_NS))) {
      visible = i & 0x1;
      visible_b = (~i) & 0x1;
    }

    visible_line[i >> 4] &= ~(0x3 << shift);
    visible_line[i >> 4] |= ((sync & 0x1) | ((visible & 0x1) << 1)) << shift;
    visible_line_b[i >> 4] &= ~(0x3 << shift);
    visible_line_b[i >> 4] |= ((sync & 0x1) | ((visible_b & 0x1) << 1)) << shift;

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

  for (int i = 0; i < LINES_PER_FIELD; i++) {
    if (i < 2) {
      frame_lines[i] = long_long_line;
    } else if (i < 3) {
      frame_lines[i] = long_short_line;
    } else if (i < 5) {
      frame_lines[i] = short_short_line;
    } else if (i < VERT_VISIBLE_START_LINE) {
      frame_lines[i] = blank_line;
    } else if (i < VERT_VISIBLE_START_LINE + VISIBLE_LINES_PER_FIELD) {
      if(i & 0x1) {
        frame_lines[i] = visible_line;
      } else {
        frame_lines[i] = visible_line_b;
      }
    } else {
      frame_lines[i] = blank_line;
    }
  }

  PIO pio = pio0;
  uint timing_offset = pio_add_program(pio, &timing_program);
  uint timing_sm = pio_claim_unused_sm(pio, true);
  timing_program_init(pio, timing_sm, timing_offset);

  while (true) {
    for (int i = 0; i < LINES_PER_FIELD; i++) {
      uint32_t *l = frame_lines[i];
      for (int j = 0; j < (DOTS_PER_LINE >> 4); j++) {
        while (pio_sm_is_tx_fifo_full(pio, timing_sm)) {
        }
        pio_sm_put(pio, timing_sm, l[j]);
      }
    }
  }
}

void timing_program_init(PIO pio, uint sm, uint offset) {
  pio_sm_config c = timing_program_get_default_config(offset);
  sm_config_set_out_pins(&c, SYNC_GPIO_PIN, 2);
  sm_config_set_out_shift(&c, true, true, 0);
  sm_config_set_clkdiv(&c, ((float)clock_get_hz(clk_sys)) / TIMING_SM_FREQ);
  sm_config_set_wrap(&c, offset + timing_wrap_target, offset + timing_wrap);
  pio_gpio_init(pio, SYNC_GPIO_PIN);
  pio_gpio_init(pio, VIDEO_GPIO_PIN);
  pio_sm_set_consecutive_pindirs(pio, sm, SYNC_GPIO_PIN, 2, true);
  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
}
