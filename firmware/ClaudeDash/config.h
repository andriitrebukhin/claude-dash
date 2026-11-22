#pragma once
// Nothing here needs editing: Wi-Fi is set up from your phone on first boot,
// the Mac is found automatically (Bonjour), and pairing uses an on-screen code.

#define SETUP_AP_NAME    "Claude Dash Setup"   // Wi-Fi network shown during setup
#define POLL_MS          1000                  // how often to ask the bridge for state
#define BACKLIGHT_LEVEL  80                    // 0..100
#define RESET_BUTTON_PIN 0                     // hold BOOT for 5 s to forget Wi-Fi + pairing

// Optional: skip Bonjour discovery and use a fixed bridge address.
// #define BRIDGE_HOST_OVERRIDE "192.168.1.50:8787"
