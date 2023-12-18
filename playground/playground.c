#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "font.h"
#include "tvout.h"

#include "bronwen.h"

typedef struct {
  uint32_t *frame_buffer;
  uint width;
  uint height;
} draw_target_t;

draw_target_t get_tvout_draw_target(uint32_t *frame_buffer) {
  draw_target_t t = {
      .frame_buffer = frame_buffer,
      .width = tvout_get_screen_width(),
      .height = tvout_get_screen_height(),
  };
  return t;
}

void draw_byte_pattern(draw_target_t *target, uint8_t pattern, int x, int y) {
  if ((y < 0) || (y >= target->height)) {
    return;
  }

  if ((x <= -8) || (x >= target->width)) {
    return;
  }

  uint32_t *line = target->frame_buffer + (y * (target->width >> 5));

  if (x >= 0) {
    uint x_shift = x & 0x1f;
    if (x_shift <= 24) {
      uint32_t s = line[x >> 5];
      s &= ~(0xff << (24 - x_shift));
      s |= pattern << (24 - x_shift);
      line[x >> 5] = s;
    } else {
      // todo
    }
  } else {
    // todo
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

  tvout_init(pio0);

  uint32_t *frame_buffer = malloc(tvout_get_screen_height() * (tvout_get_screen_width() >> 5));
  for (int i = 0; i < tvout_get_screen_height() * (tvout_get_screen_width() >> 5); i++) {
    frame_buffer[i] = i;
  }
  tvout_set_frame_buffer(frame_buffer);

  printf("Starting TV-out with resolution of %d x %d...\n", tvout_get_screen_width(),
         tvout_get_screen_height());
  tvout_start();

  draw_target_t t = get_tvout_draw_target(frame_buffer);
  for (int i = 0; i < 32 * 3; i++) {
    draw_char(&t, i, (i & 0x1f) * 8, (i >> 5) * 8);
  }

  while (true) {
  }

  tvout_cleanup();
}
