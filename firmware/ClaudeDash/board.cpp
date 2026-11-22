// Glue to Waveshare's own drivers for the ESP32-S3-Touch-LCD-2.1.
//
// Copy every .cpp/.h driver file from the Waveshare demo
//   ESP32-S3-Touch-LCD-2.1-Demo/Arduino/examples/01_LVGL_Arduino/
// into this sketch folder (everything except 01_LVGL_Arduino.ino and the
// LVGL_Example.* files). They handle the ST7701 RGB panel init through the
// TCA9554 expander, the CST820 touch, the backlight and the LVGL port.
//
// If your demo version uses different function names, copy the init calls
// from that demo's setup() (everything before the example UI is created).
#include <Arduino.h>
#include "config.h"
#include "board.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "LVGL_Driver.h"

void board_init() {
  I2C_Init();
  TCA9554PWR_Init(0x00);
  LCD_Init();                 // panel + touch + backlight
  Set_Backlight(BACKLIGHT_LEVEL);
  Lvgl_Init();
}

void board_loop() {
  Lvgl_Loop();
}
