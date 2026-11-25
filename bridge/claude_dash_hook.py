#!/usr/bin/env python3
"""Claude Code hook -> Claude Dash bridge.

Registered for every hook event by the Claude Dash plugin. Forwards the event
JSON to the local bridge, starting the bridge in the background first if it
isn't running. For PermissionRequest it waits for the answer from the desk
display; if nobody answers in time it prints nothing, so Claude Code shows its
normal prompt. Never fails loudly: any error just exits 0.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

PORT = int(os.environ.get("CLAUDE_DASH_PORT", "8787"))
STATE_DIR = Path.home() / ".claude-dash"
BRIDGE = Path(__file__).resolve().parent / "bridge.py"
LOGIN_LABEL = "com.claude-dash.bridge"
LOGIN_PLIST = Path.home() / "Library" / "LaunchAgents" / f"{LOGIN_LABEL}.plist"


def login_item_plist():
    # Exits 0 if the plugin was removed, so launchd doesn't keep retrying a missing file.
    script = f'test -f "{BRIDGE}" && exec "{sys.executable}" "{BRIDGE}" --port {PORT} || exit 0'
    log = STATE_DIR / "bridge.log"
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>{LOGIN_LABEL}</string>
  <key>ProgramArguments</key>
  <array><string>/bin/sh</string><string>-c</string><string>{script}</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>ThrottleInterval</key><integer>30</integer>
  <key>StandardOutPath</key><string>{log}</string>
  <key>StandardErrorPath</key><string>{log}</string>
</dict>
</plist>
"""


def ensure_login_item():
    """Keep the bridge's login item (a LaunchAgent) in sync with this plugin's location.
    Runs on SessionStart. Turn it off with "start_at_login": false in ~/.claude-dash/config.json."""
    if sys.platform != "darwin":
        return
    try:
        conf = json.loads((STATE_DIR / "config.json").read_text())
    except Exception:
        conf = {}
    domain = f"gui/{os.getuid()}"
    if conf.get("start_at_login", True) is False:
        if LOGIN_PLIST.exists():
            subprocess.run(["launchctl", "bootout", f"{domain}/{LOGIN_LABEL}"], capture_output=True)
            LOGIN_PLIST.unlink()
        return
    want = login_item_plist()
    if LOGIN_PLIST.exists() and LOGIN_PLIST.read_text() == want:
        return
    STATE_DIR.mkdir(exist_ok=True)
    LOGIN_PLIST.parent.mkdir(parents=True, exist_ok=True)
    changed_path = LOGIN_PLIST.exists()          # e.g. the plugin moved after an update
    subprocess.run(["launchctl", "bootout", f"{domain}/{LOGIN_LABEL}"], capture_output=True)
    LOGIN_PLIST.write_text(want)
    if changed_path:
        stop_running_bridge()                    # hand over to the bridge at the new path
    subprocess.run(["launchctl", "bootstrap", domain, str(LOGIN_PLIST)], capture_output=True)


def stop_running_bridge():
    try:
        out = subprocess.run(["lsof", "-t", "-nP", f"-iTCP:{PORT}", "-sTCP:LISTEN"],
                             capture_output=True, text=True, timeout=5).stdout.split()
        for pid in out:
            os.kill(int(pid), 15)
        time.sleep(0.5)
    except Exception:
        pass


def post(raw, timeout):
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}/hook", data=raw.encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


def start_bridge():
    """Launch bridge.py detached, at most once every 20 s."""
    STATE_DIR.mkdir(exist_ok=True)
    stamp = STATE_DIR / "spawn.stamp"
    try:
        if time.time() - stamp.stat().st_mtime < 20:
            return False
    except FileNotFoundError:
        pass
    stamp.touch()
    if sys.platform == "darwin" and LOGIN_PLIST.exists():
        # launchd owns the bridge: ask it to (re)start it rather than running a second copy
        r = subprocess.run(["launchctl", "kickstart", f"gui/{os.getuid()}/{LOGIN_LABEL}"], capture_output=True)
        if r.returncode == 0:
            return True
    log = open(STATE_DIR / "bridge.log", "a")
    subprocess.Popen([sys.executable, str(BRIDGE), "--port", str(PORT)],
                     stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                     start_new_session=True, close_fds=True)
    return True


def main():
    if os.environ.get("CLAUDE_DASH_SKIP"):      # the bridge's own `claude -p /usage` refresh runs
        return
    try:
        raw = sys.stdin.read()
        ev = json.loads(raw)
    except Exception:
        return
    if ev.get("hook_event_name") == "SessionStart":
        try:
            ensure_login_item()
        except Exception:
            pass
    is_perm = ev.get("hook_event_name") == "PermissionRequest"
    # the bridge enforces the real decision timeout; this is just a safety net
    timeout = 600 if is_perm else 1.5
    try:
        resp = post(raw, timeout)
    except urllib.error.URLError as e:
        if not isinstance(getattr(e, "reason", None), ConnectionRefusedError) or not start_bridge():
            return
        resp = None
        for _ in range(10):          # give the fresh bridge up to ~2.5 s to come up
            time.sleep(0.25)
            try:
                resp = post(raw, timeout)
                break
            except Exception:
                continue
        if resp is None:
            return
    except Exception:
        return
    decision = resp.get("decision")
    if not is_perm or not isinstance(decision, dict):
        return   # no answer from the display: Claude Code shows its normal prompt
    if decision.get("behavior") == "allow":
        d = {"behavior": "allow"}
        if isinstance(decision.get("updated_input"), dict):
            d["updatedInput"] = decision["updated_input"]
    elif decision.get("behavior") == "deny":
        d = {"behavior": "deny", "message": "Denied from the Claude Dash desk display."}
    else:
        return
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": d}}))


if __name__ == "__main__":
    main()
    sys.exit(0)
