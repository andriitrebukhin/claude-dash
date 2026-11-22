// Claude Dash — desk display for Claude Code on the Waveshare ESP32-S3-Touch-LCD-2.1
// Shows plan usage limits, active Claude Code sessions, and lets you
// Allow/Deny permission requests from the touchscreen.
#include "board.h"
#include "ui.h"
#include "dash_state.h"

static DashState snap;
static uint32_t lastSeq = 0;

void setup() {
  Serial.setTxBufferSize(2048);   // DASH HELLO / Improv replies can be long; the default USB buffer truncates them
  Serial.begin(115200);
  board_init();
  ui_init();
  net_begin();
}

void loop() {
  static uint32_t lastUi = 0;
  if (millis() - lastUi > 200) {
    lastUi = millis();
    if (net_snapshot(snap, lastSeq)) ui_update(snap);
    ui_tick();
  }
  board_loop();
  delay(5);
}
