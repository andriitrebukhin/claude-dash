# Claude Dash

A desk display for Claude Code on the Waveshare **ESP32-S3-Touch-LCD-2.1** (480×480 round touchscreen).

- **Usage rings**: the outer ring is the 5-hour limit, the inner ring is the weekly limit. Each shows a reset countdown.
- **Sessions**: one row per Claude Code session, showing the project folder, what it's doing (tool and command or file), and how many subagents it's running. The dot shows the state: green = working, orange = needs you, amber = waiting, grey = idle.
- **Decisions**: when Claude Code asks for permission, the display chirps and shows a full-screen **Allow / Deny** card. If you don't answer within the timeout (30 s by default), the normal terminal prompt takes over.

```
Claude Code ──hooks──▶ bridge.py (your Mac, :8787) ◀──Wi-Fi── ESP32 display
   (desk-display plugin)    │  Bonjour: _claude-dash._tcp  (display finds the Mac by itself)
                            └─▶ api.anthropic.com/api/oauth/usage (usage limits)
```

## Setup

### 1. Mac: install the plugin (once)

In Claude Code:

```
/plugin marketplace add /path/to/claude-dash
/plugin install desk-display@desk-display
```

That's all you need to do on the Mac. The plugin's hooks start the bridge in the background the first time Claude Code runs, so there's no daemon to set up. The bridge needs only `python3`, which comes with Xcode Command Line Tools, and it uses only the standard library.

The first time the bridge runs, expect two macOS prompts:
- **Firewall**: allow Python to accept incoming connections, so the display can reach it.
- **Keychain**: allow `security` to read `Claude Code-credentials`. This is how the bridge gets your usage limits. The token never leaves your Mac except to go to api.anthropic.com. The usage endpoint is undocumented and could change; if it does, the rings show `--` and everything else keeps working.

### 2. Display: first boot

1. **Wi-Fi.** The display shows a QR code. Scan it, or join **"Claude Dash Setup"** on your phone. A setup page opens; pick your Wi-Fi (2.4 GHz) and enter the password.
2. **Find the Mac.** The display finds the bridge over Bonjour automatically. You don't need an IP address, and it keeps working if the Mac's IP changes.
3. **Pair.** The display shows a 4-digit code, and a page opens on your Mac with the same code. Click **Approve**. The display remembers the pairing.

To start over, **hold BOOT for 5 seconds** while the display is running. It forgets the Wi-Fi and the pairing.

## Building the firmware

There's nothing to configure before flashing. `config.h` has no secrets, so one build works for any Wi-Fi and any Mac.

The build was tested with **arduino-esp32 core 3.3.13**, LVGL 8.3.10 (from Waveshare's demo) and ArduinoJson 7.4. Waveshare's display driver does **not** build on core 4.x.

1. Download Waveshare's demo: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1-Demo.zip
   - Copy `Arduino/libraries/lvgl` into your Arduino `libraries/` folder. In `lvgl/src/lv_conf.h`, set these to `1`: `LV_FONT_MONTSERRAT_20`, `_28`, `_40` and `LV_USE_QRCODE`. Without them the UI still works, but with smaller text and no QR code.
   - Copy these driver files from `Arduino/examples/LVGL_Arduino/` into `firmware/ClaudeDash/` (they're git-ignored): `Display_ST7701.*`, `TCA9554PWR.*`, `I2C_Driver.*`, `Touch_CST820.*`, `LVGL_Driver.*`.
2. Build and flash:

```bash
arduino-cli core install esp32:esp32@3.3.13
arduino-cli lib install ArduinoJson
cd firmware/ClaudeDash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" .
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" -p /dev/cu.usbmodem1101 .
```

The board shows up as `/dev/cu.usbmodem*` (the ESP32-S3's native USB); run `arduino-cli board list` to find it. `CDCOnBoot=cdc` sends the serial log over that same USB cable. In the Arduino IDE, use: *ESP32S3 Dev Module*, Flash 16MB, PSRAM **OPI PSRAM**, Partition *16M Flash (3MB APP/9.9MB FATFS)*, *USB CDC On Boot: Enabled*.

## Repo layout

```
.claude-plugin/          plugin + marketplace manifests (plugin name: desk-display)
hooks/hooks.json         Claude Code hooks -> bridge/claude_dash_hook.py
bridge/
  bridge.py              hook events, usage polling, Bonjour advert, pairing, /state, /decision, preview page
  claude_dash_hook.py    runs on each hook event; starts the bridge if it isn't running
firmware/ClaudeDash/
  ClaudeDash.ino         setup/loop
  net.cpp                Wi-Fi setup portal, Bonjour discovery, pairing, state polling (core 0)
  ui.cpp / ui.h          LVGL 8.3 round UI: usage rings, sessions, Allow/Deny, setup and pairing screens
  board.cpp / board.h    glue to Waveshare's drivers (the drivers are copied in, git-ignored)
  dash_state.h           shared state structs
  config.h               optional tweaks (backlight, poll rate, fixed bridge address)
```

## Notes

- **Preview and pairing page:** http://127.0.0.1:8787/ shows what the display shows and lists pending pairing requests. It opens by itself when a display asks to pair.
- **Bridge settings:** these are in `~/.claude-dash/config.json` (port, `decision_timeout`, paired displays). To revoke a display, delete its entry under `devices`. The bridge log is `~/.claude-dash/bridge.log`.
- **Security:** only localhost can post hook events or approve pairings. A display needs its paired token to read state or answer permission requests.
- **Raising `decision_timeout`:** if you set it above ~55 s, also raise the `PermissionRequest` timeout in `hooks/hooks.json`.
- **Demo mode:** `python3 bridge/bridge.py --demo` generates fake sessions and permission requests for testing the display.
- **Networks that block Bonjour:** on guest or office Wi-Fi with client isolation, set `BRIDGE_HOST_OVERRIDE` in `config.h`.
