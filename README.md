# Claude Dash

A desk display for Claude Code on the Waveshare **ESP32-S3-Touch-LCD-2.1** (480×480 round touchscreen).

- **Usage rings**: the outer ring is the 5-hour limit, the inner ring is the weekly limit. Each shows a reset countdown.
- **Sessions**: one row per Claude Code session, showing the project folder, what it's doing (tool and command or file), and how many subagents it's running. The dot shows the state: green = working, orange = needs you, amber = waiting, grey = idle.
- **Decisions**: when Claude Code asks for permission, the screen switches to a full-screen **Allow / Deny** card. If you don't answer within the timeout (30 s by default), the normal terminal prompt takes over.

```
Claude Code ──hooks──▶ claude_dash_hook.py ──▶ bridge.py (your Mac, :8787) ◀──Wi-Fi poll── ESP32
                                                   │
                                   api.anthropic.com/api/oauth/usage (usage limits)
```

## 1. Bridge (Mac)

Needs only Python 3 (standard library).

```bash
python3 bridge/bridge.py --demo     # fake data, to try the display / preview
python3 bridge/bridge.py            # real mode
```

When it starts, it prints the Mac's LAN address and a **display token**. These are stored in `~/.claude-dash/config.json`, where you can also change `decision_timeout` and `port`. To see a browser preview of exactly what the display shows, with working Allow/Deny buttons, open http://127.0.0.1:8787/.

**Usage limits** come from the same endpoint Claude Code's `/usage` uses. The bridge reads your Claude Code login from the macOS Keychain (`Claude Code-credentials`). The first time, macOS will ask whether `security` may access it; choose *Always Allow*. The token never leaves your Mac except to go to api.anthropic.com. This endpoint is undocumented and could change; if it does, the rings show `--` and everything else keeps working.

## 2. Hooks (Claude Code)

```bash
python3 bridge/install_hooks.py          # adds hooks to ~/.claude/settings.json (backs it up first)
python3 bridge/install_hooks.py --uninstall
```

This registers `claude_dash_hook.py` for SessionStart/End, UserPromptSubmit, Pre/PostToolUse, PermissionRequest, Notification, Stop and SubagentStop. Hooks apply to new sessions, both in the terminal and in the desktop app's Code tab. If the bridge isn't running, the hook exits immediately and Claude Code behaves normally.

> Run the installer from the folder you'll keep the project in. The hook command uses an absolute path.

## 3. Firmware (ESP32-S3-Touch-LCD-2.1)

Tested to compile with **arduino-esp32 core 3.3.13**, LVGL 8.3.10 (from Waveshare's demo) and ArduinoJson 7.4. Waveshare's display driver does **not** build on core 4.x, so stay on 3.x.

**Get the vendor pieces** from https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1-Demo.zip:

- Copy `Arduino/libraries/lvgl` into your Arduino `libraries/` folder. Its config is `lvgl/src/lv_conf.h`. Set `LV_FONT_MONTSERRAT_20`, `_28` and `_40` to `1`. Without them the UI still works, but with smaller text.
- Copy only these driver files from `Arduino/examples/LVGL_Arduino/` into `firmware/ClaudeDash/`. They're git-ignored.
  `Display_ST7701.*`, `TCA9554PWR.*`, `I2C_Driver.*`, `Touch_CST820.*`, `LVGL_Driver.*`.
  Skip the others. In particular, `Wireless.*` won't build without the BLE library.

**Configure:**

- Copy `firmware/ClaudeDash/config.example.h` to `config.h`. It's git-ignored.
- Set Wi-Fi, `BRIDGE_HOST` (the IP the bridge printed, or `yourmac.local`), `BRIDGE_TOKEN` and `TZ_INFO`.

**Build and flash with arduino-cli:**

```bash
arduino-cli core install esp32:esp32@3.3.13
arduino-cli lib install ArduinoJson
cd firmware/ClaudeDash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" .
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" -p /dev/cu.usbserial-XXXX .
```

The board connects through a CH343 USB-serial chip, so the port appears as `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial-*`. In the Arduino IDE, use: *ESP32S3 Dev Module*, Flash 16MB, PSRAM **OPI PSRAM**, Partition *16M Flash (3MB APP/9.9MB FATFS)*.

`board.cpp` is the only file that touches Waveshare's drivers: init, backlight, and the buzzer (TCA9554 pin 8), which chirps when a new permission request arrives.

## Repo layout

```
bridge/
  bridge.py            Mac bridge: hook events, usage polling, /state, /decision, browser preview
  claude_dash_hook.py  the hook Claude Code runs for every event
  install_hooks.py     adds/removes the hooks in ~/.claude/settings.json
firmware/ClaudeDash/
  ClaudeDash.ino       setup/loop
  net.cpp              Wi-Fi + bridge polling task (core 0)
  ui.cpp / ui.h        LVGL 8.3 round UI: usage rings, session list, Allow/Deny overlay
  board.cpp / board.h  glue to Waveshare's drivers (the drivers are copied in, git-ignored)
  dash_state.h         shared state structs
  config.example.h     copy to config.h (git-ignored) for Wi-Fi + bridge settings
```

## Notes

- Port 8787 must be reachable from the ESP32. Allow Python through the macOS firewall if it asks.
- Only localhost may post hook events. The display needs the token to read state or send decisions.
- `--demo` creates a fake permission request every so often, so you can test the buttons.
