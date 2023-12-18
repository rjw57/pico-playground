#include "pico/types.h"
#include "hardware/pio.h"

// TV-out uses two DMA channels claimed via dma_claim_unused_channel(), DMA IRQ 0, two PIO state
// machines and IRQ for the PIO instance containing the state machines. Pass a PIO instance to
// tvout_init() to specify which instance is used.
void tvout_init(PIO pio);

// Start TV-out. tvout_init() must have been called first.
void tvout_start(void);

// Cleanup TV-out after tvout_init().
void tvout_cleanup(void);

// Get screen resolution.
uint tvout_get_screen_width(void);
uint tvout_get_screen_height(void);

// Frame buffer is big-endian within a 32-bit word so the MSB of the word is the left-most pixel.
// Note that the pico itself is little-endian and so, with an array of bytes, the first byte in
// memory is the right-most group of 8 pixels.
void tvout_set_frame_buffer(void *frame_buffer);
