// Glue to Waveshare's own drivers for the ESP32-S3-Touch-LCD-2.1.
//
// Copy every .cpp/.h driver file from the Waveshare demo
//   ESP32-S3-Touch-LCD-2.1-Demo/Arduino/examples/LVGL_Arduino/
// into this sketch folder (everything except LVGL_Arduino.ino and the
// LVGL_Example.* files). They handle the ST7701 RGB panel init through the
// TCA9554 expander, the CST820 touch, the backlight and the LVGL port.
#include <Arduino.h>
#include "config.h"
#include "board.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "LVGL_Driver.h"

#define BUZZER_EXIO EXIO_PIN8

void board_init() {
  I2C_Init();
  TCA9554PWR_Init(0x00);
  Set_EXIO(BUZZER_EXIO, Low);   // buzzer off
  LCD_Init();                   // ST7701 reset+init, CST820 touch, backlight
  Set_Backlight(BACKLIGHT_LEVEL);
  Lvgl_Init();
  lv_obj_clean(lv_scr_act());   // Waveshare's Lvgl_Init() adds a "Hello Ardino and LVGL!" demo label
}

void board_set_backlight(uint8_t level) {
  Set_Backlight(level);
}

void board_loop() {
  Lvgl_Loop();
}

static void beepOff(lv_timer_t *t) {
  Set_EXIO(BUZZER_EXIO, Low);
  lv_timer_del(t);
}

void board_beep(uint16_t ms) {
  Set_EXIO(BUZZER_EXIO, High);
  lv_timer_create(beepOff, ms, nullptr);
}
