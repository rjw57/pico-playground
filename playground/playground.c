#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "timing.pio.h"
#include "tvout.pio.h"

// TV signal timing. See http://martin.hinner.info/vga/pal.html. We repeatedly send the first field
// which is sometimes known as "240p". (Or the PAL equivalent of "272p".) This works out as a
// resolution of 352 x 272.
#define LINE_PERIOD_NS 64000                               // Period of one line of video (ns)
#define LINES_PER_FIELD 310                                // Number of lines in a *field*
#define HSYNC_WIDTH_NS 4700                                // Line sync pulse width (ns)
#define HORIZ_OVERSCAN_NS 4000                             // Horizontal overscan (ns)
#define VSYNC_LINES_PER_FIELD 5                            // V-sync lines at start of field
#define VERT_OVERSCAN_LINES 8                              // Vertical overscan (lines per *field*)
#define VERT_VISIBLE_START_LINE 23 + VERT_OVERSCAN_LINES   // Start line of visible data (0-based)
#define VISIBLE_LINES_PER_FIELD 272                        // Number of visible lines per field
#define FRONT_PORCH_WIDTH_NS (1650 + HORIZ_OVERSCAN_NS)    // Front porch width (ns)
#define VISIBLE_WIDTH_NS (52000 - (2 * HORIZ_OVERSCAN_NS)) // Visible area (ns)
#define SHORT_SYNC_WIDTH_NS 2350                           // "Short" sync pulse width (ns)
#define LONG_SYNC_WIDTH_NS 27300                           // "Long" sync pulse width (ns)
#define DOTS_PER_LINE 512                                  // Dots per line (inc. invisible area)

// Implied back porch period
#define BACK_PORCH_WIDTH_NS                                                                        \
  (LINE_PERIOD_NS - VISIBLE_WIDTH_NS - FRONT_PORCH_WIDTH_NS - HSYNC_WIDTH_NS)

// Implied dot frequency
#define DOT_CLOCK_FREQ (DOTS_PER_LINE * (1e9 / LINE_PERIOD_NS))

// Implied dot period and dots per visible line
#define DOT_PERIOD_NS (LINE_PERIOD_NS / DOTS_PER_LINE)
#define VISIBLE_DOTS_PER_LINE (VISIBLE_WIDTH_NS / DOT_PERIOD_NS)

#define SYNC_GPIO_PIN 16                   // GPIO pin for SYNC signal == pin 21
#define VIDEO_GPIO_PIN (SYNC_GPIO_PIN + 1) // == pin 22

// Desired timing state machine clock frequency in hertz. Should be equal to the
// dot frequency.
#define TIMING_SM_FREQ (DOTS_PER_LINE * (1e9 / LINE_PERIOD_NS))

// Timing program for a blank line
uint32_t timing_blank_line[] = {
    line_timing_encode(0, HSYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, LINE_PERIOD_NS - HSYNC_WIDTH_NS, SIDE_EFFECT_NOP),
};
#define TIMING_BLANK_LINE_LEN (sizeof(timing_blank_line) / sizeof(timing_blank_line[0]))

// Timing program for a visible line
uint32_t timing_visible_line[] = {
    line_timing_encode(0, HSYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, BACK_PORCH_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, VISIBLE_WIDTH_NS, SIDE_EFFECT_SET_TRIGGER),
    line_timing_encode(1, FRONT_PORCH_WIDTH_NS, SIDE_EFFECT_CLEAR_TRIGGER),
};
#define TIMING_VISIBLE_LINE_LEN (sizeof(timing_visible_line) / sizeof(timing_visible_line[0]))

// Timing program for vsync lines
uint32_t timing_vsync_lines[] = {
    // 5 "long" sync pulses
    line_timing_encode(0, LONG_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - LONG_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, LONG_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - LONG_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, LONG_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - LONG_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, LONG_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - LONG_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, LONG_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - LONG_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),

    // 5 "short" sync pulses
    line_timing_encode(0, SHORT_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - SHORT_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, SHORT_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - SHORT_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, SHORT_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - SHORT_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, SHORT_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - SHORT_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
    line_timing_encode(0, SHORT_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - SHORT_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
};
#define TIMING_VSYNC_LINES_LEN (sizeof(timing_vsync_lines) / sizeof(timing_vsync_lines[0]))

int main() {
  // Check that all periods are an integer multiple of character period
  static_assert(LINE_PERIOD_NS % DOT_PERIOD_NS == 0);

  // Check that the number of *visible* dots per line is a multiple of 8.
  static_assert((VISIBLE_DOTS_PER_LINE & 0x7) == 0);

  // Check that the number of *visible* lines per field is a multiple of 8.
  static_assert((VISIBLE_LINES_PER_FIELD & 0x7) == 0);

  // Ensure IRQ 4 of the PIO is clear
  PIO pio = pio0;
  pio_interrupt_clear(pio, 4);

  // Configure and enable output program
  uint video_output_offset = pio_add_program(pio, &video_output_program);
  uint video_output_sm = pio_claim_unused_sm(pio, true);
  video_output_program_init(pio, video_output_sm, video_output_offset, VIDEO_GPIO_PIN,
                            DOT_CLOCK_FREQ);
  pio_sm_set_enabled(pio, video_output_sm, true);
  pio_sm_put(pio, video_output_sm, VISIBLE_DOTS_PER_LINE);

  // Configure and enable timing program.
  uint line_timing_offset = pio_add_program(pio, &line_timing_program);
  uint line_timing_sm = pio_claim_unused_sm(pio, true);
  line_timing_program_init(pio, line_timing_sm, line_timing_offset, SYNC_GPIO_PIN);
  pio_sm_set_enabled(pio, line_timing_sm, true);

  while (true) {
    for (int i = 0; i < TIMING_VSYNC_LINES_LEN; i++) {
      pio_sm_put_blocking(pio, line_timing_sm, timing_vsync_lines[i]);
    }
    for (int j = VSYNC_LINES_PER_FIELD; j < LINES_PER_FIELD; j++) {
      if ((j >= VERT_VISIBLE_START_LINE) &&
          (j < VERT_VISIBLE_START_LINE + VISIBLE_LINES_PER_FIELD)) {
        for (int i = 0; i < TIMING_VISIBLE_LINE_LEN; i++) {
          pio_sm_put_blocking(pio, line_timing_sm, timing_visible_line[i]);
        }
      } else {
        for (int i = 0; i < TIMING_BLANK_LINE_LEN; i++) {
          pio_sm_put_blocking(pio, line_timing_sm, timing_blank_line[i]);
        }
      }
    }
  }
}
