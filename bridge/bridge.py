#!/usr/bin/env python3
"""Claude Dash bridge.

Collects Claude Code hook events (sessions, tools, permission requests) and
Claude plan usage limits, and serves them to the ESP32 desk display.

  python3 bridge.py            # normal mode
  python3 bridge.py --demo     # fake sessions/usage/permission requests for testing

Endpoints
  POST /hook       (localhost only)  hook events from claude_dash_hook.py;
                                     PermissionRequest blocks until a decision
  GET  /state      (device token or localhost)  compact JSON for the display
  POST /decision   (device token or localhost)  {"id": "...", "behavior": "allow"|"deny"}
  POST /pair/request  (LAN)          display asks to pair: {"device": "...", "code": "1234"}
  GET  /pair/status?req=...  (LAN)   display polls; gets its token once approved
  POST /pair/approve  (localhost)    {"req": "...", "approve": true|false}
  GET  /           (localhost only)  browser preview of the display + pairing approval

The bridge advertises itself over Bonjour as _claude-dash._tcp, so the display
finds it without knowing the Mac's IP.
"""
import argparse
import glob
import hashlib
import select
import termios
import atexit
import json
import os
import random
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

CONF_DIR = Path.home() / ".claude-dash"
CONF_FILE = CONF_DIR / "config.json"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

IDLE_PRUNE_SECS = 2 * 3600     # drop sessions with no events for this long
STALE_WORKING_SECS = 15 * 60   # "working" with no events this long -> idle


def load_conf():
    CONF_DIR.mkdir(exist_ok=True)
    conf = {}
    if CONF_FILE.exists():
        try:
            conf = json.loads(CONF_FILE.read_text())
        except Exception:
            conf = {}
    defaults = {
        "token": secrets.token_urlsafe(12),
        "port": 8787,
        "decision_timeout": 30,   # seconds the display gets before the terminal prompt takes over
        "question_timeout": 50,   # same for multiple-choice questions (keep below the hook timeout, 60)
        "usage_poll_secs": 120,
        "bridge_id": secrets.token_hex(4),
        "devices": {},            # paired displays: name -> token
        "usb_setup": True,        # give a plugged-in display your Wi-Fi + pairing over USB
        "start_at_login": True,   # keep the bridge running from login (LaunchAgent managed by the hook)
        "update_repo": "andriitrebukhin/claude-dash",   # GitHub repo whose releases the display updates from ("" = off)
    }
    changed = False
    for k, v in defaults.items():
        if k not in conf:
            conf[k] = v
            changed = True
    if changed:
        CONF_FILE.write_text(json.dumps(conf, indent=2))
        os.chmod(CONF_FILE, 0o600)
    return conf


def save_conf(conf):
    tmp = CONF_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(conf, indent=2))
    os.chmod(tmp, 0o600)
    tmp.replace(CONF_FILE)


def notify_pairing(port, device, code):
    """Pop up the approval page on the Mac (and a notification)."""
    if sys.platform != "darwin" or os.environ.get("CLAUDE_DASH_NO_POPUP"):
        return
    try:
        msg = f"Code {code} - approve it in the page that just opened"
        subprocess.Popen(["osascript", "-e", f'display notification "{msg}" with title "Claude Dash: pair {device}?"'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.Popen(["open", f"http://127.0.0.1:{port}/"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def advertise(conf):
    """Announce the bridge on the LAN via Bonjour (built into macOS)."""
    if sys.platform != "darwin":
        try:  # optional on Linux: pip install zeroconf
            from zeroconf import ServiceInfo, Zeroconf
            zc = Zeroconf()
            info = ServiceInfo("_claude-dash._tcp.local.", f"Claude Dash {socket.gethostname()}._claude-dash._tcp.local.",
                               addresses=[socket.inet_aton(lan_ip())], port=conf["port"],
                               properties={"id": conf["bridge_id"], "v": "1"})
            zc.register_service(info)
            atexit.register(zc.close)
        except Exception as e:  # noqa: BLE001
            print(f"Bonjour advert unavailable ({e}); set the bridge address on the display manually", flush=True)
        return
    name = f"Claude Dash {socket.gethostname().split('.')[0]}"
    proc = subprocess.Popen(["dns-sd", "-R", name, "_claude-dash._tcp", "local", str(conf["port"]),
                             f"id={conf['bridge_id']}", "v=1"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    atexit.register(proc.terminate)


DISCOVERY_UDP_PORT = 8788


def udp_discovery_loop(conf):
    """Answer 'CLAUDE-DASH?' broadcasts (fallback for networks where Bonjour doesn't get through)."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", DISCOVERY_UDP_PORT))
    except OSError as e:
        print(f"UDP discovery unavailable: {e}", flush=True)
        return
    reply = f"CLAUDE-DASH {conf['port']} {conf['bridge_id']}".encode()
    while True:
        try:
            data, addr = sock.recvfrom(256)
            if data.strip() == b"CLAUDE-DASH?":
                sock.sendto(reply, addr)
        except OSError:
            time.sleep(1)


def ascii_only(s):
    if not isinstance(s, str):
        return s
    s = s.replace("…", "...").replace("—", "-").replace("–", "-")
    s = s.replace("‘", "'").replace("’", "'").replace("“", '"').replace("”", '"')
    s = unicodedata.normalize("NFKD", s)
    return "".join(c for c in s if 32 <= ord(c) < 127 or c == "\n")


def asciify(obj):
    if isinstance(obj, dict):
        return {k: asciify(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [asciify(v) for v in obj]
    return ascii_only(obj)


def short_path(p):
    parts = str(p).rstrip("/").split("/")
    return "/".join(parts[-2:]) if len(parts) > 1 else p


def tool_label(name):
    name = name or ""
    if name.startswith("mcp__"):
        bits = name.split("__")
        server = bits[1] if len(bits) > 1 else "mcp"
        tool = bits[-1] if len(bits) > 2 else ""
        if len(server) > 14 and "-" in server:  # uuid-ish connector ids
            server = "mcp"
        return f"{server}:{tool}" if tool else server
    return name


def summarize_tool(name, inp):
    if not isinstance(inp, dict):
        return ""
    if name == "Bash":
        return inp.get("command", "")
    for k in ("file_path", "notebook_path", "path"):
        if isinstance(inp.get(k), str):
            return short_path(inp[k])
    for k in ("url", "query", "pattern", "description", "prompt", "skill"):
        if isinstance(inp.get(k), str):
            return inp[k]
    for v in inp.values():
        if isinstance(v, str) and v:
            return v
    return ""


def clip(s, n):
    s = " ".join(str(s or "").split()) if "\n" not in str(s or "") else str(s)
    return s if len(s) <= n else s[: n - 3] + "..."


class Dash:
    def __init__(self, conf):
        self.conf = conf
        self.lock = threading.Lock()
        self.sessions = {}
        self.pending = {}
        self.usage = {"ok": False, "error": "fetching..."}
        self.usage_at = 0
        self.pairings = {}   # req id -> {device, code, created, status, token}
        self.displays = {}       # device name -> {"fw", "ip", "seen"} from their /state polls
        self.firmware = None     # {"latest", "size", "md5", "path"} once a release is downloaded + verified
        self.usb_wifi_skip = {}  # device -> time before which we won't ask for the Wi-Fi password again

    # ---------- pairing ----------
    def pair_request(self, device, code):
        device = clip(ascii_only(str(device or "display")), 32)
        code = "".join(c for c in str(code or "") if c.isdigit())[:6]
        if len(code) < 4:
            return None
        with self.lock:
            now = time.time()
            for k, p in list(self.pairings.items()):
                if now - p["created"] > 600 or (p["device"] == device and p["status"] == "pending"):
                    del self.pairings[k]
            req = secrets.token_urlsafe(16)
            self.pairings[req] = {"device": device, "code": code, "created": now,
                                  "status": "pending", "token": None}
        print(f"Pairing request from {device}: code {code} - approve at http://127.0.0.1:{self.conf['port']}/", flush=True)
        notify_pairing(self.conf["port"], device, code)
        return req

    def pair_status(self, req):
        with self.lock:
            p = self.pairings.get(req)
            if not p:
                return {"status": "unknown"}
            out = {"status": p["status"]}
            if p["status"] == "approved":
                out["token"] = p["token"]
                out["bridge_id"] = self.conf["bridge_id"]
                del self.pairings[req]   # hand the token out exactly once
            return out

    def pair_decide(self, req, approve):
        with self.lock:
            p = self.pairings.get(req)
            if not p or p["status"] != "pending":
                return False
            if approve:
                tok = secrets.token_urlsafe(18)
                self.conf["devices"][p["device"]] = tok
                save_conf(self.conf)
                p.update(status="approved", token=tok)
            else:
                p["status"] = "denied"
            return True

    def pending_pairings(self):
        with self.lock:
            return [{"req": k, "device": p["device"], "code": p["code"]}
                    for k, p in self.pairings.items() if p["status"] == "pending"]

    def token_ok(self, tok):
        if not tok:
            return False
        valid = list(self.conf["devices"].values()) + [self.conf["token"]]
        return any(secrets.compare_digest(tok, v) for v in valid)

    # ---------- hook events ----------
    def _session(self, sid, cwd):
        s = self.sessions.get(sid)
        if s is None:
            s = {"id": sid, "cwd": cwd, "state": "idle", "tool": "", "detail": "",
                 "subagents": 0, "started": time.time(), "last": time.time()}
            self.sessions[sid] = s
        if cwd:
            s["cwd"] = cwd
        s["last"] = time.time()
        return s

    def _cancel_pending(self, sid, tool=None):
        for p in list(self.pending.values()):
            if p["sid"] == sid and (tool is None or p["tool_raw"] == tool):
                p["decision"] = None
                p["event"].set()

    def handle_event(self, ev):
        name = ev.get("hook_event_name", "")
        sid = ev.get("session_id") or "unknown"
        cwd = ev.get("cwd") or ""
        with self.lock:
            if name == "SessionEnd":
                self.sessions.pop(sid, None)
                self._cancel_pending(sid)
                return
            s = self._session(sid, cwd)
            tool = ev.get("tool_name", "")
            if name == "SessionStart":
                s.update(state="idle", tool="", detail="session started")
            elif name == "UserPromptSubmit":
                self._cancel_pending(sid)
                s.update(state="working", tool="", detail=clip(ev.get("prompt", ""), 90))
            elif name == "PreToolUse":
                s.update(state="working", tool=tool_label(tool),
                         detail=clip(summarize_tool(tool, ev.get("tool_input")), 90))
                if tool in ("Task", "Agent"):
                    s["subagents"] += 1
            elif name in ("PostToolUse", "PostToolUseFailure"):
                self._cancel_pending(sid, tool)  # answered in the terminal
                s["state"] = "working"
                if tool in ("Task", "Agent"):
                    s["subagents"] = max(0, s["subagents"] - 1)
            elif name == "Notification":
                msg = ev.get("message", "")
                if "permission" not in msg.lower():
                    s.update(state="waiting", detail=clip(msg, 90))
            elif name == "Stop":
                self._cancel_pending(sid)
                s.update(state="idle", tool="", subagents=0, detail="finished - waiting for you")

    def wait_permission(self, ev):
        """Block until the display answers. Returns None (no decision -> normal prompt)
        or {"behavior": "allow"|"deny", "updated_input": {...}}."""
        sid = ev.get("session_id") or "unknown"
        tool = ev.get("tool_name", "")
        tool_input = ev.get("tool_input") if isinstance(ev.get("tool_input"), dict) else {}
        pid = uuid.uuid4().hex[:10]
        p = {"id": pid, "sid": sid, "tool_raw": tool, "tool": tool_label(tool),
             "detail": clip(summarize_tool(tool, tool_input), 600),
             "input": tool_input, "questions": None, "timeout": self.conf["decision_timeout"],
             "created": time.time(), "event": threading.Event(), "decision": None}
        if tool == "AskUserQuestion" and isinstance(tool_input.get("questions"), list):
            p["questions"] = [{
                "q": clip(q.get("question", ""), 200), "h": clip(q.get("header", ""), 16),
                "multi": bool(q.get("multiSelect")),
                "opts": [clip(o.get("label", ""), 40) for o in (q.get("options") or [])[:4] if isinstance(o, dict)],
            } for q in tool_input["questions"][:4] if isinstance(q, dict)]
            p["tool"] = "Question"
            p["detail"] = clip(p["questions"][0]["q"] if p["questions"] else "", 600)
            p["timeout"] = max(self.conf["decision_timeout"], self.conf.get("question_timeout", 50))
        with self.lock:
            s = self._session(sid, ev.get("cwd") or "")
            s.update(tool=p["tool"], detail=clip(p["detail"], 90))
            self.pending[pid] = p
        got = p["event"].wait(p["timeout"])
        with self.lock:
            self.pending.pop(pid, None)
        return p["decision"] if got else None

    def decide(self, pid, behavior, answers=None):
        """behavior: allow | deny | skip (skip = let the normal prompt handle it).
        answers: for questions, one list of chosen option indexes per question."""
        with self.lock:
            p = self.pending.get(pid)
            if not p or behavior not in ("allow", "deny", "skip"):
                return False
            if behavior == "skip":
                p["decision"] = None
            elif behavior == "deny":
                p["decision"] = {"behavior": "deny"}
            elif p["questions"] is not None:
                qs = p["input"].get("questions") or []
                if not isinstance(answers, list) or len(answers) < len(p["questions"]):
                    return False
                out = {}
                for q, picked in zip(qs, answers):
                    labels = [o.get("label", "") for o in (q.get("options") or [])]
                    picked = picked if isinstance(picked, list) else [picked]
                    chosen = [labels[i] for i in picked if isinstance(i, int) and 0 <= i < len(labels)]
                    if not chosen:
                        return False
                    out[q.get("question", "")] = ", ".join(chosen)   # original labels, not the display's ASCII copy
                p["decision"] = {"behavior": "allow", "updated_input": dict(p["input"], answers=out)}
            else:
                # Claude Code wants the input echoed back for tools like ExitPlanMode
                p["decision"] = {"behavior": "allow", "updated_input": p["input"]}
            p["event"].set()
            return True

    # ---------- usage ----------
    def set_usage(self, usage):
        with self.lock:
            self.usage = usage
            self.usage_at = time.time()

    # ---------- snapshot ----------
    def snapshot(self):
        now = time.time()
        with self.lock:
            for sid, s in list(self.sessions.items()):
                if now - s["last"] > IDLE_PRUNE_SECS:
                    del self.sessions[sid]
                elif s["state"] == "working" and now - s["last"] > STALE_WORKING_SECS:
                    s.update(state="idle", tool="", subagents=0)
            pend_sids = {p["sid"] for p in self.pending.values()}
            order = {"decision": 0, "working": 1, "waiting": 2, "idle": 3}
            sessions = []
            for s in self.sessions.values():
                state = "decision" if s["id"] in pend_sids else s["state"]
                sessions.append({
                    "name": clip(os.path.basename(s["cwd"].rstrip("/")) or s["id"][:8], 30),
                    "state": state, "tool": clip(s["tool"], 26), "detail": clip(s["detail"], 70),
                    "subagents": s["subagents"], "idle_secs": int(now - s["last"]),
                    "_k": (order.get(state, 9), -s["last"]),
                })
            sessions.sort(key=lambda x: x.pop("_k"))
            names = {sid: os.path.basename(s["cwd"].rstrip("/")) or sid[:8] for sid, s in self.sessions.items()}
            pending = [dict({
                "id": p["id"], "session": clip(names.get(p["sid"], p["sid"][:8]), 30),
                "tool": clip(p["tool"], 26), "detail": p["detail"],
                "expires_in": max(0, int(p["timeout"] - (now - p["created"]))),
            }, **({"questions": p["questions"]} if p["questions"] is not None else {}))
                for p in sorted(self.pending.values(), key=lambda p: p["created"])]
            usage = dict(self.usage)
            # keep reset countdowns fresh between fetches
            for k in ("five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"):
                w = usage.get(k)
                if isinstance(w, dict) and w.get("resets_at"):
                    usage[k] = dict(w, resets_in=max(0, int(w["resets_at"] - now)))
        fw = self.firmware
        extra = {"firmware": {k: fw[k] for k in ("latest", "size", "md5")}} if fw else {}
        return {**extra, "ts": int(now), "clock": time.strftime("%H:%M"), "usage": usage, "sessions": sessions[:8], "pending": pending[:4]}


# ---------------- USB setup ----------------
MODE_PORTAL = 1  # matches DashMode in firmware/ClaudeDash/dash_state.h


def serial_open(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0                                   # iflag
    a[1] = 0                                   # oflag
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0                                   # lflag: raw
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def serial_ask(fd, line, expect, timeout):
    """Send a line, return the first received line starting with `expect`."""
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, (line + "\n").encode())
    buf, t0 = b"", time.time()
    while time.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                buf += os.read(fd, 4096)
            except OSError:
                pass
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                txt = raw.decode(errors="replace").strip()
                if txt.startswith(expect):
                    return txt[len(expect):].strip()
    return None


def preferred_networks():
    try:
        out = subprocess.run(["networksetup", "-listpreferredwirelessnetworks", "en0"],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return []
    return [ln.strip() for ln in out.splitlines()[1:] if ln.strip()]


def wifi_password(ssid):
    """Read a saved Wi-Fi password; macOS asks the user to authorize this."""
    for args in (["-D", "AirPort network password", "-a", ssid],
                 ["-s", "AirPort", "-a", ssid],
                 ["-a", ssid]):
        try:
            r = subprocess.run(["security", "find-generic-password", *args, "-w"],
                               capture_output=True, text=True, timeout=180)
        except Exception:
            continue
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.rstrip("\n")
        if r.returncode != 44:   # 44 = not found under this name; anything else (e.g. cancelled) -> stop asking
            print(f"USB: keychain said no (exit {r.returncode}): {r.stderr.strip()[:120]}", flush=True)
            return None
    return None


def notify(title, msg):
    if sys.platform == "darwin" and not os.environ.get("CLAUDE_DASH_NO_POPUP"):
        subprocess.Popen(["osascript", "-e", f"display notification {json.dumps(msg)} with title {json.dumps(title)}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def usb_setup_once(dash, path):
    """Set up a display on `path`. Returns True if it should be checked again later
    (it still needs Wi-Fi), False if it's done or isn't a Claude Dash display."""
    try:
        fd = serial_open(path)
    except OSError:
        return True    # busy (serial monitor / flashing) or gone: try again later
    try:
        hello = serial_ask(fd, "DASH?", "DASH HELLO", 2.0)
        if not hello:
            return False   # not a Claude Dash display
        info = json.loads(hello)
        device = clip(ascii_only(str(info.get("device") or "display")), 32)
        setup = {}
        if not info.get("paired") or info.get("bid") != dash.conf["bridge_id"]:
            tok = dash.conf["devices"].get(device) or secrets.token_urlsafe(18)
            with dash.lock:
                dash.conf["devices"][device] = tok
                save_conf(dash.conf)
            setup.update(token=tok, bid=dash.conf["bridge_id"])
        if info.get("mode") == MODE_PORTAL and time.time() > dash.usb_wifi_skip.get(device, 0):
            visible = set(info.get("ssids") or [])
            ssid = next((n for n in preferred_networks() if n in visible), None)
            if not ssid:
                print(f"USB: {device} needs Wi-Fi, but none of this Mac's saved networks are visible to it", flush=True)
            else:
                notify("Claude Dash", f"Sharing Wi-Fi \"{ssid}\" with the display - approve the macOS prompt")
                print(f"USB: sharing Wi-Fi '{ssid}' with {device} (macOS will ask to allow it)", flush=True)
                pw = wifi_password(ssid)
                if pw is None:
                    dash.usb_wifi_skip[device] = time.time() + 600   # don't re-prompt every minute
                    print("USB: couldn't read the Wi-Fi password (prompt denied?) - will ask again in 10 min", flush=True)
                else:
                    setup.update(ssid=ssid, **{"pass": pw})
        if setup:
            ok = serial_ask(fd, "DASH SETUP " + json.dumps(setup), "DASH OK", 3.0) is not None
            what = " + ".join(k for k in ("ssid", "token") if k in setup).replace("ssid", "Wi-Fi").replace("token", "pairing")
            print(f"USB: sent {what} to {device}: {'ok' if ok else 'no answer'}", flush=True)
            if ok and "token" in setup:
                notify("Claude Dash", f"Paired with {device} over USB")
            return "ssid" not in setup and info.get("mode") == MODE_PORTAL
        return info.get("mode") == MODE_PORTAL
    except Exception as e:  # noqa: BLE001
        print(f"USB: {path}: {e}", flush=True)
        return True
    finally:
        os.close(fd)


ESPRESSIF_VID = 0x303A   # ESP32-S3 built-in USB (the board's "USB" port)


def espressif_ports():
    """Serial ports that belong to an ESP32's own USB. UART adapters (CH343 etc.) are
    skipped: opening those can reset the board, and they'd collide with flashing."""
    try:
        out = subprocess.run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return set()
    ports, vid = set(), None
    for line in out.splitlines():
        if '"idVendor" = ' in line:
            try:
                vid = int(line.split("=")[1].strip())
            except ValueError:
                vid = None
        elif '"IOCalloutDevice" = ' in line and vid == ESPRESSIF_VID:
            ports.add(line.split("=")[1].strip().strip('"'))
    return ports


def usb_loop(dash):
    """Watch for a display plugged into this Mac's USB and set it up over the cable.
    Each port is checked once when it appears, and again every minute only while
    that display still needs Wi-Fi."""
    state = {}   # port -> {"at": last check, "again": re-check?}
    while True:
        ports = espressif_ports()
        for gone in set(state) - ports:
            del state[gone]
        for p in ports:
            st = state.get(p)
            if st is None or (st["again"] and time.time() - st["at"] > 60):
                needs_more = usb_setup_once(dash, p)
                state[p] = {"at": time.time(), "again": needs_more}
        time.sleep(3)


# ---------------- firmware updates ----------------
def http_get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "claude-dash-bridge", "Accept": "application/octet-stream, application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def check_firmware(dash):
    """Download the latest release's app image (verified against SHA256SUMS) for the display."""
    repo = dash.conf.get("update_repo")
    if not repo:
        return
    rel = json.loads(http_get(f"https://api.github.com/repos/{repo}/releases/latest", 20))
    tag = rel.get("tag_name", "")
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    name = f"claude-dash-{tag}-app.bin"
    if name not in assets or "SHA256SUMS" not in assets:
        return
    fwdir = CONF_DIR / "firmware"
    fwdir.mkdir(exist_ok=True)
    path = fwdir / name
    if not path.exists():
        sums = http_get(assets["SHA256SUMS"], 20).decode()
        expected = next((ln.split()[0] for ln in sums.splitlines() if ln.strip().endswith(name)), None)
        data = http_get(assets[name], 300)
        if not expected or hashlib.sha256(data).hexdigest() != expected:
            print(f"Firmware {tag}: checksum mismatch, ignoring", flush=True)
            return
        tmp = path.with_suffix(".tmp")
        tmp.write_bytes(data)
        tmp.replace(path)
        print(f"Firmware {tag} downloaded and verified", flush=True)
        for old in fwdir.glob("claude-dash-*-app.bin"):
            if old != path:
                old.unlink()
    data = path.read_bytes()
    with dash.lock:
        dash.firmware = {"latest": tag, "size": len(data), "md5": hashlib.md5(data).hexdigest(), "path": str(path)}


FIRMWARE_CHECK_MIN_GAP = 600   # seconds between release checks triggered by a display coming online
_fw_lock = threading.Lock()
_fw_last_check = [0.0]


def check_firmware_safe(dash):
    if not _fw_lock.acquire(blocking=False):
        return
    try:
        _fw_last_check[0] = time.time()
        check_firmware(dash)
    except Exception as e:  # noqa: BLE001
        print(f"Firmware check failed: {clip(str(e), 80)}", flush=True)
    finally:
        _fw_lock.release()


def check_firmware_soon(dash):
    """A display just came online (e.g. rebooted): look for a new release, rate-limited."""
    if time.time() - _fw_last_check[0] > FIRMWARE_CHECK_MIN_GAP:
        threading.Thread(target=check_firmware_safe, args=(dash,), daemon=True).start()


def firmware_loop(dash):
    while True:
        check_firmware_safe(dash)
        time.sleep(6 * 3600)


# ---------------- usage fetching ----------------
def read_oauth_token():
    raw = None
    if sys.platform == "darwin":
        r = subprocess.run(["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
                           capture_output=True, text=True, timeout=30)
        if r.returncode == 0:
            raw = r.stdout.strip()
    if not raw:
        f = Path.home() / ".claude" / ".credentials.json"
        if f.exists():
            raw = f.read_text()
    if not raw:
        raise RuntimeError("no Claude Code login found")
    return json.loads(raw)["claudeAiOauth"]["accessToken"]


def parse_window(w):
    if not isinstance(w, dict) or w.get("utilization") is None:
        return None
    out = {"pct": round(float(w["utilization"]), 1)}
    ra = w.get("resets_at")
    if ra:
        try:
            out["resets_at"] = datetime.fromisoformat(ra.replace("Z", "+00:00")).timestamp()
        except ValueError:
            pass
    return out


def usage_loop(dash):
    token = None
    backoff = 0
    while True:
        try:
            if token is None:
                token = read_oauth_token()
            req = urllib.request.Request(USAGE_URL, headers={
                "Authorization": f"Bearer {token}",
                "anthropic-beta": "oauth-2025-04-20",
                "Accept": "application/json",
                "User-Agent": "claude-dash/0.1",
            })
            with urllib.request.urlopen(req, timeout=15) as r:
                data = json.loads(r.read())
            usage = {"ok": True}
            for k in ("five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"):
                w = parse_window(data.get(k))
                if w:
                    usage[k] = w
            if "five_hour" not in usage and "seven_day" not in usage:
                usage = {"ok": False, "error": "no limits in response"}
            dash.set_usage(usage)
            backoff = 0
        except urllib.error.HTTPError as e:
            if e.code in (401, 403):
                token = None  # Claude Code refreshes its token when you use it; re-read next time
                dash.set_usage({"ok": False, "error": "login expired - open Claude Code"})
            else:
                dash.set_usage({"ok": False, "error": f"usage API HTTP {e.code}"})
                if e.code == 429:
                    backoff = min(backoff * 2 or 300, 1800)
        except Exception as e:  # noqa: BLE001
            dash.set_usage({"ok": False, "error": clip(str(e), 40)})
        time.sleep(dash.conf["usage_poll_secs"] + backoff)


# ---------------- demo mode ----------------
def demo_loop(dash):
    projects = [("api-server", "/Users/demo/api-server"), ("firmware", "/Users/demo/firmware"),
                ("website", "/Users/demo/website")]
    tools = [("Bash", {"command": "npm test -- --watch=false"}), ("Edit", {"file_path": "/x/src/ui/Panel.tsx"}),
             ("Read", {"file_path": "/x/README.md"}), ("Grep", {"pattern": "TODO"}),
             ("Agent", {"description": "Explore API routes"})]
    t0 = time.time()
    dash.set_usage({"ok": True, "five_hour": {"pct": 42.0, "resets_at": t0 + 2 * 3600 + 13 * 60},
                    "seven_day": {"pct": 18.0, "resets_at": t0 + 3 * 86400 + 4 * 3600}})
    for i, (n, cwd) in enumerate(projects):
        dash.handle_event({"hook_event_name": "SessionStart", "session_id": f"demo-{i}", "cwd": cwd})
    while True:
        i = random.randrange(len(projects))
        name, inp = random.choice(tools)
        dash.handle_event({"hook_event_name": random.choice(["PreToolUse", "PreToolUse", "Stop"]),
                           "session_id": f"demo-{i}", "cwd": projects[i][1], "tool_name": name, "tool_input": inp})
        if random.random() < 0.12 and not dash.pending:
            ev = {"session_id": "demo-0", "cwd": projects[0][1], "tool_name": "Bash",
                  "tool_input": {"command": "git push origin feature/display-dashboard"}}
            threading.Thread(target=lambda: print("demo decision:", dash.wait_permission(ev)), daemon=True).start()
        time.sleep(3)


# ---------------- HTTP ----------------
def make_handler(dash):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def _local(self):
            return self.client_address[0] in ("127.0.0.1", "::1", "::ffff:127.0.0.1")

        def _authed(self):
            return self._local() or dash.token_ok(self.headers.get("X-Token", ""))

        def _send(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                return json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                return {}

        def do_GET(self):
            u = urlparse(self.path)
            if u.path == "/state":
                if not self._authed():
                    return self._send(401, {"error": "bad token"})
                dev = self.headers.get("X-Device")
                if dev:
                    prev = dash.displays.get(clip(ascii_only(dev), 32))
                    if (prev is None or time.time() - prev["seen"] > 120) and dash.conf.get("update_repo"):
                        check_firmware_soon(dash)
                    dash.displays[clip(ascii_only(dev), 32)] = {
                        "fw": clip(ascii_only(self.headers.get("X-Fw", "")), 24),
                        "ip": self.client_address[0], "seen": int(time.time())}
                snap = dash.snapshot()
                if parse_qs(u.query).get("ascii"):
                    snap = asciify(snap)
                return self._send(200, snap)
            if u.path == "/firmware/app.bin":
                if not self._authed():
                    return self._send(401, {"error": "bad token"})
                fw = dash.firmware
                if not fw:
                    return self._send(404, {"error": "no firmware"})
                data = Path(fw["path"]).read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                for i in range(0, len(data), 16384):
                    self.wfile.write(data[i:i + 16384])
                return
            if u.path == "/pair/status":
                return self._send(200, dash.pair_status(parse_qs(u.query).get("req", [""])[0]))
            if u.path == "/displays" and self._local():
                now = int(time.time())
                return self._send(200, {k: dict(v, ago=now - v["seen"]) for k, v in dash.displays.items()})
            if u.path == "/pairings" and self._local():
                return self._send(200, dash.pending_pairings())
            if u.path == "/" and self._local():
                return self._send(200, PREVIEW_HTML.encode(), "text/html; charset=utf-8")
            self._send(404, {"error": "not found"})

        def do_POST(self):
            u = urlparse(self.path)
            if u.path == "/hook":
                if not self._local():
                    return self._send(403, {"error": "local only"})
                ev = self._body()
                if ev.get("hook_event_name") == "PermissionRequest":
                    return self._send(200, {"decision": dash.wait_permission(ev)})
                dash.handle_event(ev)
                return self._send(200, {})
            if u.path == "/pair/request":
                b = self._body()
                req = dash.pair_request(b.get("device"), b.get("code"))
                return self._send(200 if req else 400, {"req": req} if req else {"error": "bad code"})
            if u.path == "/pair/approve":
                if not self._local():
                    return self._send(403, {"error": "local only"})
                b = self._body()
                return self._send(200, {"ok": dash.pair_decide(str(b.get("req", "")), bool(b.get("approve")))})
            if u.path == "/decision":
                if not self._authed():
                    return self._send(401, {"error": "bad token"})
                b = self._body()
                ok = dash.decide(str(b.get("id", "")), b.get("behavior"), b.get("answers"))
                return self._send(200 if ok else 404, {"ok": ok})
            self._send(404, {"error": "not found"})

    return H


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


PREVIEW_HTML = r"""<!doctype html><html><head><meta charset="utf-8"><title>Claude Dash preview</title>
<style>
body{background:#000;margin:0;display:grid;place-items:center;height:100vh;font-family:-apple-system,Helvetica,sans-serif}
#d{width:480px;height:480px;border-radius:50%;background:#141413;position:relative;overflow:hidden;color:#FAF9F5}
svg{position:absolute;inset:0}
.c{position:absolute;left:0;right:0;text-align:center}
#pct{top:60px;font-size:40px;font-weight:600}#sub{top:110px;font-size:14px;color:#87867F}#wk{top:132px;font-size:14px;color:#D4A27F}
#list{position:absolute;top:164px;left:75px;width:330px;height:200px;overflow:auto;display:flex;flex-direction:column;gap:6px}
.row{background:#1F1E1D;border-radius:12px;padding:7px 10px 7px 26px;position:relative;height:36px;flex:none}
.dot{position:absolute;left:9px;top:19px;width:10px;height:10px;border-radius:50%}
.nm{font-size:17px}.dt{font-size:13px;color:#87867F;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;width:250px}
.bd{position:absolute;right:10px;top:8px;font-size:13px;color:#87867F}
#st{bottom:46px;font-size:14px;color:#87867F}
#dec{position:absolute;inset:0;border-radius:50%;background:#141413;border:4px solid #B1B9F9;box-sizing:border-box;display:none}
#dt{top:58px;font-size:13px;color:#B1B9F9;letter-spacing:1px}#ds{top:84px;font-size:18px}#dtool{top:110px;font-size:28px;font-weight:600}
#dbox{position:absolute;top:161px;left:66px;width:316px;height:110px;background:#1F1E1D;border-radius:12px;padding:12px;font-size:14px;overflow:auto;white-space:pre-wrap;word-break:break-all}
button{position:absolute;top:305px;width:150px;height:72px;border:0;border-radius:36px;font-size:20px;color:#FAF9F5;cursor:pointer}
#deny{left:83px;background:#3D3D3A}#allow{left:243px;background:#D77757}
#dh{bottom:40px;font-size:13px;color:#87867F}
#pair{position:fixed;top:24px;left:50%;transform:translateX(-50%);background:#FAF9F5;color:#141413;border-radius:16px;padding:18px 22px;display:none;text-align:center;box-shadow:0 8px 30px #0008;z-index:9}
#pair b{display:block;font-size:44px;letter-spacing:8px;margin:6px 0 10px}
#pair button{position:static;width:auto;height:auto;padding:10px 20px;font-size:16px;border-radius:10px;margin:0 4px}
#pair .ok{background:#D77757}#pair .no{background:#87867F}
</style></head><body><div id="pair"></div><div id="d">
<svg viewBox="0 0 480 480"><path id="a1b" fill="none" stroke="#30302E" stroke-width="22" stroke-linecap="round"/><path id="a1" fill="none" stroke="#D77757" stroke-width="22" stroke-linecap="round"/>
<path id="a2b" fill="none" stroke="#30302E" stroke-width="14" stroke-linecap="round"/><path id="a2" fill="none" stroke="#D4A27F" stroke-width="14" stroke-linecap="round"/></svg>
<div class="c" id="pct">--</div><div class="c" id="sub"></div><div class="c" id="wk"></div>
<div id="list"></div><div class="c" id="st"></div>
<div id="dec"><div class="c" id="dt"></div><div class="c" id="ds"></div><div class="c" id="dtool"></div><div id="dbox"></div>
<button id="deny">Deny</button><button id="allow">Allow</button><div class="c" id="dh"></div></div></div>
<script>
const $=id=>document.getElementById(id);
function arc(r,pct){const a0=150,sweep=240*Math.max(0,Math.min(100,pct))/100;if(sweep<=0)return"";const p=a=>[240+r*Math.cos(a*Math.PI/180),240+r*Math.sin(a*Math.PI/180)];
const [x0,y0]=p(a0),[x1,y1]=p(a0+sweep);return`M${x0} ${y0} A${r} ${r} 0 ${sweep>180?1:0} 1 ${x1} ${y1}`}
$('a1b').setAttribute('d',arc(227,100));$('a2b').setAttribute('d',arc(203,100));
const lvl=(p,base)=>p>=90?'#FF6B80':p>=75?'#FFC107':base;
function dur(s){if(s==null)return"?";const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?`${d}d ${h}h`:h?`${h}h ${String(m).padStart(2,'0')}m`:`${m}m`}
const col={working:'#4EBA65',decision:'#B1B9F9',waiting:'#FFC107',idle:'#87867F'};
const word={decision:'needs you',waiting:'waiting',idle:'idle'};
let cur=null,sent={};
async function tick(){let s;try{s=await (await fetch('/state')).json()}catch(e){$('st').textContent='bridge offline';return}
const u=s.usage,f=u.five_hour,w=u.seven_day;
if(u.ok&&f){$('pct').textContent=Math.round(f.pct)+'%';$('sub').textContent='5-hour limit · resets '+dur(f.resets_in);
$('a1').setAttribute('d',arc(227,f.pct));$('a1').setAttribute('stroke',lvl(f.pct,'#D77757'));$('sub').style.color=lvl(f.pct,'#D77757')}else{$('pct').textContent='--';$('sub').textContent=u.error||'usage unavailable';$('sub').style.color='#87867F'}
if(u.ok&&w){$('wk').textContent='Week '+Math.round(w.pct)+'% · resets '+dur(w.resets_in);$('a2').setAttribute('d',arc(203,w.pct));$('a2').setAttribute('stroke',lvl(w.pct,'#D4A27F'));$('wk').style.color=lvl(w.pct,'#D4A27F')}
$('list').innerHTML=s.sessions.length?s.sessions.map(x=>`<div class="row"><div class="dot" style="background:${col[x.state]||'#87867F'}"></div><div class="nm">${esc(x.name)}</div><div class="dt">${esc(x.tool?x.tool+': '+x.detail:x.detail)}</div><div class="bd">${x.subagents?'+'+x.subagents+(x.subagents>1?' agents':' agent'):(word[x.state]||'')}</div></div>`).join(''):'<div style="text-align:center;color:#87867F;margin-top:80px">No active Claude sessions</div>';
const working=s.sessions.filter(x=>x.state=='working').length;
$('st').textContent=new Date().toTimeString().slice(0,5)+' · '+working+' working';
const p=s.pending.find(p=>!sent[p.id]);
if(p){$('dec').style.display='block';if(cur!==p.id){cur=p.id;$('dt').textContent='PERMISSION NEEDED'+(s.pending.length>1?' · 1/'+s.pending.length:'');
$('ds').textContent=p.session;$('dtool').textContent=p.tool;curQ=p.questions||null;
if(curQ){$('dbox').innerHTML=curQ.map((q,i)=>`<div style="margin-bottom:8px"><b>${esc(q.q)}</b>`+q.opts.map((o,j)=>`<label style="display:block"><input type="${q.multi?'checkbox':'radio'}" name="q${i}" value="${j}"> ${esc(o)}</label>`).join('')+'</div>').join('');$('allow').textContent='Answer';$('deny').textContent='Skip'}
else{$('dbox').textContent=p.detail;$('allow').textContent='Allow';$('deny').textContent='Deny'}}$('dh').textContent='terminal takes over in '+p.expires_in+'s'}else{$('dec').style.display='none';cur=null}}
function esc(t){return String(t).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}
let curQ=null;
async function decide(b){if(!cur)return;const body={id:cur,behavior:b};
if(curQ){if(b==='deny')body.behavior='skip';else{body.answers=curQ.map((q,i)=>[...document.querySelectorAll(`input[name=q${i}]:checked`)].map(x=>+x.value));if(body.answers.some(a=>!a.length))return}}
sent[cur]=1;await fetch('/decision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});tick()}
$('allow').onclick=()=>decide('allow');$('deny').onclick=()=>decide('deny');
async function pairs(){let l=[];try{l=await (await fetch('/pairings')).json()}catch(e){}
const el=$('pair');if(!l.length){el.style.display='none';return}const p=l[0];el.style.display='block';
if(el.dataset.req===p.req)return;el.dataset.req=p.req;
el.innerHTML=`Pair <i>${esc(p.device)}</i>? Check the code on the display:<b>${esc(p.code)}</b><button class="ok">Approve</button><button class="no">Reject</button>`;
el.querySelector('.ok').onclick=()=>pd(p.req,true);el.querySelector('.no').onclick=()=>pd(p.req,false)}
async function pd(req,ok){await fetch('/pair/approve',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({req,approve:ok})});$('pair').dataset.req='';pairs()}
tick();pairs();setInterval(tick,1000);setInterval(pairs,1500);
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", action="store_true", help="generate fake data instead of fetching usage")
    ap.add_argument("--port", type=int)
    args = ap.parse_args()
    conf = load_conf()
    if args.port:
        conf["port"] = args.port
    dash = Dash(conf)
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))  # so atexit stops the Bonjour advert
    threading.Thread(target=demo_loop if args.demo else usage_loop, args=(dash,), daemon=True).start()
    try:
        srv = ThreadingHTTPServer(("0.0.0.0", conf["port"]), make_handler(dash))
    except OSError:
        print(f"Port {conf['port']} busy - bridge already running?", flush=True)
        return
    srv.daemon_threads = True

    def quiet_errors(request, client_address, _orig=srv.handle_error):
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError, TimeoutError)):
            return   # a display rebooted or dropped Wi-Fi mid-request
        _orig(request, client_address)
    srv.handle_error = quiet_errors
    advertise(conf)
    threading.Thread(target=udp_discovery_loop, args=(conf,), daemon=True).start()
    if not args.demo:
        threading.Thread(target=firmware_loop, args=(dash,), daemon=True).start()
    if conf.get("usb_setup") and sys.platform == "darwin":
        threading.Thread(target=usb_loop, args=(dash,), daemon=True).start()
    print(f"Claude Dash bridge on http://{lan_ip()}:{conf['port']} (Bonjour: _claude-dash._tcp)", flush=True)
    print(f"Open http://127.0.0.1:{conf['port']}/ for the preview and to approve pairing.", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
