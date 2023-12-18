#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "tvout.h"

#include "bronwen.h"

int main() {
  stdio_init_all();
  puts("Starting...");

  tvout_init(pio0);

  uint32_t *frame_buffer = malloc(tvout_get_screen_height() * (tvout_get_screen_width() >> 5));
  for(int i=0; i<tvout_get_screen_height() * (tvout_get_screen_width() >> 5); i++) {
    frame_buffer[i] = i;
  }
  tvout_set_frame_buffer(frame_buffer);

  printf("Starting TV-out with resolution of %d x %d...\n", tvout_get_screen_width(),
         tvout_get_screen_height());
  tvout_start();

  while (true) {
    sleep_ms(2000);
    tvout_set_frame_buffer(bronwen_data);
    sleep_ms(2000);
    tvout_set_frame_buffer(frame_buffer);
  }

  tvout_cleanup();
}
