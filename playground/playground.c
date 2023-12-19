#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/pio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "font.h"
#include "tvout.h"

#include "family.h"

#define GPIO_SYNC_PIN 16
#define GPIO_VIDEO_PIN 17

uint8_t *frame_buffer;
uint width, height, stride;

uint cursor_row, cursor_col;
bool cursor_shown;

#define console_rows() (height >> 3)
#define console_cols() (width >> 3)

void console_reset(void);
void console_putc(char c);
void console_line_feed(void);
void console_carriage_return(void);
void console_refresh(void);

void console_intl_toggle_cursor(void);

void console_reset(void) {
  cursor_row = cursor_col = 0;
  cursor_shown = false;
}

void console_intl_toggle_cursor(void) {
  uint8_t *dest = frame_buffer + cursor_col + (6 + (cursor_row << 3)) * stride;
  for (int i = 0; i < 2; i++, dest += stride) {
    *dest ^= 0xFF;
  }
}

void console_putc(char c) {
  bool cursor_was_shown = cursor_shown;
  if (cursor_was_shown) {
    console_intl_toggle_cursor();
    cursor_shown = false;
  }

  if ((c >= 32) && (c < 127)) {
    uint8_t *char_rows = font + ((c - 32) << 3);
    uint8_t *dest = frame_buffer + cursor_col + (cursor_row << 3) * stride;
    for (int i = 0; i < 8; i++, char_rows++, dest += stride) {
      *dest = *char_rows;
    }

    cursor_col += 1;
    if (cursor_col >= console_cols()) {
      console_carriage_return();
      console_line_feed();
    }
  } else if (c == 0x0A) {
    console_line_feed();
  } else if (c == 0x0D) {
    console_carriage_return();
  }

  if (cursor_was_shown) {
    console_intl_toggle_cursor();
    cursor_shown = true;
  }
}

void console_carriage_return(void) {
  bool cursor_was_shown = cursor_shown;
  if (cursor_was_shown) {
    console_intl_toggle_cursor();
    cursor_shown = false;
  }

  cursor_col = 0;

  if (cursor_was_shown) {
    console_intl_toggle_cursor();
    cursor_shown = true;
  }
}

void console_line_feed(void) {
  bool cursor_was_shown = cursor_shown;
  if (cursor_was_shown) {
    console_intl_toggle_cursor();
    cursor_shown = false;
  }

  cursor_row += 1;
  while (cursor_row >= console_rows()) {
    memmove(frame_buffer, frame_buffer + (stride << 3), (height - 8) * stride);
    memset(frame_buffer + (height - 8) * stride, 0x00, stride << 3);
    cursor_row--;
  }

  if (cursor_was_shown) {
    console_intl_toggle_cursor();
    cursor_shown = true;
  }
}

void console_refresh(void) {
  static uint frame_count = 0;
  if (frame_count == 0) {
    console_intl_toggle_cursor();
    cursor_shown = !cursor_shown;
  }
  frame_count = (frame_count + 1) & 0xf;
}

int main() {
  stdio_init_all();
  puts("Starting...");

  tvout_init(pio0, true, GPIO_SYNC_PIN, GPIO_VIDEO_PIN);

  width = tvout_get_screen_width();
  height = tvout_get_screen_height();
  stride = width >> 3;

  frame_buffer = malloc(stride * height);
  tvout_set_frame_buffer(frame_buffer);
  tvout_set_vblank_callback(console_refresh);

  tvout_start();
  console_reset();

  // memcpy(frame_buffer, family, stride * height);
  memset(frame_buffer, 0x00, stride * height);

  while (true) {
    char c = uart_getc(uart0);
    console_putc(c);
    uart_putc(uart0, c);
  }

  tvout_cleanup();
}
