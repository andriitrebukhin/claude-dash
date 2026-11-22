#pragma once
#include <stdint.h>
void board_init();   // I2C, IO expander, ST7701 panel, touch, backlight, LVGL
void board_loop();   // LVGL timer handler
void board_beep(uint16_t ms = 60);   // short buzzer chirp (non-blocking, call from the LVGL thread)
