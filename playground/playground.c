#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "font.h"
#include "tvout.h"

#include "bronwen.h"

#define GPIO_SYNC_PIN 16
#define GPIO_VIDEO_PIN 17

typedef struct {
  uint32_t *frame_buffer;
  uint width;
  uint height;
  size_t stride; // in words
} draw_target_t;

draw_target_t get_tvout_draw_target(uint32_t *frame_buffer) {
  draw_target_t t = {
      .frame_buffer = frame_buffer,
      .width = tvout_get_screen_width(),
      .height = tvout_get_screen_height(),
      .stride = tvout_get_screen_width() >> 5,
  };
  return t;
}

void draw_point(draw_target_t *target, uint8_t v, int x, int y) {
  if ((y < 0) || (y >= target->height)) {
    return;
  }

  if ((x < 0) || (x >= target->width)) {
    return;
  }

  uint32_t *line = target->frame_buffer + (y * target->stride);
  uint32_t s = line[x >> 5];
  uint bit = x & 0x1f;
  s &= ~(1 << (31 - bit));
  s |= ((v & 0x1) << (31 - bit));
  line[x >> 5] = s;
}

void draw_byte_pattern(draw_target_t *target, uint8_t pattern, int x, int y) {
  if ((y < 0) || (y >= target->height)) {
    return;
  }

  if ((x <= -8) || ((x > 0) && (x >= target->width))) {
    return;
  }

  if (x >= 0) {
    uint x_shift = x & 0x1f;
    if (x_shift <= 24) {
      // fast path: entirely within one word
      uint32_t *line = target->frame_buffer + (y * (target->width >> 5));
      uint32_t s = line[x >> 5];
      s &= ~(0xff << (24 - x_shift));
      s |= pattern << (24 - x_shift);
      line[x >> 5] = s;
      return;
    }
  }

  // we're on the slow path
  for (int b = 0; b < 8; b++) {
    draw_point(target, (pattern >> (7 - b)) & 0x1, x + b, y);
  }
}

void draw_char(draw_target_t *target, uint8_t ch_idx, int x, int y) {
  if (ch_idx >= (font_len >> 3)) {
    return;
  }

  for (int r = 0; r < 8; r++) {
    draw_byte_pattern(target, font[(ch_idx << 3) + r], x, y + r);
  }
}

int main() {
  stdio_init_all();
  puts("Starting...");

  tvout_init(pio0, GPIO_SYNC_PIN, GPIO_VIDEO_PIN);

  const size_t frame_buffer_size = tvout_get_screen_height() * (tvout_get_screen_width() >> 3);
  uint32_t *frame_buffer = malloc(frame_buffer_size);
  tvout_set_frame_buffer(frame_buffer);
  draw_target_t t = get_tvout_draw_target(frame_buffer);

  printf("Starting TV-out with resolution of %d x %d...\n", tvout_get_screen_width(),
         tvout_get_screen_height());
  tvout_start();

  memset(frame_buffer, 0xff, frame_buffer_size);
  for (int i = 0; i < 32 * 3; i++) {
    draw_char(&t, i, (i & 0x1f) * 8, (i >> 5) * 8);
  }

  while (true) {
  }

  tvout_cleanup();
}
