#include <stdalign.h>
#include <stdatomic.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "pico/sync.h"

#include "tvout.h"
#include "tvout.pio.h"

// Resolution
#define VISIBLE_DOTS_PER_LINE 640                          // Horizontal resolution
#define VISIBLE_LINES_PER_FIELD 256                        // Number of visible lines per field

// TV signal timing. See http://martin.hinner.info/vga/pal.html. We repeatedly send the first field
// which is sometimes known as "240p". (Or the PAL equivalent of "272p".)
#define LINE_PERIOD_NS 64000                               // Period of one line of video (ns)
#define LINES_PER_FIELD 310                                // Number of lines in a *field*
#define HSYNC_WIDTH_NS 4700                                // Line sync pulse width (ns)
#define HORIZ_OVERSCAN_NS 5520                             // Horizontal overscan (ns)
#define VSYNC_LINES_PER_FIELD 5                            // V-sync lines at start of field
#define VERT_OVERSCAN_LINES 16                             // Vertical overscan (lines per *field*)
#define VERT_VISIBLE_START_LINE (23 + VERT_OVERSCAN_LINES) // Start line of visible data (0-based)
#define FRONT_PORCH_WIDTH_NS (1650 + HORIZ_OVERSCAN_NS)    // Front porch width (ns)
#define VISIBLE_WIDTH_NS (52000 - (2 * HORIZ_OVERSCAN_NS)) // Visible area (ns)
#define SHORT_SYNC_WIDTH_NS 2350                           // "Short" sync pulse width (ns)
#define LONG_SYNC_WIDTH_NS 27300                           // "Long" sync pulse width (ns)

// Implied back porch period
#define BACK_PORCH_WIDTH_NS                                                                        \
  (LINE_PERIOD_NS - VISIBLE_WIDTH_NS - FRONT_PORCH_WIDTH_NS - HSYNC_WIDTH_NS)

// Implied dot frequency
#define DOT_CLOCK_FREQ (VISIBLE_DOTS_PER_LINE * (1e9 / VISIBLE_WIDTH_NS))

// Timing program for a blank line
alignas(8) uint32_t timing_blank_line[] = {
    line_timing_encode(0, HSYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, LINE_PERIOD_NS - HSYNC_WIDTH_NS, SIDE_EFFECT_NOP),
};
#define TIMING_BLANK_LINE_LEN (sizeof(timing_blank_line) / sizeof(timing_blank_line[0]))

// Timing program for a visible line. Note that we need to shift the visible portion by a few line
// timing program clock cycles because of the difference in time between side effect and pin change
// times.
alignas(16) uint32_t timing_visible_line[] = {
    line_timing_encode(0, HSYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, BACK_PORCH_WIDTH_NS + (2 * LINE_TIMING_CLOCK_PERIOD_NS), SIDE_EFFECT_NOP),
    line_timing_encode(1, VISIBLE_WIDTH_NS, SIDE_EFFECT_SET_TRIGGER),
    line_timing_encode(1, FRONT_PORCH_WIDTH_NS - (2 * LINE_TIMING_CLOCK_PERIOD_NS),
                       SIDE_EFFECT_CLEAR_TRIGGER),
};
#define TIMING_VISIBLE_LINE_LEN (sizeof(timing_visible_line) / sizeof(timing_visible_line[0]))

// "Long" sync pulse "half line"
alignas(8) uint32_t timing_long_sync_half_line[] = {
    line_timing_encode(0, LONG_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - LONG_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
};
#define TIMING_LONG_SYNC_HALF_LINE_LEN                                                             \
  (sizeof(timing_long_sync_half_line) / sizeof(timing_long_sync_half_line[0]))

// "Short" sync pulse "half line"
alignas(8) uint32_t timing_short_sync_half_line[] = {
    line_timing_encode(0, SHORT_SYNC_WIDTH_NS, SIDE_EFFECT_NOP),
    line_timing_encode(1, ((LINE_PERIOD_NS >> 1) - SHORT_SYNC_WIDTH_NS), SIDE_EFFECT_NOP),
};
#define TIMING_SHORT_SYNC_HALF_LINE_LEN                                                            \
  (sizeof(timing_short_sync_half_line) / sizeof(timing_short_sync_half_line[0]))

// Semaphore used to signal vblank.
semaphore_t vblank_semaphore;

// Current frame buffer pointer. Marked as atomic so that the ISR always gets a valid value.
static atomic_uintptr_t frame_buffer_ptr;

// Blanking interval callback
static tvout_vblank_callback_t vblank_callback = NULL;

// PIO-related configuration values.
static uint video_output_sm;
static uint video_output_offset;
static uint line_timing_sm;
static uint line_timing_offset;
static PIO pio_instance;

// Frame timing DMA channel number and config.
static uint field_timing_dma_channel;
static dma_channel_config field_timing_dma_channel_config;

// Video data DMA channel number.
static uint video_dma_channel;

// Configure a DMA channel to copy the frame buffer into the video output PIO state machine.
static inline dma_channel_config
get_video_output_dma_channel_config(uint dma_chan, PIO pio, uint sm,
                                    bool byte_oriented_frame_buffer) {
  dma_channel_config c = dma_channel_get_default_config(dma_chan);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
  channel_config_set_bswap(&c, byte_oriented_frame_buffer);
  return c;
}

// Configure a DMA channel to copy the timing configurations into the timing PIO state machine.
static inline dma_channel_config get_field_timing_dma_channel_config(uint dma_chan, PIO pio,
                                                                     uint sm) {
  dma_channel_config c = dma_channel_get_default_config(dma_chan);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
  return c;
}

// DMA handler called when each phase of a frame timing is finished.
static void field_timing_dma_handler() {
  // 0 - vsync A, 1 - vsync B, 2 - top blank lines, 3 - visible lines, 4 - bottom blank lines
  static uint phase = 0;

  dma_channel_acknowledge_irq0(field_timing_dma_channel);

  switch (phase) {
  case 0:
    // Start frame buffer transfer for the next field.
    dma_channel_transfer_from_buffer_now(video_dma_channel, (void *)atomic_load(&frame_buffer_ptr),
                                         VISIBLE_LINES_PER_FIELD * (VISIBLE_DOTS_PER_LINE >> 5));
    // "long pulse" half lines
    channel_config_set_ring(&field_timing_dma_channel_config, false, 3);
    dma_channel_set_config(field_timing_dma_channel, &field_timing_dma_channel_config, false);
    dma_channel_transfer_from_buffer_now(field_timing_dma_channel, timing_long_sync_half_line,
                                         TIMING_LONG_SYNC_HALF_LINE_LEN * VSYNC_LINES_PER_FIELD);
    break;
  case 1:
    // "short pulse" half lines
    channel_config_set_ring(&field_timing_dma_channel_config, false, 3);
    dma_channel_set_config(field_timing_dma_channel, &field_timing_dma_channel_config, false);
    dma_channel_transfer_from_buffer_now(field_timing_dma_channel, timing_short_sync_half_line,
                                         TIMING_SHORT_SYNC_HALF_LINE_LEN * VSYNC_LINES_PER_FIELD);
    break;
  case 2:
    // Top blank lines
    channel_config_set_ring(&field_timing_dma_channel_config, false, 3);
    dma_channel_set_config(field_timing_dma_channel, &field_timing_dma_channel_config, false);
    dma_channel_transfer_from_buffer_now(field_timing_dma_channel, timing_blank_line,
                                         TIMING_BLANK_LINE_LEN *
                                             (VERT_VISIBLE_START_LINE - VSYNC_LINES_PER_FIELD));

    break;
  case 3:
    // Visible lines
    channel_config_set_ring(&field_timing_dma_channel_config, false, 4);
    dma_channel_set_config(field_timing_dma_channel, &field_timing_dma_channel_config, false);
    dma_channel_transfer_from_buffer_now(field_timing_dma_channel, timing_visible_line,
                                         TIMING_VISIBLE_LINE_LEN * VISIBLE_LINES_PER_FIELD);
    break;
  case 4:
    // Bottom blank lines
    channel_config_set_ring(&field_timing_dma_channel_config, false, 3);
    dma_channel_set_config(field_timing_dma_channel, &field_timing_dma_channel_config, false);
    dma_channel_transfer_from_buffer_now(
        field_timing_dma_channel, timing_blank_line,
        TIMING_BLANK_LINE_LEN *
            (LINES_PER_FIELD - VERT_VISIBLE_START_LINE - VISIBLE_LINES_PER_FIELD));

    // Release the vblank semaphore which will wake anything waiting on it.
    sem_release(&vblank_semaphore);

    // Call the vertical blanking interval callback, if one is configured.
    if (vblank_callback != NULL) {
      vblank_callback();
    }
    break;
  }

  phase = (phase + 1) % 5;
}

// This function contains all static asserts. It's never called but the compiler will raise a
// diagnostic if the assertions fail.
static inline void all_static_asserts() {
  // Check that the number of *visible* dots per line is a multiple of 32.
  static_assert((VISIBLE_DOTS_PER_LINE & 0x1f) == 0);

  // Check that the number of *visible* lines per field is a multiple of 8.
  static_assert((VISIBLE_LINES_PER_FIELD & 0x7) == 0);

  // Statically assert alignment and length of timing programs. Alignment is necessary to allow DMA
  // in ring mode and the length needs to be known because we need to set the number of significan
  // bits in ring mode.
  static_assert(alignof(timing_long_sync_half_line) == sizeof(timing_long_sync_half_line));
  static_assert(TIMING_LONG_SYNC_HALF_LINE_LEN == 2);
  static_assert(alignof(timing_short_sync_half_line) == sizeof(timing_short_sync_half_line));
  static_assert(TIMING_SHORT_SYNC_HALF_LINE_LEN == 2);
  static_assert(alignof(timing_blank_line) == sizeof(timing_blank_line));
  static_assert(TIMING_BLANK_LINE_LEN == 2);
  static_assert(alignof(timing_visible_line) == sizeof(timing_visible_line));
  static_assert(TIMING_VISIBLE_LINE_LEN == 4);
  static_assert(VERT_VISIBLE_START_LINE > VSYNC_LINES_PER_FIELD);
  static_assert(LINES_PER_FIELD > (VERT_VISIBLE_START_LINE + VISIBLE_LINES_PER_FIELD));
}

void tvout_init(PIO pio, bool byte_oriented_frame_buffer, uint sync_pin, uint video_pin) {
  // Record which PIO instance is used.
  pio_instance = pio;

  // Ensure IRQ 4 of the PIO is clear
  pio_interrupt_clear(pio_instance, 4);

  // Configure and enable output program
  video_output_offset = pio_add_program(pio_instance, &video_output_program);
  video_output_sm = pio_claim_unused_sm(pio_instance, true);
  video_output_program_init(pio_instance, video_output_sm, video_output_offset, video_pin,
                            DOT_CLOCK_FREQ);

  // Configure and enable timing program.
  line_timing_offset = pio_add_program(pio_instance, &line_timing_program);
  line_timing_sm = pio_claim_unused_sm(pio_instance, true);
  line_timing_program_init(pio_instance, line_timing_sm, line_timing_offset, sync_pin);

  // Configure frame timing DMA channel.
  field_timing_dma_channel = dma_claim_unused_channel(true);
  field_timing_dma_channel_config =
      get_field_timing_dma_channel_config(field_timing_dma_channel, pio_instance, line_timing_sm);
  dma_channel_set_write_addr(field_timing_dma_channel, &pio_instance->txf[line_timing_sm], false);
  dma_channel_set_irq0_enabled(field_timing_dma_channel, true);

  // Configure DMA channel for copying frame buffer to video output.
  video_dma_channel = dma_claim_unused_channel(true);
  dma_channel_config video_dma_channel_config = get_video_output_dma_channel_config(
      video_dma_channel, pio_instance, video_output_sm, byte_oriented_frame_buffer);
  dma_channel_set_config(video_dma_channel, &video_dma_channel_config, false);
  dma_channel_set_write_addr(video_dma_channel, &pio_instance->txf[video_output_sm], false);

  // Enable interrupt handler for field timing.
  irq_set_exclusive_handler(DMA_IRQ_0, field_timing_dma_handler);
}

void tvout_start(void) {
  sem_init(&vblank_semaphore, 0, 1);

  pio_sm_put(pio_instance, video_output_sm, VISIBLE_DOTS_PER_LINE - 1);
  pio_sm_set_enabled(pio_instance, video_output_sm, true);
  pio_sm_set_enabled(pio_instance, line_timing_sm, true);

  // Start field timing.
  irq_set_enabled(DMA_IRQ_0, true);
  field_timing_dma_handler();
}

void tvout_cleanup(void) {
  irq_set_enabled(DMA_IRQ_0, false);
  dma_channel_cleanup(video_dma_channel);
  dma_channel_unclaim(video_dma_channel);
  dma_channel_cleanup(field_timing_dma_channel);
  dma_channel_unclaim(field_timing_dma_channel);

  pio_sm_set_enabled(pio_instance, video_output_sm, false);
  pio_remove_program(pio_instance, &video_output_program, video_output_offset);
  pio_sm_unclaim(pio_instance, video_output_sm);
  pio_sm_set_enabled(pio_instance, line_timing_sm, false);
  pio_remove_program(pio_instance, &line_timing_program, line_timing_offset);
  pio_sm_unclaim(pio_instance, line_timing_sm);
}

void tvout_set_vblank_callback(tvout_vblank_callback_t callback) { vblank_callback = callback; }

uint tvout_get_screen_width(void) { return VISIBLE_DOTS_PER_LINE; }

uint tvout_get_screen_height(void) { return VISIBLE_LINES_PER_FIELD; }

void tvout_set_frame_buffer(void *frame_buffer) {
  atomic_store(&frame_buffer_ptr, (uintptr_t)frame_buffer);
}

void tvout_wait_for_vblank(void) { sem_acquire_blocking(&vblank_semaphore); }
