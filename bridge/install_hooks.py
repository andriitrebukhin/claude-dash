#!/usr/bin/env python3
"""Add (or remove with --uninstall) the Claude Dash hooks in ~/.claude/settings.json.

A timestamped backup of settings.json is written before any change.
"""
import json
import shutil
import sys
import time
from pathlib import Path

SETTINGS = Path.home() / ".claude" / "settings.json"
CONF = Path.home() / ".claude-dash" / "config.json"
HOOK = Path(__file__).resolve().parent / "claude_dash_hook.py"
MARK = "claude_dash_hook.py"

TOOL_EVENTS = ["PreToolUse", "PostToolUse", "PermissionRequest"]       # take a matcher
PLAIN_EVENTS = ["SessionStart", "SessionEnd", "UserPromptSubmit", "Notification", "Stop", "SubagentStop"]


def strip(settings):
    hooks = settings.get("hooks", {})
    for ev in list(hooks):
        groups = []
        for g in hooks[ev]:
            g = dict(g, hooks=[h for h in g.get("hooks", []) if MARK not in h.get("command", "")])
            if g["hooks"]:
                groups.append(g)
        if groups:
            hooks[ev] = groups
        else:
            del hooks[ev]
    if not hooks:
        settings.pop("hooks", None)
    return settings


def main():
    settings = json.loads(SETTINGS.read_text()) if SETTINGS.exists() else {}
    if SETTINGS.exists():
        backup = SETTINGS.with_name(f"settings.json.bak-claude-dash-{int(time.time())}")
        shutil.copy2(SETTINGS, backup)
        print(f"Backup: {backup}")
    settings = strip(settings)
    if "--uninstall" not in sys.argv:
        timeout = 30
        if CONF.exists():
            timeout = json.loads(CONF.read_text()).get("decision_timeout", 30)
        cmd = f'python3 "{HOOK}"'
        hooks = settings.setdefault("hooks", {})
        for ev in TOOL_EVENTS + PLAIN_EVENTS:
            entry = {"type": "command", "command": cmd,
                     "timeout": timeout + 15 if ev == "PermissionRequest" else 5}
            group = {"hooks": [entry]}
            if ev in TOOL_EVENTS:
                group = {"matcher": "*", **group}
            hooks.setdefault(ev, []).append(group)
        print(f"Installed Claude Dash hooks for {len(TOOL_EVENTS + PLAIN_EVENTS)} events -> {HOOK}")
    else:
        print("Removed Claude Dash hooks.")
    SETTINGS.parent.mkdir(exist_ok=True)
    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n")


if __name__ == "__main__":
    main()
