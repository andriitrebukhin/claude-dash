// Networking on core 0: Wi-Fi setup, discovery of Mac bridges, pairing, and
// state polling. LVGL is never touched here; the UI reads a copy of the state
// through net_snapshot().
//
//   no Wi-Fi saved ──▶ PORTAL (phone hotspot + captive page, or Improv / Mac over USB)
//   WIFI ──▶ no Mac paired yet: DISCOVER ──▶ PAIR (code on screen) ──▶ RUN
//   RUN: polls every paired Mac and merges their sessions. Unpaired Macs seen on
//        the network are offered as "Pair <Mac>"; a Mac that rejects our token is
//        dropped (and offered again).
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

// A paired Mac. `base` is where it was last found ("" = not found yet).
struct Bridge {
  char id[16];
  char token[40];
  char host[28];
  char base[48];
  uint8_t fails;
};

// An unpaired Mac seen on the network.
struct Candidate {
  char id[16];
  char host[28];
  char base[48];
};

#define MAX_CANDIDATES 4

static DashState g_state;
static DashState g_tmp;               // merge buffer, only used by the net task
static SemaphoreHandle_t g_mtx;
static QueueHandle_t g_decQ;

static Preferences prefs;
static String g_ssid, g_pass;
static Bridge g_br[MAX_BRIDGES];
static uint8_t g_nbr = 0;
static Candidate g_cand[MAX_CANDIDATES];
static uint8_t g_ncand = 0;
static String g_deviceName;
static String g_visibleSsids = "[]";  // JSON array from the last Wi-Fi scan (for USB setup)
static volatile bool g_restartSoon = false;
static volatile bool g_updateRequested = false;
static volatile bool g_pairRequested = false;
static volatile bool g_pairCancel = false;
static uint8_t g_mode = MODE_BOOT;
static Stream *g_io = &Serial;        // the port the current Improv/DASH message came in on

static void cp(char *dst, size_t n, const char *src) { strlcpy(dst, src ? src : "", n); }

// ---------------------------------------------------------------- shared state
static void setMode(uint8_t mode, const char *title, const char *big, const char *sub, const char *qr = "",
                    bool cancelable = false) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_state.mode = mode;
  g_mode = mode;
  cp(g_state.setup.title, sizeof(g_state.setup.title), title);
  cp(g_state.setup.big, sizeof(g_state.setup.big), big);
  cp(g_state.setup.sub, sizeof(g_state.setup.sub), sub);
  cp(g_state.setup.qr, sizeof(g_state.setup.qr), qr);
  g_state.pairCancelable = cancelable;
  g_state.wifiOk = WiFi.status() == WL_CONNECTED;
  if (mode != MODE_RUN) g_state.bridgeOk = false;
  g_state.seq++;
  xSemaphoreGive(g_mtx);
}

// ---------------------------------------------------------------- saved settings
static void saveBridges() {
  prefs.begin("dash", false);
  prefs.putUChar("nb", g_nbr);
  for (int i = 0; i < MAX_BRIDGES; i++) {
    char k[8];
    snprintf(k, sizeof(k), "b%di", i);
    if (i < g_nbr) {
      prefs.putString(k, g_br[i].id);
      k[2] = 't'; prefs.putString(k, g_br[i].token);
      k[2] = 'h'; prefs.putString(k, g_br[i].host);
    } else {
      prefs.remove(k); k[2] = 't'; prefs.remove(k); k[2] = 'h'; prefs.remove(k);
    }
  }
  prefs.end();
}

static void loadPrefs() {
  prefs.begin("dash", true);
  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
  g_nbr = min<uint8_t>(prefs.getUChar("nb", 0), MAX_BRIDGES);
  for (int i = 0; i < g_nbr; i++) {
    char k[8];
    snprintf(k, sizeof(k), "b%di", i);
    memset(&g_br[i], 0, sizeof(Bridge));
    cp(g_br[i].id, sizeof(g_br[i].id), prefs.getString(k, "").c_str());
    k[2] = 't'; cp(g_br[i].token, sizeof(g_br[i].token), prefs.getString(k, "").c_str());
    k[2] = 'h'; cp(g_br[i].host, sizeof(g_br[i].host), prefs.getString(k, "Mac").c_str());
  }
  // settings from v0.4 and earlier: one Mac under "token"/"bid"
  String oldTok = prefs.getString("token", "");
  String oldBid = prefs.getString("bid", "");
  prefs.end();
  if (g_nbr == 0 && oldTok.length()) {
    memset(&g_br[0], 0, sizeof(Bridge));
    cp(g_br[0].id, sizeof(g_br[0].id), oldBid.c_str());
    cp(g_br[0].token, sizeof(g_br[0].token), oldTok.c_str());
    cp(g_br[0].host, sizeof(g_br[0].host), "Mac");
    g_nbr = 1;
    saveBridges();
    prefs.begin("dash", false);
    prefs.remove("token");
    prefs.remove("bid");
    prefs.end();
  }
}

static int findBridge(const char *id) {
  for (int i = 0; i < g_nbr; i++)
    if (id[0] && !strcmp(g_br[i].id, id)) return i;
  return -1;
}

// Add or update a paired Mac. Returns its slot (-1 if the list is full).
static int addBridge(const char *id, const char *token, const char *host, const char *base) {
  int i = findBridge(id);
  if (i < 0) {
    if (g_nbr >= MAX_BRIDGES) return -1;
    i = g_nbr++;
    memset(&g_br[i], 0, sizeof(Bridge));
    cp(g_br[i].id, sizeof(g_br[i].id), id);
  }
  cp(g_br[i].token, sizeof(g_br[i].token), token);
  if (host && host[0]) cp(g_br[i].host, sizeof(g_br[i].host), host);
  if (base && base[0]) cp(g_br[i].base, sizeof(g_br[i].base), base);
  g_br[i].fails = 0;
  saveBridges();
  for (int c = 0; c < g_ncand; c++)            // no longer a candidate
    if (!strcmp(g_cand[c].id, id)) { g_cand[c] = g_cand[--g_ncand]; break; }
  return i;
}

static void removeBridge(int i) {
  Serial.printf("[net] forgetting Mac %s (token rejected)\n", g_br[i].host);
  for (int j = i; j + 1 < g_nbr; j++) g_br[j] = g_br[j + 1];
  g_nbr--;
  saveBridges();
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

// ---------------------------------------------------------------- Improv (Wi-Fi setup from the browser installer)
// https://www.improv-wifi.com/serial/  Packets: "IMPROV" ver type len data checksum
#define IMPROV_STATE_READY        0x02
#define IMPROV_STATE_PROVISIONING 0x03
#define IMPROV_STATE_PROVISIONED  0x04
#define IMPROV_ERR_NONE           0x00
#define IMPROV_ERR_INVALID        0x01
#define IMPROV_ERR_UNKNOWN_CMD    0x02
#define IMPROV_ERR_CONNECT        0x03

static void improvSend(uint8_t type, const uint8_t *data, uint8_t len) {
  uint8_t hdr[9] = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, len};
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  for (int i = 0; i < len; i++) sum += data[i];
  g_io->write(hdr, sizeof(hdr));
  if (len) g_io->write(data, len);
  g_io->write(sum);
  g_io->flush();
}

static void improvState(uint8_t st) { improvSend(0x01, &st, 1); }
static void improvError(uint8_t err) { improvSend(0x02, &err, 1); }

static void improvResult(uint8_t cmd, const char *const *strs, int n) {
  uint8_t buf[200];
  int len = 2;
  for (int i = 0; i < n; i++) {
    int l = strlen(strs[i]);
    if (len + 1 + l > (int)sizeof(buf)) break;
    buf[len++] = l;
    memcpy(buf + len, strs[i], l);
    len += l;
  }
  buf[0] = cmd;
  buf[1] = len - 2;
  improvSend(0x04, buf, len);
}

static void handleImprov(const uint8_t *pkt, int total) {
  uint8_t sum = 0;
  for (int i = 0; i < total - 1; i++) sum += pkt[i];
  if (sum != pkt[total - 1] || pkt[7] != 0x03) { improvError(IMPROV_ERR_INVALID); return; }
  const uint8_t *d = pkt + 9;
  uint8_t cmd = d[0];
  improvError(IMPROV_ERR_NONE);
  if (cmd == 0x02) {                                    // current state
    improvState(WiFi.status() == WL_CONNECTED ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY);
  } else if (cmd == 0x03) {                             // device info
    const char *info[4] = {"Claude Dash", DASH_VERSION, "ESP32-S3", g_deviceName.c_str()};
    improvResult(0x03, info, 4);
  } else if (cmd == 0x04) {                             // scan networks
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i), rssi = String(WiFi.RSSI(i));
      if (!ssid.length()) continue;
      const char *row[3] = {ssid.c_str(), rssi.c_str(), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"};
      improvResult(0x04, row, 3);
    }
    WiFi.scanDelete();
    improvResult(0x04, nullptr, 0);
  } else if (cmd == 0x01) {                             // Wi-Fi settings
    int sl = d[2];
    String ssid((const char *)d + 3, sl);
    int pl = d[3 + sl];
    String pass((const char *)d + 4 + sl, pl);
    improvState(IMPROV_STATE_PROVISIONING);
    setMode(MODE_WIFI, "CONNECTING", "", ("Joining " + ssid).c_str());
    if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(100));
    if (WiFi.status() == WL_CONNECTED) {
      prefs.begin("dash", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      improvState(IMPROV_STATE_PROVISIONED);
      improvResult(0x01, nullptr, 0);
      setMode(MODE_BOOT, "WI-FI CONNECTED", "", "Restarting...");
      g_restartSoon = true;
    } else {
      improvError(IMPROV_ERR_CONNECT);
      improvState(IMPROV_STATE_READY);
      if (g_ssid.length()) WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    }
  } else {
    improvError(IMPROV_ERR_UNKNOWN_CMD);
  }
}

// ---------------------------------------------------------------- USB setup from the Mac bridge
// Line protocol over the USB cable:
//   Mac:     DASH?
//   display: DASH HELLO {"device":..,"fw":..,"mode":..,"bids":[..],"ssids":[..]}
//   Mac:     DASH SETUP {"ssid":..,"pass":..,"token":..,"bid":..,"host":..}   (all optional)
//   display: DASH OK   (restarts if Wi-Fi changed)
static void handleSerialLine(const char *line) {
  if (!strcmp(line, "DASH?")) {
    JsonDocument d;
    d["device"] = g_deviceName;
    d["fw"] = DASH_VERSION;
    d["mode"] = g_mode;
    d["paired"] = g_nbr > 0;
    JsonArray bids = d["bids"].to<JsonArray>();
    for (int i = 0; i < g_nbr; i++) bids.add(g_br[i].id);
    d["bid"] = g_nbr ? g_br[0].id : "";   // pre-0.5 bridges look at this
    d["ip"] = WiFi.localIP().toString();
    d["ssids"] = serialized(g_visibleSsids);
    String out;
    serializeJson(d, out);
    g_io->println("DASH HELLO " + out);
    return;
  }
  if (!strcmp(line, "DASH DIAG")) {
    g_io->printf("DASH DIAG ip=%s wifi=%d macs=%d candidates=%d\n", WiFi.localIP().toString().c_str(),
                  WiFi.status(), g_nbr, g_ncand);
    for (int i = 0; i < g_nbr; i++)
      g_io->printf("DASH DIAG mac %d %s id=%s base=%s fails=%d\n", i, g_br[i].host, g_br[i].id, g_br[i].base, g_br[i].fails);
    return;
  }
  if (!strncmp(line, "DASH SETUP ", 11)) {
    JsonDocument d;
    if (deserializeJson(d, line + 11)) { g_io->println("DASH ERR bad json"); return; }
    String ssid = d["ssid"] | "", pass = d["pass"] | "", token = d["token"] | "", bid = d["bid"] | "";
    String host = d["host"] | "Mac";
    if (ssid.length()) {
      prefs.begin("dash", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
    }
    if (token.length() && bid.length()) addBridge(bid.c_str(), token.c_str(), host.c_str(), "");
    g_io->println("DASH OK");
    g_io->flush();
    if (ssid.length()) {
      g_restartSoon = true;
      setMode(MODE_BOOT, "WI-FI RECEIVED", "", "Got your Wi-Fi from the Mac. Restarting...");
    }
  }
}

// Bytes from a serial port: Improv packets (binary, start with "IMPROV") or text lines.
// Both the board's USB port (native USB) and its UART port (CH343) are served.
struct RxBuf { uint8_t b[640]; size_t n; };

static void pollPort(Stream &port, RxBuf &buf) {
  uint8_t *rx = buf.b;
  size_t &n = buf.n;
  g_io = &port;
  while (port.available()) {
    uint8_t c = port.read();
    if (n >= sizeof(buf.b)) n = 0;
    rx[n++] = c;
    if (n > 6 && !memcmp(rx + n - 6, "IMPROV", 6)) {    // Improv header after leftover bytes
      memcpy(rx, "IMPROV", 6);
      n = 6;
      continue;
    }
    size_t m = n < 6 ? n : 6;
    if (!memcmp(rx, "IMPROV", m)) {                     // (part of) an Improv header
      if (n >= 9 && n == (size_t)(9 + rx[8] + 1)) { handleImprov(rx, n); n = 0; }
      continue;
    }
    if (c == '\n' || c == '\r') {
      rx[n - 1] = 0;
      if (n > 1) handleSerialLine((const char *)rx);
      n = 0;
    }
  }
  g_io = &Serial;
}

static void checkSerial() {
  static RxBuf usb, uart;
  pollPort(Serial, usb);
  pollPort(Serial0, uart);
  if (g_restartSoon) { delay(1500); ESP.restart(); }
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
    "<style>body{font-family:-apple-system,sans-serif;background:#141413;color:#FAF9F5;max-width:420px;margin:auto;padding:20px}"
    "h1{font-size:22px}button,input{font-size:17px;border-radius:10px;border:0;padding:12px;width:100%;box-sizing:border-box;margin:4px 0}"
    "button{background:#30302E;color:#FAF9F5;text-align:left}input{background:#FAF9F5}.go{background:#D77757;text-align:center;margin-top:14px}"
    "p{color:#87867F}</style><h1>Connect Claude Dash to Wi-Fi</h1><p>Pick your network (2.4 GHz) and enter its password.</p>") +
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
      "<meta name=viewport content='width=device-width'><body style='font-family:sans-serif;background:#141413;color:#FAF9F5;padding:20px'>"
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

// ---------------------------------------------------------------- HTTP helpers
static int httpGet(const char *base, const String &path, String &body, const char *token, uint16_t timeout = 3000) {
  HTTPClient http;
  http.setTimeout(timeout);
  http.begin(String(base) + path);
  if (token && token[0]) http.addHeader("X-Token", token);
  http.addHeader("X-Device", g_deviceName);
  http.addHeader("X-Fw", DASH_VERSION);
  int code = http.GET();
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

static int httpPost(const char *base, const String &path, const String &json, String &body, const char *token) {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(String(base) + path);
  http.addHeader("Content-Type", "application/json");
  if (token && token[0]) http.addHeader("X-Token", token);
  int code = http.POST(json);
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

// ---------------------------------------------------------------- discovery
#define DISCOVERY_UDP_PORT 8788

// A Mac answered: remember where a paired one is, or offer an unpaired one.
static void noteFound(const char *id, const char *host, const char *base) {
  if (!id[0]) return;
  int i = findBridge(id);
  if (i >= 0) {
    if (strcmp(g_br[i].base, base)) Serial.printf("[net] %s is at %s\n", host, base);
    cp(g_br[i].base, sizeof(g_br[i].base), base);
    if (host[0]) cp(g_br[i].host, sizeof(g_br[i].host), host);
    return;
  }
  for (int c = 0; c < g_ncand; c++)
    if (!strcmp(g_cand[c].id, id)) { cp(g_cand[c].base, sizeof(g_cand[c].base), base); return; }
  if (g_ncand >= MAX_CANDIDATES) return;
  Candidate &c = g_cand[g_ncand++];
  cp(c.id, sizeof(c.id), id);
  cp(c.host, sizeof(c.host), host[0] ? host : "Mac");
  cp(c.base, sizeof(c.base), base);
  Serial.printf("[net] unpaired Mac %s at %s\n", c.host, c.base);
}

#ifdef BRIDGE_HOST_OVERRIDE
// Ask a bridge at a known address who it is.
static void identify(const char *base) {
  String body;
  if (httpGet(base, "/id", body, nullptr, 2000) != 200) return;
  JsonDocument d;
  if (deserializeJson(d, body)) return;
  noteFound(d["id"] | "", d["host"] | "", base);
}
#endif

// Broadcast "CLAUDE-DASH?"; each bridge replies "CLAUDE-DASH <port> <id> <host...>".
static void discoverUdp() {
  WiFiUDP udp;
  if (!udp.begin(DISCOVERY_UDP_PORT + 1)) return;
  for (int attempt = 0; attempt < 2; attempt++) {
    udp.beginPacket(WiFi.broadcastIP(), DISCOVERY_UDP_PORT);
    udp.print("CLAUDE-DASH?");
    udp.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 700) {
      if (udp.parsePacket()) {
        char buf[128];
        int len = udp.read(buf, sizeof(buf) - 1);
        buf[len > 0 ? len : 0] = 0;
        int port = 0;
        char id[16] = "", host[28] = "";
        if (sscanf(buf, "CLAUDE-DASH %d %15s %27[^\n]", &port, id, host) >= 2) {
          char base[48];
          snprintf(base, sizeof(base), "http://%s:%d", udp.remoteIP().toString().c_str(), port);
          noteFound(id, host, base);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  udp.stop();
}

static void discoverMdns() {
  int n = MDNS.queryService("claude-dash", "tcp");
  for (int i = 0; i < n; i++) {
    IPAddress ip = MDNS.address(i);
    if (ip == IPAddress(0, 0, 0, 0)) ip = MDNS.queryHost(MDNS.hostname(i), 1500);   // service without an A record
    if (ip == IPAddress(0, 0, 0, 0)) continue;
    char base[48];
    snprintf(base, sizeof(base), "http://%s:%u", ip.toString().c_str(), MDNS.port(i));
    String host = MDNS.txt(i, "host");
    noteFound(MDNS.txt(i, "id").c_str(), host.length() ? host.c_str() : MDNS.hostname(i).c_str(), base);
  }
}

static void discoverAll(bool useMdns) {
  int known = 0;
  for (int i = 0; i < g_nbr; i++) if (g_br[i].base[0]) known++;
  Serial.printf("[net] looking for Macs (%d/%d paired found, mdns=%d)\n", known, g_nbr, useMdns);
#ifdef BRIDGE_HOST_OVERRIDE
  identify("http://" BRIDGE_HOST_OVERRIDE);
#endif
  discoverUdp();
  if (useMdns) discoverMdns();
}

// ---------------------------------------------------------------- pairing
// Code flow with one Mac. firstMac: keep going until paired (nothing to fall back to).
// Otherwise give up after 3 minutes, on rejection, or when cancelled from the screen.
static bool pairWith(Candidate c, bool firstMac) {
  int failures = 0;
  uint32_t started = millis();
  g_pairCancel = false;
  for (;;) {
    char code[8];
    snprintf(code, sizeof(code), "%04u", (unsigned)(esp_random() % 10000));
    String body;
    int rc = httpPost(c.base, "/pair/request", String("{\"device\":\"") + g_deviceName + "\",\"code\":\"" + code + "\"}", body, nullptr);
    if (rc != 200) { if (++failures > 4) return false; waitMs(2000); continue; }
    JsonDocument doc;
    if (deserializeJson(doc, body)) { waitMs(2000); continue; }
    String req = doc["req"] | "";
    char sub[96];
    snprintf(sub, sizeof(sub), "Approve this code on %s", c.host);
    setMode(MODE_PAIR, firstMac ? "PAIR WITH YOUR MAC" : "PAIR ANOTHER MAC", code, sub, "", !firstMac);

    while (true) {
      waitMs(1500);
      if (g_pairCancel) return false;
      if (findBridge(c.id) >= 0) return true;                       // paired over USB meanwhile
      if (!firstMac && millis() - started > 180000) return false;
      int sc = httpGet(c.base, "/pair/status?req=" + req, body, nullptr);
      if (sc != 200) { if (++failures > 6) return false; continue; }
      failures = 0;
      JsonDocument st;
      if (deserializeJson(st, body)) continue;
      String status = st["status"] | "";
      if (status == "approved") {
        String bid = st["bridge_id"] | c.id;
        String host = st["host"] | c.host;
        addBridge(bid.c_str(), st["token"] | "", host.c_str(), c.base);
        Serial.printf("[net] paired with %s\n", host.c_str());
        return true;
      }
      if (status == "denied") {
        if (!firstMac) return false;
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

static long versionNum(const char *v) {
  if (*v == 'v' || *v == 'V') v++;
  int a = 0, b = 0, c = 0;
  if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return 0;
  return a * 1000000L + b * 1000L + c;
}

static uint8_t g_fwBridge = 0;   // which Mac serves the firmware we were offered

// Merge one Mac's /state into `st`: its usage goes to macs[slot] (its page),
// its sessions and permission requests are tagged with the slot.
static bool mergeState(const String &body, DashState &st, uint8_t slot) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (!st.clock[0]) cp(st.clock, sizeof(st.clock), doc["clock"] | "");
  const char *host = doc["host"] | "";
  if (host[0]) cp(g_br[slot].host, sizeof(g_br[slot].host), host);

  const char *fw = doc["firmware"]["latest"] | "";
  if (fw[0] && versionNum(fw) > versionNum(st.fwLatest)) {
    cp(st.fwLatest, sizeof(st.fwLatest), fw);
    st.fwSize = doc["firmware"]["size"] | 0;
    cp(st.fwMd5, sizeof(st.fwMd5), doc["firmware"]["md5"] | "");
    g_fwBridge = slot;
  }

  MacInfo &m = st.macs[slot];
  m.ok = true;
  JsonVariantConst u = doc["usage"];
  m.usageOk = u["ok"] | false;
  cp(m.usageErr, sizeof(m.usageErr), u["error"] | "");
  parseWindow(u["five_hour"], m.fiveHour);
  parseWindow(u["seven_day"], m.week);

  for (JsonVariantConst s : doc["sessions"].as<JsonArrayConst>()) {
    if (st.nSessions >= MAX_SESSIONS) break;
    SessionInfo &d = st.sessions[st.nSessions++];
    cp(d.name, sizeof(d.name), s["name"] | "");
    cp(d.state, sizeof(d.state), s["state"] | "idle");
    cp(d.tool, sizeof(d.tool), s["tool"] | "");
    cp(d.detail, sizeof(d.detail), s["detail"] | "");
    d.subagents = s["subagents"] | 0;
    d.bridge = slot;
  }
  bool label = g_nbr > 1;
  for (JsonVariantConst p : doc["pending"].as<JsonArrayConst>()) {
    if (st.nPending >= MAX_PENDING) break;
    PendingInfo &d = st.pending[st.nPending++];
    cp(d.id, sizeof(d.id), p["id"] | "");
    if (label) snprintf(d.session, sizeof(d.session), "%s on %s", (const char *)(p["session"] | ""), g_br[slot].host);
    else cp(d.session, sizeof(d.session), p["session"] | "");
    cp(d.tool, sizeof(d.tool), p["tool"] | "");
    cp(d.detail, sizeof(d.detail), p["detail"] | "");
    d.expiresIn = p["expires_in"] | 0;
    d.bridge = slot;
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
  // send it to the Mac that asked
  int slot = -1;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  for (int i = 0; i < g_state.nPending; i++)
    if (!strcmp(g_state.pending[i].id, d.id)) { slot = g_state.pending[i].bridge; break; }
  xSemaphoreGive(g_mtx);
  if (slot < 0 || slot >= g_nbr || !g_br[slot].base[0]) return;
  String body;
  String json = String("{\"id\":\"") + d.id + "\",\"behavior\":\"" + d.behavior + "\"";
  if (d.answers[0]) json += String(",\"answers\":") + d.answers;
  json += "}";
  int code = httpPost(g_br[slot].base, "/decision", json, body, g_br[slot].token);
  Serial.printf("[net] decision %s %s %s -> %s %d\n", d.id, d.behavior, d.answers, g_br[slot].host, code);
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
  if (!ver[0] || !size || g_fwBridge >= g_nbr) return;
  Bridge &b = g_br[g_fwBridge];

  char sub[96];
  snprintf(sub, sizeof(sub), "Installing %s. Keep it powered; the screen dims and may flicker.", ver);
  setMode(MODE_UPDATE, "UPDATING", "0%", sub);
  vTaskDelay(pdMS_TO_TICKS(600));            // let the message show at full brightness first
  board_set_backlight(UPDATE_BACKLIGHT);
  Serial.printf("[ota] %s -> %s (%u bytes) from %s\n", DASH_VERSION, ver, (unsigned)size, b.host);

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(String(b.base) + "/firmware/app.bin");
  http.addHeader("X-Token", b.token);
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

// ---------------------------------------------------------------- main loop
static void publish(bool anyOk, uint8_t nOk) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_mode = MODE_RUN;
  g_tmp.mode = MODE_RUN;
  g_tmp.setup = g_state.setup;
  g_tmp.wifiOk = WiFi.status() == WL_CONNECTED;
  g_tmp.bridgeOk = anyOk;
  g_tmp.nBridges = g_nbr;
  g_tmp.nBridgesOk = nOk;
  g_tmp.nCandidates = g_ncand;
  cp(g_tmp.candHost, sizeof(g_tmp.candHost), g_ncand ? g_cand[0].host : "");
  for (int i = 0; i < g_nbr; i++) cp(g_tmp.macs[i].host, sizeof(g_tmp.macs[i].host), g_br[i].host);
  g_tmp.seq = g_state.seq + 1;
  memcpy(&g_state, &g_tmp, sizeof(g_state));
  xSemaphoreGive(g_mtx);
}

static void netTask(void *) {
  Serial.printf("[net] Claude Dash firmware %s\n", DASH_VERSION);
  setMode(MODE_BOOT, "CLAUDE DASH", "", "Starting " DASH_VERSION "...");
  connectWifi();
  uint32_t lastDiscovery = 0;
  uint32_t lastReached = millis();   // last time any paired Mac answered
  uint8_t kicks = 0;
  for (;;) {
    checkResetButton();
    checkSerial();
    if (WiFi.status() != WL_CONNECTED) connectWifi();

    // ---- first Mac: look, then pair with the first one found
    if (g_nbr == 0) {
      setMode(MODE_DISCOVER, "LOOKING FOR YOUR MAC", "",
              "Install the desk-display plugin in Claude Code on a Mac on this Wi-Fi, or plug me into it");
      discoverAll(true);
      if (g_ncand) {
        Candidate c = g_cand[0];
        pairWith(c, true);
      } else {
        waitMs(3000);
      }
      continue;
    }

    // ---- requests from the screen
    if (g_updateRequested) { g_updateRequested = false; doUpdate(); }
    if (g_pairRequested) {
      g_pairRequested = false;
      if (g_ncand) { Candidate c = g_cand[0]; pairWith(c, false); }
    }
    Decision d;
    while (xQueueReceive(g_decQ, &d, 0) == pdTRUE) postDecision(d);

    // ---- find Macs: often while a paired one is missing, otherwise once a minute
    bool missing = false;
    for (int i = 0; i < g_nbr; i++) if (!g_br[i].base[0]) missing = true;
    if (millis() - lastDiscovery > (missing ? 15000UL : 60000UL) || lastDiscovery == 0) {
      discoverAll(missing);
      lastDiscovery = millis();
    }

    // ---- poll every paired Mac and merge
    memset(&g_tmp, 0, sizeof(g_tmp));
    uint8_t nOk = 0;
    for (int i = 0; i < g_nbr; i++) {
      Bridge &b = g_br[i];
      if (!b.base[0]) continue;
      String body;
      int code = httpGet(b.base, "/state?ascii=1", body, b.token, nOk ? 1500 : 3000);
      if (code == 401) { removeBridge(i); i--; lastDiscovery = 0; continue; }
      if (code == 200 && mergeState(body, g_tmp, i)) { b.fails = 0; nOk++; continue; }
      if (++b.fails >= 3) { b.base[0] = 0; b.fails = 0; }   // Mac asleep / moved: look for it again
    }
    publish(nOk > 0, nOk);

    // Wi-Fi watchdog: the link can die silently while WiFi.status() still says
    // connected. If no paired Mac has answered for a while, reconnect; then restart.
    if (nOk) { lastReached = millis(); kicks = 0; }
    else if (millis() - lastReached > 45000) {
      if (++kicks >= 3) { Serial.println("[net] still unreachable - restarting"); delay(200); ESP.restart(); }
      Serial.printf("[net] no Mac reachable for 45 s - reconnecting Wi-Fi (%d)\n", kicks);
      WiFi.disconnect(false, false);
      vTaskDelay(pdMS_TO_TICKS(300));
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) waitMs(250);
      lastReached = millis();
      lastDiscovery = 0;
    }

    // sleep until next poll, but wake immediately if something was tapped
    xQueuePeek(g_decQ, &d, pdMS_TO_TICKS(nOk ? POLL_MS : 2000));
  }
}

void net_begin() {
  g_mtx = xSemaphoreCreateMutex();
  g_decQ = xQueueCreate(4, sizeof(Decision));
  memset(&g_state, 0, sizeof(g_state));
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  Serial0.begin(115200);   // UART port: Improv from the browser installer works here too
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

static void wakeNet() {
  Decision wake = {};
  cp(wake.behavior, sizeof(wake.behavior), "noop");
  xQueueSend(g_decQ, &wake, 0);
}

void net_start_update() { g_updateRequested = true; wakeNet(); }
void net_pair_candidate() { g_pairRequested = true; wakeNet(); }
void net_cancel_pair() { g_pairCancel = true; }

void net_send_decision(const char *id, const char *behavior, const char *answersJson) {
  Decision d;
  cp(d.id, sizeof(d.id), id);
  cp(d.behavior, sizeof(d.behavior), behavior);
  cp(d.answers, sizeof(d.answers), answersJson);
  xQueueSend(g_decQ, &d, 0);
}
