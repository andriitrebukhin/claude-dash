#pragma once
// ---- Edit these ----
#define WIFI_SSID      "your-wifi"
#define WIFI_PASS      "your-wifi-password"

// Your Mac running bridge.py: its LAN IP (printed by the bridge) or "<hostname>.local"
#define BRIDGE_HOST    "192.168.1.50"
#define BRIDGE_PORT    8787
// Printed by bridge.py on start (also in ~/.claude-dash/config.json)
#define BRIDGE_TOKEN   "paste-token-here"

// POSIX TZ string for the clock, e.g. "EET-2EEST,M3.5.0/3,M10.5.0/4" (Kyiv),
// "CET-1CEST,M3.5.0,M10.5.0/3" (Berlin), "PST8PDT,M3.2.0,M11.1.0" (California)
#define TZ_INFO        "UTC0"

#define POLL_MS          1000   // how often to ask the bridge for state
#define BACKLIGHT_LEVEL  80     // 0..100
