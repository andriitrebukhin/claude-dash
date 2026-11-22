// Networking on core 0: Wi-Fi setup portal, Bonjour discovery of the Mac
// bridge, pairing, and state polling. LVGL is never touched here; the UI reads
// a copy of the state through net_snapshot().
//
//   no Wi-Fi saved ──▶ PORTAL (phone hotspot + captive page) ──▶ restart
//   WIFI ──▶ DISCOVER (_claude-dash._tcp) ──▶ PAIR (code on screen) ──▶ RUN
//   RUN: 401 ──▶ PAIR,  bridge unreachable ──▶ DISCOVER
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <ArduinoJson.h>   // v7
#include <esp_mac.h>
#include "config.h"
#include "dash_state.h"
#include "board.h"

struct Decision { char id[24]; char behavior[8]; char answers[96]; };

static DashState g_state;
static DashState g_tmp;               // parse buffer, only used by the net task
static SemaphoreHandle_t g_mtx;
static QueueHandle_t g_decQ;

static Preferences prefs;
static String g_ssid, g_pass, g_token, g_bridgeId;
static String g_base;                 // "http://ip:port" of the bridge
static String g_deviceName;
static String g_visibleSsids = "[]";   // JSON array from the last Wi-Fi scan (for USB setup)
static volatile bool g_usbSaved = false;
static volatile bool g_updateRequested = false;
static uint8_t g_mode = MODE_BOOT;

static void cp(char *dst, size_t n, const char *src) { strlcpy(dst, src ? src : "", n); }

// ---------------------------------------------------------------- shared state
static void setMode(uint8_t mode, const char *title, const char *big, const char *sub, const char *qr = "") {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_state.mode = mode;
  g_mode = mode;
  cp(g_state.setup.title, sizeof(g_state.setup.title), title);
  cp(g_state.setup.big, sizeof(g_state.setup.big), big);
  cp(g_state.setup.sub, sizeof(g_state.setup.sub), sub);
  cp(g_state.setup.qr, sizeof(g_state.setup.qr), qr);
  g_state.wifiOk = WiFi.status() == WL_CONNECTED;
  if (mode != MODE_RUN) g_state.bridgeOk = false;
  g_state.seq++;
  xSemaphoreGive(g_mtx);
}

static void saveStr(const char *key, const String &v) {
  prefs.begin("dash", false);
  prefs.putString(key, v);
  prefs.end();
}

static void loadPrefs() {
  prefs.begin("dash", true);
  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
  g_token = prefs.getString("token", "");
  g_bridgeId = prefs.getString("bid", "");
  prefs.end();
}

// Hold BOOT for 5 s at any time to forget Wi-Fi and pairing.
static void checkResetButton() {
  static uint32_t downSince = 0;
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!downSince) downSince = millis();
    if (millis() - downSince > 5000) {
      setMode(MODE_BOOT, "RESETTING", "", "Forgetting Wi-Fi and pairing...");
      prefs.begin("dash", false);
      prefs.clear();
      prefs.end();
      delay(800);
      ESP.restart();
    }
  } else {
    downSince = 0;
  }
}

static bool discoverUdp();

// ---------------------------------------------------------------- USB setup
// Line protocol over the USB cable, used by the Mac bridge:
//   Mac:     DASH?
//   display: DASH HELLO {"device":..,"mode":..,"paired":..,"bid":..,"ssids":[..]}
//   Mac:     DASH SETUP {"ssid":..,"pass":..,"token":..,"bid":..}   (all optional)
//   display: DASH OK   (restarts if Wi-Fi changed)
static void handleSerialLine(const String &line) {
  if (line == "DASH?") {
    JsonDocument d;
    d["device"] = g_deviceName;
    d["fw"] = DASH_VERSION;
    d["mode"] = g_mode;
    d["paired"] = g_token.length() > 0;
    d["bid"] = g_bridgeId;
    d["bridge"] = g_base;
    d["ip"] = WiFi.localIP().toString();
    d["ssids"] = serialized(g_visibleSsids);
    String out;
    serializeJson(d, out);
    Serial.println("DASH HELLO " + out);
    return;
  }
  if (line == "DASH DIAG") {
    Serial.printf("DASH DIAG ip=%s wifi=%d base=%s\n", WiFi.localIP().toString().c_str(), WiFi.status(), g_base.c_str());
    int n = MDNS.queryService("claude-dash", "tcp");
    Serial.printf("DASH DIAG mdns n=%d\n", n);
    for (int i = 0; i < n; i++)
      Serial.printf("DASH DIAG mdns %d host=%s ip=%s port=%u id=%s\n", i, MDNS.hostname(i).c_str(),
                    MDNS.address(i).toString().c_str(), MDNS.port(i), MDNS.txt(i, "id").c_str());
    Serial.printf("DASH DIAG udp=%d base=%s\n", discoverUdp(), g_base.c_str());
    return;
  }
  if (line.startsWith("DASH SETUP ")) {
    JsonDocument d;
    if (deserializeJson(d, line.substring(11))) { Serial.println("DASH ERR bad json"); return; }
    String ssid = d["ssid"] | "", pass = d["pass"] | "", token = d["token"] | "", bid = d["bid"] | "";
    prefs.begin("dash", false);
    if (ssid.length()) { prefs.putString("ssid", ssid); prefs.putString("pass", pass); }
    if (token.length()) { prefs.putString("token", token); prefs.putString("bid", bid); }
    prefs.end();
    if (token.length()) { g_token = token; g_bridgeId = bid; }
    Serial.println("DASH OK");
    Serial.flush();
    if (ssid.length()) {
      g_usbSaved = true;
      setMode(MODE_BOOT, "WI-FI RECEIVED", "", "Got your Wi-Fi from the Mac. Restarting...");
    }
    return;
  }
}

static void checkSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) handleSerialLine(line);
      line = "";
    } else if (line.length() < 600) {
      line += c;
    }
  }
  if (g_usbSaved) { delay(1200); ESP.restart(); }
}

static void waitMs(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { checkResetButton(); checkSerial(); vTaskDelay(pdMS_TO_TICKS(50)); }
}

// ---------------------------------------------------------------- Wi-Fi setup portal
static String htmlEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
  }
  return o;
}

static void runPortal(const char *why) {
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  String opts;
  int shown = 0;
  JsonDocument seen;
  JsonArray seenArr = seen.to<JsonArray>();
  for (int i = 0; i < n && shown < 15; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || opts.indexOf(">" + htmlEscape(s) + "<") >= 0) continue;
    opts += "<button type=button onclick=\"f.s.value=this.textContent;f.p.focus()\">" + htmlEscape(s) + "</button>";
    seenArr.add(s);
    shown++;
  }
  g_visibleSsids = "";
  serializeJson(seen, g_visibleSsids);
  WiFi.scanDelete();
  WiFi.softAP(SETUP_AP_NAME);
  IPAddress apIp = WiFi.softAPIP();

  DNSServer dns;
  dns.start(53, "*", apIp);
  WebServer server(80);
  bool saved = false;

  const String page = String(
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Claude Dash setup</title>"
    "<style>body{font-family:-apple-system,sans-serif;background:#141413;color:#F0EEE6;max-width:420px;margin:auto;padding:20px}"
    "h1{font-size:22px}button,input{font-size:17px;border-radius:10px;border:0;padding:12px;width:100%;box-sizing:border-box;margin:4px 0}"
    "button{background:#2A2925;color:#F0EEE6;text-align:left}input{background:#F0EEE6}.go{background:#D97757;text-align:center;margin-top:14px}"
    "p{color:#8A877D}</style><h1>Connect Claude Dash to Wi-Fi</h1><p>Pick your network (2.4 GHz) and enter its password.</p>") +
    opts + "<form name=f method=post action=/save><input name=s placeholder='Network name' required>"
    "<input name=p type=password placeholder='Password'><button class=go>Connect</button></form>";

  server.on("/", HTTP_GET, [&]() { server.send(200, "text/html", page); });
  server.on("/save", HTTP_POST, [&]() {
    String s = server.arg("s"), p = server.arg("p");
    s.trim();
    if (!s.length()) { server.send(400, "text/plain", "Network name required"); return; }
    prefs.begin("dash", false);
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    prefs.end();
    server.send(200, "text/html",
      "<meta name=viewport content='width=device-width'><body style='font-family:sans-serif;background:#141413;color:#F0EEE6;padding:20px'>"
      "<h2>Saved</h2><p>Claude Dash is joining " + htmlEscape(s) + ". You can reconnect your phone to your normal Wi-Fi now.</p>");
    saved = true;
  });
  server.onNotFound([&]() {   // captive-portal detection on iOS/Android/macOS
    server.sendHeader("Location", String("http://") + apIp.toString() + "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  char sub[120];
  snprintf(sub, sizeof(sub), "%sJoin \"%s\" on your phone, or plug me into your Mac", why, SETUP_AP_NAME);
  char qr[96];
  snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", SETUP_AP_NAME);
  setMode(MODE_PORTAL, "SET UP WI-FI", "", sub, qr);

  uint32_t savedAt = 0, lastRetry = millis();
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    checkResetButton();
    checkSerial();
    if (saved && !savedAt) savedAt = millis();
    if (savedAt && millis() - savedAt > 1500) ESP.restart();
    // saved network came back (e.g. router rebooted)? go back to normal
    if (g_ssid.length() && !saved) {
      if (WiFi.status() == WL_CONNECTED) ESP.restart();
      if (millis() - lastRetry > 60000) { WiFi.begin(g_ssid.c_str(), g_pass.c_str()); lastRetry = millis(); }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void connectWifi() {
  if (!g_ssid.length()) runPortal("");   // never returns
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  char sub[120];
  snprintf(sub, sizeof(sub), "Joining %s", g_ssid.c_str());
  setMode(MODE_WIFI, "CONNECTING", "", sub);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 40000) {
      snprintf(sub, sizeof(sub), "Couldn't join %s. ", g_ssid.c_str());
      runPortal(sub);                      // never returns
    }
    waitMs(250);
  }
  Serial.printf("[net] Wi-Fi up, IP %s\n", WiFi.localIP().toString().c_str());
  MDNS.begin(g_deviceName.c_str());
}

// ---------------------------------------------------------------- discovery
#define DISCOVERY_UDP_PORT 8788

// Fallback for networks where Bonjour queries don't get answered:
// broadcast "CLAUDE-DASH?" and the bridge replies "CLAUDE-DASH <port> <bridge_id>".
static bool discoverUdp() {
  WiFiUDP udp;
  if (!udp.begin(DISCOVERY_UDP_PORT + 1)) return false;
  bool found = false;
  for (int attempt = 0; attempt < 2 && !found; attempt++) {
    udp.beginPacket(WiFi.broadcastIP(), DISCOVERY_UDP_PORT);
    udp.print("CLAUDE-DASH?");
    udp.endPacket();
    uint32_t t0 = millis();
    String best;
    while (millis() - t0 < 1200) {
      if (udp.parsePacket()) {
        char buf[96];
        int len = udp.read(buf, sizeof(buf) - 1);
        buf[len > 0 ? len : 0] = 0;
        int port; char bid[24] = "";
        if (sscanf(buf, "CLAUDE-DASH %d %23s", &port, bid) >= 1) {
          String base = "http://" + udp.remoteIP().toString() + ":" + String(port);
          if (!found || (g_bridgeId.length() && g_bridgeId == bid)) { g_base = base; found = true; }
          if (g_bridgeId == bid) break;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  udp.stop();
  if (found) Serial.printf("[net] found bridge %s (broadcast)\n", g_base.c_str());
  return found;
}

static bool discover() {
#ifdef BRIDGE_HOST_OVERRIDE
  g_base = String("http://") + BRIDGE_HOST_OVERRIDE;
  return true;
#endif
  int n = MDNS.queryService("claude-dash", "tcp");
  if (n <= 0) return discoverUdp();
  int pick = 0;
  for (int i = 0; i < n; i++)
    if (g_bridgeId.length() && MDNS.txt(i, "id") == g_bridgeId) { pick = i; break; }
  IPAddress ip = MDNS.address(pick);
  if (ip == IPAddress(0, 0, 0, 0)) {
    // Bonjour answered with the service but no A record: resolve the host name,
    // or fall back to the LAN broadcast.
    ip = MDNS.queryHost(MDNS.hostname(pick), 2000);
    if (ip == IPAddress(0, 0, 0, 0)) return discoverUdp();
  }
  g_base = "http://" + ip.toString() + ":" + String(MDNS.port(pick));
  Serial.printf("[net] found bridge %s (%d on LAN)\n", g_base.c_str(), n);
  return true;
}

static void discoverLoop() {
  setMode(MODE_DISCOVER, "LOOKING FOR YOUR MAC", "",
          "Start Claude Code with the desk-display plugin on a Mac on this Wi-Fi");
  while (!discover()) {
    if (WiFi.status() != WL_CONNECTED) { connectWifi(); continue; }
    waitMs(3000);
  }
}

// ---------------------------------------------------------------- HTTP helpers
static int httpGet(const String &path, String &body, bool auth) {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(g_base + path);
  if (auth) http.addHeader("X-Token", g_token);
  http.addHeader("X-Device", g_deviceName);
  http.addHeader("X-Fw", DASH_VERSION);
  int code = http.GET();
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

static int httpPost(const String &path, const String &json, String &body, bool auth) {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(g_base + path);
  http.addHeader("Content-Type", "application/json");
  if (auth) http.addHeader("X-Token", g_token);
  int code = http.POST(json);
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

// ---------------------------------------------------------------- pairing
// Returns true once paired; false if the bridge went away (caller re-discovers).
static bool pairLoop() {
  int failures = 0;
  for (;;) {
    char code[8];
    snprintf(code, sizeof(code), "%04u", (unsigned)(esp_random() % 10000));
    String body;
    int rc = httpPost("/pair/request", String("{\"device\":\"") + g_deviceName + "\",\"code\":\"" + code + "\"}", body, false);
    if (rc != 200) { if (++failures > 4) return false; waitMs(2000); continue; }
    JsonDocument doc;
    if (deserializeJson(doc, body)) { waitMs(2000); continue; }
    String req = doc["req"] | "";
    setMode(MODE_PAIR, "PAIR WITH YOUR MAC", code, "Approve this code in the page that opened on your Mac");

    uint32_t t0 = millis();
    while (millis() - t0 < 9 * 60 * 1000UL) {
      waitMs(1500);
      if (g_token.length()) return true;   // paired over USB meanwhile
      int sc = httpGet("/pair/status?req=" + req, body, false);
      if (sc != 200) { if (++failures > 6) return false; continue; }
      failures = 0;
      JsonDocument st;
      if (deserializeJson(st, body)) continue;
      String status = st["status"] | "";
      if (status == "approved") {
        g_token = st["token"] | "";
        g_bridgeId = st["bridge_id"] | "";
        saveStr("token", g_token);
        saveStr("bid", g_bridgeId);
        Serial.println("[net] paired");
        return true;
      }
      if (status == "denied") {
        setMode(MODE_PAIR, "PAIRING REJECTED", "", "Getting a new code...");
        waitMs(4000);
        break;
      }
      if (status == "unknown") break;   // bridge restarted: ask again
    }
  }
}

// ---------------------------------------------------------------- dashboard
static void parseWindow(JsonVariantConst w, UsageWindow &o) {
  o.valid = !w.isNull() && !w["pct"].isNull();
  o.pct = w["pct"] | 0.0f;
  o.resetsIn = w["resets_in"] | -1L;
}

static bool parseState(const String &body, DashState &st) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  memset(&st, 0, sizeof(st));
  cp(st.clock, sizeof(st.clock), doc["clock"] | "");
  cp(st.fwLatest, sizeof(st.fwLatest), doc["firmware"]["latest"] | "");
  st.fwSize = doc["firmware"]["size"] | 0;
  cp(st.fwMd5, sizeof(st.fwMd5), doc["firmware"]["md5"] | "");

  JsonVariantConst u = doc["usage"];
  st.usageOk = u["ok"] | false;
  cp(st.usageErr, sizeof(st.usageErr), u["error"] | "");
  parseWindow(u["five_hour"], st.fiveHour);
  parseWindow(u["seven_day"], st.week);

  for (JsonVariantConst s : doc["sessions"].as<JsonArrayConst>()) {
    if (st.nSessions >= MAX_SESSIONS) break;
    SessionInfo &d = st.sessions[st.nSessions++];
    cp(d.name, sizeof(d.name), s["name"] | "");
    cp(d.state, sizeof(d.state), s["state"] | "idle");
    cp(d.tool, sizeof(d.tool), s["tool"] | "");
    cp(d.detail, sizeof(d.detail), s["detail"] | "");
    d.subagents = s["subagents"] | 0;
  }
  for (JsonVariantConst p : doc["pending"].as<JsonArrayConst>()) {
    if (st.nPending >= MAX_PENDING) break;
    PendingInfo &d = st.pending[st.nPending++];
    cp(d.id, sizeof(d.id), p["id"] | "");
    cp(d.session, sizeof(d.session), p["session"] | "");
    cp(d.tool, sizeof(d.tool), p["tool"] | "");
    cp(d.detail, sizeof(d.detail), p["detail"] | "");
    d.expiresIn = p["expires_in"] | 0;
    for (JsonVariantConst q : p["questions"].as<JsonArrayConst>()) {
      if (d.nQuestions >= MAX_QUESTIONS) break;
      QuestionInfo &qi = d.questions[d.nQuestions++];
      cp(qi.header, sizeof(qi.header), q["h"] | "");
      cp(qi.text, sizeof(qi.text), q["q"] | "");
      qi.multi = q["multi"] | false;
      for (JsonVariantConst o : q["opts"].as<JsonArrayConst>()) {
        if (qi.nOpts >= MAX_OPTIONS) break;
        cp(qi.opts[qi.nOpts++], sizeof(qi.opts[0]), o | "");
      }
    }
  }
  return true;
}

static void postDecision(const Decision &d) {
  if (!strcmp(d.behavior, "noop")) return;
  String body;
  String json = String("{\"id\":\"") + d.id + "\",\"behavior\":\"" + d.behavior + "\"";
  if (d.answers[0]) json += String(",\"answers\":") + d.answers;
  json += "}";
  int code = httpPost("/decision", json, body, true);
  Serial.printf("[net] decision %s %s %s -> %d\n", d.id, d.behavior, d.answers, code);
}

// ---------------------------------------------------------------- wireless update
// Writing flash briefly blocks PSRAM, where the RGB panel's frame buffer lives, so
// the screen glitches during the install. Dim it so that's barely visible.
#define UPDATE_BACKLIGHT 10

static void updateFailed(const char *why) {
  Serial.printf("[ota] failed: %s\n", why);
  board_set_backlight(BACKLIGHT_LEVEL);
  setMode(MODE_UPDATE, "UPDATE FAILED", "", why);
  waitMs(5000);
}

static void doUpdate() {
  char ver[24], md5[36];
  uint32_t size;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  cp(ver, sizeof(ver), g_state.fwLatest);
  cp(md5, sizeof(md5), g_state.fwMd5);
  size = g_state.fwSize;
  xSemaphoreGive(g_mtx);
  if (!ver[0] || !size) return;

  char sub[96];
  snprintf(sub, sizeof(sub), "Installing %s. Keep it powered; the screen dims and may flicker.", ver);
  setMode(MODE_UPDATE, "UPDATING", "0%", sub);
  vTaskDelay(pdMS_TO_TICKS(600));            // let the message show at full brightness first
  board_set_backlight(UPDATE_BACKLIGHT);
  Serial.printf("[ota] %s -> %s (%u bytes)\n", DASH_VERSION, ver, (unsigned)size);

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(g_base + "/firmware/app.bin");
  http.addHeader("X-Token", g_token);
  int code = http.GET();
  if (code != 200) { http.end(); updateFailed("Couldn't download the firmware from the Mac."); return; }
  int len = http.getSize();
  if (len != (int)size) { http.end(); updateFailed("Firmware size mismatch."); return; }
  if (!Update.begin(len)) { http.end(); updateFailed("Not enough space for the update."); return; }
  if (md5[0]) Update.setMD5(md5);

  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[4096];
  int written = 0, lastPct = -1;
  uint32_t lastData = millis();
  while (written < len) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastData > 15000) break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    int r = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
    if (r <= 0) continue;
    if (Update.write(buf, r) != (size_t)r) break;
    written += r;
    lastData = millis();
    int pct = (int)((int64_t)written * 100 / len);
    if (pct != lastPct && pct % 2 == 0) {
      char big[8];
      snprintf(big, sizeof(big), "%d%%", pct);
      setMode(MODE_UPDATE, "UPDATING", big, sub);
      lastPct = pct;
    }
  }
  http.end();
  if (written != len) { Update.abort(); updateFailed("Download interrupted."); return; }
  if (!Update.end(true)) { updateFailed(Update.getError() == UPDATE_ERROR_MD5 ? "Checksum mismatch." : Update.errorString()); return; }
  board_set_backlight(BACKLIGHT_LEVEL);
  setMode(MODE_UPDATE, "UPDATED", ver, "Restarting...");
  Serial.printf("[ota] done, restarting into %s\n", ver);
  delay(1500);
  ESP.restart();
}

enum RunExit { EXIT_UNAUTHORIZED, EXIT_LOST };

static RunExit runLoop() {
  int failures = 0;
  for (;;) {
    checkResetButton();
    checkSerial();
    if (g_updateRequested) { g_updateRequested = false; doUpdate(); }
    Decision d;
    while (xQueueReceive(g_decQ, &d, 0) == pdTRUE) postDecision(d);

    String body;
    int code = httpGet("/state?ascii=1", body, true);
    if (code == 401) return EXIT_UNAUTHORIZED;
    bool ok = code == 200 && parseState(body, g_tmp);

    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_mode = MODE_RUN;
    if (ok) {
      g_tmp.mode = MODE_RUN;
      g_tmp.setup = g_state.setup;
      g_tmp.wifiOk = true;
      g_tmp.bridgeOk = true;
      g_tmp.seq = g_state.seq + 1;
      memcpy(&g_state, &g_tmp, sizeof(g_state));
    } else {
      g_state.mode = MODE_RUN;
      g_state.wifiOk = WiFi.status() == WL_CONNECTED;
      g_state.bridgeOk = false;
      g_state.seq++;
    }
    xSemaphoreGive(g_mtx);

    failures = ok ? 0 : failures + 1;
    if (failures >= 5) return EXIT_LOST;   // Mac asleep / IP changed: look again

    // sleep until next poll, but wake immediately if a decision was tapped
    xQueuePeek(g_decQ, &d, pdMS_TO_TICKS(ok ? POLL_MS : 2000));
  }
}

static void netTask(void *) {
  Serial.printf("[net] Claude Dash firmware %s\n", DASH_VERSION);
  setMode(MODE_BOOT, "CLAUDE DASH", "", "Starting " DASH_VERSION "...");
  connectWifi();
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) connectWifi();
    discoverLoop();
    if (!g_token.length() && !pairLoop()) continue;
    if (runLoop() == EXIT_UNAUTHORIZED) {
      Serial.println("[net] token rejected - pairing again");
      g_token = "";
      saveStr("token", "");
    }
  }
}

void net_begin() {
  g_mtx = xSemaphoreCreateMutex();
  g_decQ = xQueueCreate(4, sizeof(Decision));
  memset(&g_state, 0, sizeof(g_state));
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char name[24];
  snprintf(name, sizeof(name), "claude-dash-%02x%02x", mac[4], mac[5]);
  g_deviceName = name;
  loadPrefs();
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

bool net_snapshot(DashState &out, uint32_t &lastSeq) {
  bool changed = false;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_state.seq != lastSeq) {
    memcpy(&out, &g_state, sizeof(out));
    lastSeq = g_state.seq;
    changed = true;
  }
  xSemaphoreGive(g_mtx);
  return changed;
}

void net_start_update() {
  g_updateRequested = true;
  Decision wake = {};   // wake the poll loop right away
  cp(wake.behavior, sizeof(wake.behavior), "noop");
  xQueueSend(g_decQ, &wake, 0);
}

void net_send_decision(const char *id, const char *behavior, const char *answersJson) {
  Decision d;
  cp(d.id, sizeof(d.id), id);
  cp(d.behavior, sizeof(d.behavior), behavior);
  cp(d.answers, sizeof(d.answers), answersJson);
  xQueueSend(g_decQ, &d, 0);
}
