#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "font.h"
#include "tvout.h"

#include "family.h"

#define GPIO_SYNC_PIN 16
#define GPIO_VIDEO_PIN 17

int main() {
  stdio_init_all();
  puts("Starting...");

  tvout_init(pio0, true, GPIO_SYNC_PIN, GPIO_VIDEO_PIN);
  tvout_set_frame_buffer(family);
  tvout_start();

  while (true) {
  }

  tvout_cleanup();
}
