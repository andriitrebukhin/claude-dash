#pragma once
#include <Arduino.h>

#define MAX_SESSIONS 8
#define MAX_PENDING  4

struct SessionInfo {
  char name[32];
  char state[12];    // working | decision | waiting | idle
  char tool[28];
  char detail[72];
  uint8_t subagents;
};

struct PendingInfo {
  char id[24];
  char session[32];
  char tool[28];
  char detail[260];
  int  expiresIn;
};

struct UsageWindow {
  bool  valid;
  float pct;
  long  resetsIn;    // seconds, -1 unknown
};

struct DashState {
  bool wifiOk;
  bool bridgeOk;
  bool usageOk;
  char usageErr[48];
  UsageWindow fiveHour;
  UsageWindow week;
  uint8_t nSessions;
  SessionInfo sessions[MAX_SESSIONS];
  uint8_t nPending;
  PendingInfo pending[MAX_PENDING];
  uint32_t seq;
};

void net_begin();
// Copies the latest state into `out` if it changed since `lastSeq`. Returns true if copied.
bool net_snapshot(DashState &out, uint32_t &lastSeq);
void net_send_decision(const char *id, bool allow);
