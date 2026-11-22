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
    log = open(STATE_DIR / "bridge.log", "a")
    subprocess.Popen([sys.executable, str(BRIDGE), "--port", str(PORT)],
                     stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                     start_new_session=True, close_fds=True)
    return True


def main():
    try:
        raw = sys.stdin.read()
        ev = json.loads(raw)
    except Exception:
        return
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
