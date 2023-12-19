#include "pico/types.h"
#include "hardware/pio.h"

// Callback to be notified of video blanking period start.
typedef void (*tvout_vblank_callback_t) (void);

// TV-out uses two DMA channels claimed via dma_claim_unused_channel(), DMA IRQ 0, two PIO state
// machines and IRQ for the PIO instance containing the state machines. Pass a PIO instance to
// tvout_init() to specify which instance is used.
//
// If big_endian_frame_buffer is true then the frame buffer is byte-oriented so that the MSB of the
// first byte in memory is the top-left most pixel. If false then the frame buffer is word oriented
// so that the MSB of the first *word* in memory is the top-left most pixel. Note that the pico is
// little-endian and so, in this case, the top-left most pixel corresponds to the MSB of the
// *fourth* byte in memory.
void tvout_init(PIO pio, bool byte_oriented_frame_buffer, uint sync_pin, uint video_pin);

// Start TV-out. tvout_init() must have been called first.
void tvout_start(void);

// Cleanup TV-out after tvout_init().
void tvout_cleanup(void);

// Get screen resolution.
uint tvout_get_screen_width(void);
uint tvout_get_screen_height(void);

// Set vblank callback. Pass NULL to disable.
void tvout_set_vblank_callback(tvout_vblank_callback_t callback);

// Frame buffer is big-endian within a 32-bit word so the MSB of the word is the left-most pixel.
// Note that the pico itself is little-endian and so, with an array of bytes, the first byte in
// memory is the right-most group of 8 pixels.
void tvout_set_frame_buffer(void *frame_buffer);

// Wait until the next vblank interval
void tvout_wait_for_vblank(void);
