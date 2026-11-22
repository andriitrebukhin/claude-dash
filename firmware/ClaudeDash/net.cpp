// Wi-Fi + bridge polling on core 0. LVGL is never touched here; the UI reads
// a copy of the state through net_snapshot().
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>   // v7
#include <time.h>
#include "config.h"
#include "dash_state.h"

struct Decision { char id[24]; bool allow; };

static DashState g_state;
static DashState g_tmp;               // parse buffer, only used by the net task
static SemaphoreHandle_t g_mtx;
static QueueHandle_t g_decQ;
static IPAddress g_bridgeIp;
static bool g_ipResolved = false;

static void cp(char *dst, size_t n, const char *src) { strlcpy(dst, src ? src : "", n); }

static String bridgeBase() {
  String host = BRIDGE_HOST;
  if (host.endsWith(".local")) {
    if (!g_ipResolved) {
      IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
      if (ip != IPAddress(0, 0, 0, 0)) { g_bridgeIp = ip; g_ipResolved = true; }
    }
    if (g_ipResolved) host = g_bridgeIp.toString();
  }
  return "http://" + host + ":" + String(BRIDGE_PORT);
}

static void parseWindow(JsonVariantConst w, UsageWindow &o) {
  o.valid = !w.isNull() && !w["pct"].isNull();
  o.pct = w["pct"] | 0.0f;
  o.resetsIn = w["resets_in"] | -1L;
}

static bool parseState(const String &body, DashState &st) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  memset(&st, 0, sizeof(st));

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
  }
  return true;
}

static void postDecision(const Decision &d) {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(bridgeBase() + "/decision");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", BRIDGE_TOKEN);
  String body = String("{\"id\":\"") + d.id + "\",\"behavior\":\"" + (d.allow ? "allow" : "deny") + "\"}";
  int code = http.POST(body);
  Serial.printf("[net] decision %s %s -> %d\n", d.id, d.allow ? "allow" : "deny", code);
  http.end();
}

static void setLink(bool wifi, bool bridge) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_state.wifiOk != wifi || g_state.bridgeOk != bridge) {
    g_state.wifiOk = wifi;
    g_state.bridgeOk = bridge;
    g_state.seq++;
  }
  xSemaphoreGive(g_mtx);
}

static void netTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t lastTry = millis();
  bool started = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      setLink(false, false);
      if (millis() - lastTry > 15000) {
        Serial.println("[net] reconnecting Wi-Fi");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        lastTry = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!started) {
      Serial.printf("[net] Wi-Fi up, IP %s\n", WiFi.localIP().toString().c_str());
      MDNS.begin("claude-dash");
      configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
      started = true;
    }

    Decision d;
    while (xQueueReceive(g_decQ, &d, 0) == pdTRUE) postDecision(d);

    HTTPClient http;
    http.setTimeout(3000);
    http.begin(bridgeBase() + "/state?ascii=1");
    http.addHeader("X-Token", BRIDGE_TOKEN);
    int code = http.GET();
    bool ok = code == 200 && parseState(http.getString(), g_tmp);
    http.end();
    if (code == 401) Serial.println("[net] bridge rejected token - check BRIDGE_TOKEN");

    if (ok) {
      xSemaphoreTake(g_mtx, portMAX_DELAY);
      g_tmp.wifiOk = true;
      g_tmp.bridgeOk = true;
      g_tmp.seq = g_state.seq + 1;
      memcpy(&g_state, &g_tmp, sizeof(g_state));
      xSemaphoreGive(g_mtx);
    } else {
      g_ipResolved = false;   // re-resolve .local next time
      setLink(true, false);
    }

    // sleep until next poll, but wake immediately if a decision was tapped
    xQueuePeek(g_decQ, &d, pdMS_TO_TICKS(ok ? POLL_MS : 3000));
  }
}

void net_begin() {
  g_mtx = xSemaphoreCreateMutex();
  g_decQ = xQueueCreate(4, sizeof(Decision));
  memset(&g_state, 0, sizeof(g_state));
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
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

void net_send_decision(const char *id, bool allow) {
  Decision d;
  cp(d.id, sizeof(d.id), id);
  d.allow = allow;
  xQueueSend(g_decQ, &d, 0);
}
