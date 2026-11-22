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

1. Arduino IDE, with the **esp32 by Espressif** core **3.x**. Board: *ESP32S3 Dev Module*, Flash 16MB, **PSRAM: OPI PSRAM**, Partition: *16M Flash (3MB APP/9.9MB FATFS)*.
2. Download Waveshare's demo: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1-Demo.zip
   - Copy its `Arduino/libraries/*` (LVGL **8.3.x** plus its `lv_conf.h`) into `~/Documents/Arduino/libraries/`.
   - Copy the driver `.cpp/.h` files from `Arduino/examples/01_LVGL_Arduino/` into `firmware/ClaudeDash/`. Copy everything except the `.ino` and the `LVGL_Example*` files.
3. Install **ArduinoJson** (v7) from Library Manager.
4. In `lv_conf.h`, enable `LV_FONT_MONTSERRAT_14`, `_20`, `_28` and `_40`. Other sizes also work, because the UI falls back to what's available.
5. Copy `firmware/ClaudeDash/config.example.h` to `config.h` (git-ignored) and set Wi-Fi, `BRIDGE_HOST` (the IP the bridge printed, or `yourmac.local`), `BRIDGE_TOKEN` and `TZ_INFO`.
6. Flash it.

`board.cpp` is the only file that touches Waveshare's drivers. If their demo version names the functions differently, copy the init calls from the demo's `setup()` into `board_init()`.

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
