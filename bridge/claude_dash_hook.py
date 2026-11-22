#!/usr/bin/env python3
"""Claude Code hook -> Claude Dash bridge.

Registered for every hook event by install_hooks.py. Forwards the event JSON to
the local bridge. For PermissionRequest it waits for the answer from the desk
display and returns it to Claude Code; if the bridge is not running or nobody
answers in time it prints nothing, so Claude Code shows its normal prompt.
Never fails loudly: any error just exits 0.
"""
import json
import os
import sys
import urllib.request

PORT = int(os.environ.get("CLAUDE_DASH_PORT", "8787"))


def main():
    try:
        raw = sys.stdin.read()
        ev = json.loads(raw)
    except Exception:
        return
    is_perm = ev.get("hook_event_name") == "PermissionRequest"
    try:
        req = urllib.request.Request(
            f"http://127.0.0.1:{PORT}/hook", data=raw.encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        # the bridge enforces the real decision timeout; this is just a safety net
        with urllib.request.urlopen(req, timeout=600 if is_perm else 1.5) as r:
            resp = json.loads(r.read() or b"{}")
    except Exception:
        return
    decision = resp.get("decision")
    if is_perm and decision in ("allow", "deny"):
        d = {"behavior": decision}
        if decision == "deny":
            d["message"] = "Denied from the Claude Dash desk display."
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": d}}))


if __name__ == "__main__":
    main()
    sys.exit(0)
