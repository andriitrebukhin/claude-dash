#pragma once
#include <Arduino.h>

#ifndef DASH_VERSION
#define DASH_VERSION "dev"   // release builds pass -DDASH_VERSION="vX.Y.Z"
#endif

#define MAX_SESSIONS 12     // across all Macs
#define MAX_BRIDGES  4      // paired Macs
#define MAX_PENDING  4
#define MAX_QUESTIONS 4
#define MAX_OPTIONS   4

enum DashMode : uint8_t {
  MODE_BOOT,       // starting up
  MODE_PORTAL,     // no Wi-Fi yet: phone setup hotspot running
  MODE_WIFI,       // joining Wi-Fi
  MODE_DISCOVER,   // looking for the Mac bridge (Bonjour)
  MODE_PAIR,       // showing a pairing code, waiting for approval on the Mac
  MODE_RUN,        // normal dashboard
  MODE_UPDATE,     // downloading + installing new firmware
};

struct SessionInfo {
  char name[32];
  char state[12];    // working | decision | waiting | idle
  char tool[28];
  char detail[72];
  uint8_t subagents;
  uint8_t bridge;          // which Mac (page) it belongs to
};

struct QuestionInfo {     // one AskUserQuestion question
  char header[20];
  char text[164];
  bool multi;
  uint8_t nOpts;
  char opts[MAX_OPTIONS][44];
};

struct PendingInfo {
  char id[24];
  char session[32];
  char tool[28];
  char detail[260];
  int  expiresIn;
  uint8_t bridge;         // which paired Mac asked
  uint8_t nQuestions;     // >0: a multiple-choice question instead of Allow/Deny
  QuestionInfo questions[MAX_QUESTIONS];
};

struct UsageWindow {
  bool  valid;
  float pct;
  long  resetsIn;    // seconds, -1 unknown
};

struct SetupInfo {
  char title[40];
  char big[24];      // e.g. the pairing code
  char sub[120];
  char qr[96];       // QR payload, empty = none
};

struct MacInfo {           // one paired Mac = one page
  char host[28];
  bool ok;                 // answering right now
  bool usageOk;
  char usageErr[48];
  UsageWindow fiveHour;
  UsageWindow week;
};

struct DashState {
  uint8_t mode;
  SetupInfo setup;
  bool wifiOk;
  bool bridgeOk;
  char clock[8];     // HH:MM from the Mac
  uint8_t nSessions;
  SessionInfo sessions[MAX_SESSIONS];
  uint8_t nPending;
  PendingInfo pending[MAX_PENDING];
  MacInfo macs[MAX_BRIDGES];
  uint8_t nBridges;    // paired Macs
  uint8_t nBridgesOk;  // ...reachable right now
  uint8_t nCandidates; // unpaired Macs seen on the network
  char candHost[28];   // name of the first one, for the "Pair" button
  bool pairCancelable; // MODE_PAIR for an extra Mac: tap to cancel
  char fwLatest[24];   // newest firmware the bridge can serve ("" = none)
  uint32_t fwSize;
  char fwMd5[36];
  uint32_t seq;
};

void net_begin();
// Copies the latest state into `out` if it changed since `lastSeq`. Returns true if copied.
bool net_snapshot(DashState &out, uint32_t &lastSeq);
// behavior: "allow" | "deny" | "skip". answersJson: for questions, JSON like [[0],[1,2]]
// (chosen option indexes per question).
// Ask the net task to download and install the firmware the bridge offers.
void net_start_update();
// Pair with the first unpaired Mac seen on the network / stop pairing.
void net_pair_candidate();
void net_cancel_pair();
void net_send_decision(const char *id, const char *behavior, const char *answersJson = nullptr);
