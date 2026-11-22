// LVGL 8.3 UI for the 480x480 round panel.
// Enable in lv_conf.h for best looks: LV_FONT_MONTSERRAT_14, _20, _28, _40
#include <lvgl.h>
#include "ui.h"
#include "board.h"

// ---------- fonts (fall back to whatever lv_conf.h has) ----------
#if LV_FONT_MONTSERRAT_40
  #define F_XL &lv_font_montserrat_40
#elif LV_FONT_MONTSERRAT_36
  #define F_XL &lv_font_montserrat_36
#elif LV_FONT_MONTSERRAT_32
  #define F_XL &lv_font_montserrat_32
#else
  #define F_XL LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_28
  #define F_L &lv_font_montserrat_28
#elif LV_FONT_MONTSERRAT_24
  #define F_L &lv_font_montserrat_24
#else
  #define F_L LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_20
  #define F_M &lv_font_montserrat_20
#elif LV_FONT_MONTSERRAT_18
  #define F_M &lv_font_montserrat_18
#elif LV_FONT_MONTSERRAT_16
  #define F_M &lv_font_montserrat_16
#else
  #define F_M LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_14
  #define F_S &lv_font_montserrat_14
#else
  #define F_S LV_FONT_DEFAULT
#endif

// ---------- palette ----------
#define C_BG     lv_color_hex(0x141413)
#define C_CARD   lv_color_hex(0x1E1D1A)
#define C_TRACK  lv_color_hex(0x2A2925)
#define C_TEXT   lv_color_hex(0xF0EEE6)
#define C_MUTED  lv_color_hex(0x8A877D)
#define C_ACCENT lv_color_hex(0xD97757)
#define C_WEEK   lv_color_hex(0xC2B8A3)
#define C_GREEN  lv_color_hex(0x7FB77E)
#define C_AMBER  lv_color_hex(0xE8B04B)
#define C_RED    lv_color_hex(0xE5544B)
#define C_BTN    lv_color_hex(0x3A3935)

struct Row { lv_obj_t *box, *dot, *name, *detail, *badge; };

static lv_obj_t *arc5, *arcWeek, *lblPct, *lblSub, *lblWeek, *list, *lblEmpty, *lblStatus;
static Row rows[MAX_SESSIONS];
static lv_obj_t *setupScr, *lblSetupTitle, *lblSetupBig, *lblSetupSub, *lblSetupHint, *qr;
static char lastQr[96] = "";
static char clockText[8] = "--:--";
static lv_obj_t *dec, *lblDecTitle, *lblDecSession, *lblDecTool, *lblDecDetail, *btnAllow, *btnDeny, *lblDecHint;

static char curPending[24] = "";
static struct { char id[24]; uint32_t at; } sent[6];
static int workingCount = 0;
static bool linkOk = false, wifiOk = false;

// ---------- helpers ----------
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

static lv_obj_t *ring(lv_obj_t *parent, int size, int width, lv_color_t c) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, size, size);
  lv_obj_center(a);
  lv_arc_set_bg_angles(a, 135, 45);
  lv_arc_set_range(a, 0, 100);
  lv_arc_set_value(a, 0);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  return a;
}

static void fmtDur(long s, char *out, size_t n) {
  if (s < 0) { strlcpy(out, "?", n); return; }
  long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d) snprintf(out, n, "%ldd %ldh", d, h);
  else if (h) snprintf(out, n, "%ldh %02ldm", h, m);
  else snprintf(out, n, "%ldm", m);
}

static lv_color_t stateColor(const char *st) {
  if (!strcmp(st, "working")) return C_GREEN;
  if (!strcmp(st, "decision")) return C_ACCENT;
  if (!strcmp(st, "waiting")) return C_AMBER;
  return C_MUTED;
}

static bool wasSent(const char *id) {
  for (auto &s : sent)
    if (s.id[0] && !strcmp(s.id, id) && millis() - s.at < 15000) return true;
  return false;
}

static void markSent(const char *id) {
  int oldest = 0;
  for (int i = 1; i < 6; i++) if (sent[i].at < sent[oldest].at) oldest = i;
  strlcpy(sent[oldest].id, id, sizeof(sent[oldest].id));
  sent[oldest].at = millis();
}

static void setButtonsEnabled(bool en) {
  if (en) { lv_obj_clear_state(btnAllow, LV_STATE_DISABLED); lv_obj_clear_state(btnDeny, LV_STATE_DISABLED); }
  else    { lv_obj_add_state(btnAllow, LV_STATE_DISABLED);   lv_obj_add_state(btnDeny, LV_STATE_DISABLED); }
}

static void onDecision(lv_event_t *e) {
  bool allow = (bool)(intptr_t)lv_event_get_user_data(e);
  if (!curPending[0]) return;
  net_send_decision(curPending, allow ? "allow" : "deny");
  markSent(curPending);
  setButtonsEnabled(false);
  lv_label_set_text(lblDecHint, allow ? "Allowed - sending..." : "Denied - sending...");
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_color_t bg, int x, bool allow) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 150, 72);
  lv_obj_align(b, LV_ALIGN_CENTER, x, 101);
  lv_obj_set_style_radius(b, 36, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_set_style_transform_width(b, -4, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(b, -4, LV_STATE_PRESSED);
  lv_obj_t *l = label(b, F_M, C_TEXT);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, onDecision, LV_EVENT_CLICKED, (void *)(intptr_t)allow);
  return b;
}

// ---------- multiple-choice questions (AskUserQuestion) ----------
static lv_obj_t *qdlg, *lblQTitle, *lblQSession, *lblQText, *btnSkip, *btnDone;
static lv_obj_t *optBtn[MAX_OPTIONS], *optLbl[MAX_OPTIONS];
static PendingInfo curQ;                    // copy of the question set being answered
static uint8_t qIdx = 0;
static uint8_t qPicked[MAX_QUESTIONS];      // bitmask of chosen options per question

static void setQuestionButtonsEnabled(bool en) {
  for (int i = 0; i < MAX_OPTIONS; i++) {
    if (en) lv_obj_clear_state(optBtn[i], LV_STATE_DISABLED); else lv_obj_add_state(optBtn[i], LV_STATE_DISABLED);
  }
  if (en) { lv_obj_clear_state(btnSkip, LV_STATE_DISABLED); lv_obj_clear_state(btnDone, LV_STATE_DISABLED); }
  else    { lv_obj_add_state(btnSkip, LV_STATE_DISABLED);   lv_obj_add_state(btnDone, LV_STATE_DISABLED); }
}

static void showQuestion() {
  const QuestionInfo &q = curQ.questions[qIdx];
  const char *head = q.header[0] ? q.header : "QUESTION";
  if (curQ.nQuestions > 1) lv_label_set_text_fmt(lblQTitle, "%s  %d/%d", head, qIdx + 1, curQ.nQuestions);
  else lv_label_set_text(lblQTitle, head);
  lv_label_set_text(lblQText, q.text);
  for (int i = 0; i < MAX_OPTIONS; i++) {
    if (i >= q.nOpts) { lv_obj_add_flag(optBtn[i], LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(optBtn[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(optLbl[i], q.opts[i]);
    lv_obj_clear_state(optBtn[i], LV_STATE_CHECKED);
    if (q.multi) lv_obj_add_flag(optBtn[i], LV_OBJ_FLAG_CHECKABLE);
    else lv_obj_clear_flag(optBtn[i], LV_OBJ_FLAG_CHECKABLE);
  }
  if (q.multi) {
    lv_obj_clear_flag(btnDone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(btnSkip, LV_ALIGN_BOTTOM_MID, -62, -46);
  } else {
    lv_obj_add_flag(btnDone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(btnSkip, LV_ALIGN_BOTTOM_MID, 0, -46);
  }
  setQuestionButtonsEnabled(true);
}

static void sendAnswers() {
  char json[96];
  size_t n = snprintf(json, sizeof(json), "[");
  for (int qi = 0; qi < curQ.nQuestions; qi++) {
    n += snprintf(json + n, sizeof(json) - n, "%s[", qi ? "," : "");
    bool first = true;
    for (int i = 0; i < MAX_OPTIONS; i++)
      if (qPicked[qi] & (1 << i)) { n += snprintf(json + n, sizeof(json) - n, "%s%d", first ? "" : ",", i); first = false; }
    n += snprintf(json + n, sizeof(json) - n, "]");
  }
  snprintf(json + n, sizeof(json) - n, "]");
  net_send_decision(curQ.id, "allow", json);
  markSent(curQ.id);
  setQuestionButtonsEnabled(false);
  lv_label_set_text(lblQTitle, "ANSWER SENT");
}

static void nextQuestion() {
  if (qIdx + 1 < curQ.nQuestions) { qIdx++; showQuestion(); }
  else sendAnswers();
}

static void onOption(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (curQ.questions[qIdx].multi) {        // CHECKABLE already toggled the state
    if (lv_obj_has_state(optBtn[i], LV_STATE_CHECKED)) qPicked[qIdx] |= (1 << i);
    else qPicked[qIdx] &= ~(1 << i);
    return;
  }
  qPicked[qIdx] = 1 << i;
  nextQuestion();
}

static void onDone(lv_event_t *) {
  if (!qPicked[qIdx]) { lv_label_set_text(lblQTitle, "PICK AT LEAST ONE"); return; }
  nextQuestion();
}

static void onSkip(lv_event_t *) {
  net_send_decision(curQ.id, "skip");
  markSent(curQ.id);
  setQuestionButtonsEnabled(false);
  lv_label_set_text(lblQTitle, "ANSWER ON YOUR MAC");
}

static lv_obj_t *smallButton(lv_obj_t *parent, const char *txt, lv_color_t bg, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 110, 38);
  lv_obj_set_style_radius(b, 19, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_t *l = label(b, F_S, C_TEXT);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static void buildQuestionOverlay() {
  qdlg = lv_obj_create(lv_layer_top());
  lv_obj_set_size(qdlg, 480, 480);
  lv_obj_center(qdlg);
  lv_obj_set_style_radius(qdlg, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(qdlg, C_BG, 0);
  lv_obj_set_style_bg_opa(qdlg, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(qdlg, C_WEEK, 0);
  lv_obj_set_style_border_width(qdlg, 5, 0);
  lv_obj_set_style_pad_all(qdlg, 0, 0);
  lv_obj_clear_flag(qdlg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(qdlg, LV_OBJ_FLAG_HIDDEN);

  lblQTitle = label(qdlg, F_S, C_ACCENT);
  lv_obj_align(lblQTitle, LV_ALIGN_TOP_MID, 0, 44);
  lblQSession = label(qdlg, F_S, C_MUTED);
  lv_obj_align(lblQSession, LV_ALIGN_TOP_MID, 0, 66);

  lblQText = label(qdlg, F_M, C_TEXT);
  lv_obj_set_size(lblQText, 330, 84);
  lv_obj_set_style_text_align(lblQText, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lblQText, LV_LABEL_LONG_DOT);
  lv_obj_align(lblQText, LV_ALIGN_TOP_MID, 0, 92);

  for (int i = 0; i < MAX_OPTIONS; i++) {
    lv_obj_t *b = lv_btn_create(qdlg);
    lv_obj_set_size(b, 300, 44);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 186 + i * 50);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_color(b, C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, C_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, C_TRACK, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    optLbl[i] = label(b, F_S, C_TEXT);
    lv_obj_set_width(optLbl[i], 270);
    lv_obj_set_style_text_align(optLbl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(optLbl[i], LV_LABEL_LONG_DOT);
    lv_obj_center(optLbl[i]);
    lv_obj_add_event_cb(b, onOption, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    optBtn[i] = b;
  }
  btnSkip = smallButton(qdlg, "Skip", C_BTN, onSkip);
  lv_obj_align(btnSkip, LV_ALIGN_BOTTOM_MID, 0, -46);
  btnDone = smallButton(qdlg, "Done", C_ACCENT, onDone);
  lv_obj_align(btnDone, LV_ALIGN_BOTTOM_MID, 62, -46);
}

// ---------- firmware update offer ----------
static lv_obj_t *btnUpdatePill, *lblUpdatePill, *updCard, *lblUpdVer, *lblUpdSub;
static char offeredVer[24] = "";
static char dismissedVer[24] = "";

// "v1.2.3-test" -> 1002003 (suffix ignored); anything unparseable -> 0
static long versionNum(const char *v) {
  if (*v == 'v' || *v == 'V') v++;
  int a = 0, b = 0, c = 0;
  if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return 0;
  return a * 1000000L + b * 1000L + c;
}

static void onUpdatePill(lv_event_t *) {
  lv_label_set_text(lblUpdVer, offeredVer);
  lv_label_set_text(lblUpdSub, "Installed: " DASH_VERSION "\nTakes about a minute; the display restarts.");
  lv_obj_clear_flag(updCard, LV_OBJ_FLAG_HIDDEN);
}

static void onUpdateLater(lv_event_t *) {
  strlcpy(dismissedVer, offeredVer, sizeof(dismissedVer));
  lv_obj_add_flag(updCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btnUpdatePill, LV_OBJ_FLAG_HIDDEN);
}

static void onUpdateNow(lv_event_t *) {
  lv_obj_add_flag(updCard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btnUpdatePill, LV_OBJ_FLAG_HIDDEN);
  net_start_update();
}

static void buildUpdateUi(lv_obj_t *scr) {
  btnUpdatePill = lv_btn_create(scr);
  lv_obj_set_size(btnUpdatePill, 190, 30);
  lv_obj_align(btnUpdatePill, LV_ALIGN_BOTTOM_MID, 0, -74);
  lv_obj_set_style_radius(btnUpdatePill, 15, 0);
  lv_obj_set_style_bg_color(btnUpdatePill, C_CARD, 0);
  lv_obj_set_style_border_color(btnUpdatePill, C_ACCENT, 0);
  lv_obj_set_style_border_width(btnUpdatePill, 1, 0);
  lv_obj_set_style_shadow_width(btnUpdatePill, 0, 0);
  lblUpdatePill = label(btnUpdatePill, F_S, C_ACCENT);
  lv_obj_center(lblUpdatePill);
  lv_obj_add_event_cb(btnUpdatePill, onUpdatePill, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(btnUpdatePill, LV_OBJ_FLAG_HIDDEN);

  updCard = lv_obj_create(lv_layer_top());
  lv_obj_set_size(updCard, 480, 480);
  lv_obj_center(updCard);
  lv_obj_set_style_radius(updCard, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(updCard, C_BG, 0);
  lv_obj_set_style_bg_opa(updCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(updCard, C_ACCENT, 0);
  lv_obj_set_style_border_width(updCard, 5, 0);
  lv_obj_set_style_pad_all(updCard, 0, 0);
  lv_obj_clear_flag(updCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(updCard, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *t = label(updCard, F_S, C_ACCENT);
  lv_label_set_text(t, "FIRMWARE UPDATE");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 90);
  lblUpdVer = label(updCard, F_XL, C_TEXT);
  lv_obj_align(lblUpdVer, LV_ALIGN_TOP_MID, 0, 130);
  lblUpdSub = label(updCard, F_S, C_MUTED);
  lv_obj_set_width(lblUpdSub, 300);
  lv_obj_set_style_text_align(lblUpdSub, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lblUpdSub, LV_LABEL_LONG_WRAP);
  lv_obj_align(lblUpdSub, LV_ALIGN_TOP_MID, 0, 200);

  lv_obj_t *later = lv_btn_create(updCard);
  lv_obj_t *now = lv_btn_create(updCard);
  lv_obj_t *bs[2] = {later, now};
  const char *txt[2] = {"Later", "Update"};
  for (int i = 0; i < 2; i++) {
    lv_obj_set_size(bs[i], 150, 72);
    lv_obj_align(bs[i], LV_ALIGN_CENTER, i ? 82 : -82, 101);
    lv_obj_set_style_radius(bs[i], 36, 0);
    lv_obj_set_style_bg_color(bs[i], i ? C_ACCENT : C_BTN, 0);
    lv_obj_set_style_shadow_width(bs[i], 0, 0);
    lv_obj_t *l = label(bs[i], F_M, C_TEXT);
    lv_label_set_text(l, txt[i]);
    lv_obj_center(l);
  }
  lv_obj_add_event_cb(later, onUpdateLater, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(now, onUpdateNow, LV_EVENT_CLICKED, nullptr);
}

// ---------- build ----------
void ui_init() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  arc5 = ring(scr, 468, 16, C_ACCENT);
  arcWeek = ring(scr, 428, 8, C_WEEK);

  lblPct = label(scr, F_XL, C_TEXT);
  lv_obj_align(lblPct, LV_ALIGN_TOP_MID, 0, 50);
  lv_label_set_text(lblPct, "--");
  lblSub = label(scr, F_S, C_MUTED);
  lv_obj_align(lblSub, LV_ALIGN_TOP_MID, 0, 104);
  lv_label_set_text(lblSub, "Connecting...");
  lblWeek = label(scr, F_S, C_WEEK);
  lv_obj_align(lblWeek, LV_ALIGN_TOP_MID, 0, 126);

  list = lv_obj_create(scr);
  lv_obj_set_size(list, 340, 210);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 160);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

  for (int i = 0; i < MAX_SESSIONS; i++) {
    Row &r = rows[i];
    r.box = lv_obj_create(list);
    lv_obj_set_size(r.box, 340, 52);
    lv_obj_set_style_bg_color(r.box, C_CARD, 0);
    lv_obj_set_style_border_width(r.box, 0, 0);
    lv_obj_set_style_radius(r.box, 12, 0);
    lv_obj_set_style_pad_all(r.box, 0, 0);
    lv_obj_clear_flag(r.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);

    r.dot = lv_obj_create(r.box);
    lv_obj_set_size(r.dot, 10, 10);
    lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r.dot, 0, 0);
    lv_obj_align(r.dot, LV_ALIGN_LEFT_MID, 10, 0);

    r.name = label(r.box, F_M, C_TEXT);
    lv_obj_set_width(r.name, 200);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 28, 6);

    r.detail = label(r.box, F_S, C_MUTED);
    lv_obj_set_width(r.detail, 300);
    lv_label_set_long_mode(r.detail, LV_LABEL_LONG_DOT);
    lv_obj_align(r.detail, LV_ALIGN_BOTTOM_LEFT, 28, -6);

    r.badge = label(r.box, F_S, C_MUTED);
    lv_obj_align(r.badge, LV_ALIGN_TOP_RIGHT, -12, 9);
  }

  lblEmpty = label(scr, F_S, C_MUTED);
  lv_label_set_text(lblEmpty, "No active Claude sessions");
  lv_obj_align(lblEmpty, LV_ALIGN_CENTER, 0, 20);

  lblStatus = label(scr, F_S, C_MUTED);
  lv_label_set_recolor(lblStatus, true);
  lv_obj_align(lblStatus, LV_ALIGN_BOTTOM_MID, 0, -46);

  buildUpdateUi(scr);

  // ----- setup overlay: Wi-Fi setup, discovery, pairing -----
  setupScr = lv_obj_create(lv_layer_top());
  lv_obj_set_size(setupScr, 480, 480);
  lv_obj_center(setupScr);
  lv_obj_set_style_radius(setupScr, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(setupScr, C_BG, 0);
  lv_obj_set_style_bg_opa(setupScr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(setupScr, 0, 0);
  lv_obj_set_style_pad_all(setupScr, 0, 0);
  lv_obj_clear_flag(setupScr, LV_OBJ_FLAG_SCROLLABLE);

  lblSetupTitle = label(setupScr, F_S, C_ACCENT);
  lv_obj_align(lblSetupTitle, LV_ALIGN_TOP_MID, 0, 70);
  lv_label_set_text(lblSetupTitle, "CLAUDE DASH");
  lblSetupBig = label(setupScr, F_XL, C_TEXT);
  lv_obj_set_style_text_letter_space(lblSetupBig, 10, 0);
  lv_obj_align(lblSetupBig, LV_ALIGN_CENTER, 0, -20);
#if LV_USE_QRCODE
  qr = lv_qrcode_create(setupScr, 170, C_BG, C_TEXT);
  lv_obj_set_style_border_color(qr, C_TEXT, 0);
  lv_obj_set_style_border_width(qr, 8, 0);
  lv_obj_align(qr, LV_ALIGN_CENTER, 0, -10);
  lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
#endif
  lblSetupSub = label(setupScr, F_S, C_TEXT);
  lv_obj_set_width(lblSetupSub, 300);
  lv_obj_set_style_text_align(lblSetupSub, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lblSetupSub, LV_LABEL_LONG_WRAP);
  lv_obj_align(lblSetupSub, LV_ALIGN_CENTER, 0, 110);
  lv_label_set_text(lblSetupSub, "Starting...");
  lblSetupHint = label(setupScr, F_S, C_MUTED);
  lv_obj_align(lblSetupHint, LV_ALIGN_BOTTOM_MID, 0, -50);

  // ----- decision overlay (top layer so it covers everything) -----
  dec = lv_obj_create(lv_layer_top());
  lv_obj_set_size(dec, 480, 480);
  lv_obj_center(dec);
  lv_obj_set_style_radius(dec, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dec, C_BG, 0);
  lv_obj_set_style_bg_opa(dec, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(dec, C_ACCENT, 0);
  lv_obj_set_style_border_width(dec, 5, 0);
  lv_obj_set_style_pad_all(dec, 0, 0);
  lv_obj_clear_flag(dec, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(dec, LV_OBJ_FLAG_HIDDEN);

  lblDecTitle = label(dec, F_S, C_ACCENT);
  lv_obj_align(lblDecTitle, LV_ALIGN_TOP_MID, 0, 54);
  lblDecSession = label(dec, F_M, C_TEXT);
  lv_obj_align(lblDecSession, LV_ALIGN_TOP_MID, 0, 80);
  lblDecTool = label(dec, F_L, C_TEXT);
  lv_obj_align(lblDecTool, LV_ALIGN_TOP_MID, 0, 108);

  lv_obj_t *box = lv_obj_create(dec);
  lv_obj_set_size(box, 340, 130);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 156);
  lv_obj_set_style_bg_color(box, C_CARD, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 12, 0);
  lv_obj_set_style_pad_all(box, 12, 0);
  lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
  lblDecDetail = label(box, F_S, C_TEXT);
  lv_obj_set_width(lblDecDetail, 316);
  lv_label_set_long_mode(lblDecDetail, LV_LABEL_LONG_WRAP);

  btnDeny = button(dec, "Deny", C_BTN, -82, false);
  btnAllow = button(dec, "Allow", C_ACCENT, 82, true);

  lblDecHint = label(dec, F_S, C_MUTED);
  lv_obj_align(lblDecHint, LV_ALIGN_BOTTOM_MID, 0, -42);

  buildQuestionOverlay();
}

// ---------- update ----------
void ui_update(const DashState &s) {
  char a[24], b[24], buf[96];
  wifiOk = s.wifiOk;
  linkOk = s.bridgeOk;
  if (s.clock[0]) strlcpy(clockText, s.clock, sizeof(clockText));

  // setup / pairing screens cover the dashboard until we're running
  if (s.mode != MODE_RUN) {
    lv_obj_clear_flag(setupScr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dec, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(qdlg, LV_OBJ_FLAG_HIDDEN);
    curPending[0] = 0;
    lv_label_set_text(lblSetupTitle, s.setup.title);
    lv_label_set_text(lblSetupBig, s.setup.big);
    lv_label_set_text(lblSetupSub, s.setup.sub);
    lv_label_set_text(lblSetupHint, (s.mode == MODE_PAIR || s.mode == MODE_PORTAL || s.mode == MODE_UPDATE) ? "" : "Hold BOOT 5 s to reset");
    lv_obj_add_flag(updCard, LV_OBJ_FLAG_HIDDEN);
#if LV_USE_QRCODE
    if (s.setup.qr[0]) {
      if (strcmp(lastQr, s.setup.qr)) {
        strlcpy(lastQr, s.setup.qr, sizeof(lastQr));
        lv_qrcode_update(qr, lastQr, strlen(lastQr));
      }
      lv_obj_clear_flag(qr, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    return;
  }
  lv_obj_add_flag(setupScr, LV_OBJ_FLAG_HIDDEN);

  // firmware update offer: only for a newer release than what's running
  if (s.bridgeOk && s.fwLatest[0] && versionNum(s.fwLatest) > versionNum(DASH_VERSION) && strcmp(s.fwLatest, dismissedVer)) {
    if (strcmp(offeredVer, s.fwLatest)) {
      strlcpy(offeredVer, s.fwLatest, sizeof(offeredVer));
      lv_label_set_text_fmt(lblUpdatePill, LV_SYMBOL_DOWNLOAD "  Update to %s", offeredVer);
    }
    lv_obj_clear_flag(btnUpdatePill, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(btnUpdatePill, LV_OBJ_FLAG_HIDDEN);
  }

  // usage rings
  if (!s.wifiOk || !s.bridgeOk) {
    lv_label_set_text(lblPct, "--");
    lv_label_set_text(lblSub, s.wifiOk ? "Bridge offline" : "Connecting to Wi-Fi...");
    lv_label_set_text(lblWeek, "");
  } else if (!s.usageOk || !s.fiveHour.valid) {
    lv_label_set_text(lblPct, "--");
    lv_label_set_text(lblSub, s.usageErr[0] ? s.usageErr : "Usage unavailable");
    lv_label_set_text(lblWeek, "");
    lv_arc_set_value(arc5, 0);
    lv_arc_set_value(arcWeek, 0);
  } else {
    float p = s.fiveHour.pct;
    lv_label_set_text_fmt(lblPct, "%d%%", (int)(p + 0.5f));
    fmtDur(s.fiveHour.resetsIn, a, sizeof(a));
    snprintf(buf, sizeof(buf), "5-hour limit - resets %s", a);
    lv_label_set_text(lblSub, buf);
    lv_arc_set_value(arc5, (int)constrain(p + 0.5f, 0, 100));
    lv_obj_set_style_arc_color(arc5, p >= 90 ? C_RED : p >= 75 ? C_AMBER : C_ACCENT, LV_PART_INDICATOR);
    if (s.week.valid) {
      fmtDur(s.week.resetsIn, b, sizeof(b));
      snprintf(buf, sizeof(buf), "Week %d%% - resets %s", (int)(s.week.pct + 0.5f), b);
      lv_label_set_text(lblWeek, buf);
      lv_arc_set_value(arcWeek, (int)constrain(s.week.pct + 0.5f, 0, 100));
    } else {
      lv_label_set_text(lblWeek, "");
    }
  }

  // sessions
  workingCount = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    Row &r = rows[i];
    if (i >= s.nSessions || !s.bridgeOk) { lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN); continue; }
    const SessionInfo &x = s.sessions[i];
    if (!strcmp(x.state, "working")) workingCount++;
    lv_obj_clear_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(r.dot, stateColor(x.state), 0);
    lv_label_set_text(r.name, x.name);
    if (x.tool[0]) { snprintf(buf, sizeof(buf), "%s: %s", x.tool, x.detail); lv_label_set_text(r.detail, buf); }
    else lv_label_set_text(r.detail, x.detail);
    if (x.subagents) lv_label_set_text_fmt(r.badge, "+%d agent%s", x.subagents, x.subagents > 1 ? "s" : "");
    else if (!strcmp(x.state, "decision")) lv_label_set_text(r.badge, "needs you");
    else if (!strcmp(x.state, "waiting")) lv_label_set_text(r.badge, "waiting");
    else if (!strcmp(x.state, "idle")) lv_label_set_text(r.badge, "idle");
    else lv_label_set_text(r.badge, "");
    lv_obj_set_style_text_color(r.badge, strcmp(x.state, "decision") ? C_MUTED : C_ACCENT, 0);
  }
  if (s.bridgeOk && s.nSessions == 0) lv_obj_clear_flag(lblEmpty, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(lblEmpty, LV_OBJ_FLAG_HIDDEN);

  // decisions: show the oldest one we haven't answered yet
  const PendingInfo *p = nullptr;
  if (s.bridgeOk)
    for (int i = 0; i < s.nPending; i++)
      if (!wasSent(s.pending[i].id)) { p = &s.pending[i]; break; }

  if (!p) {
    lv_obj_add_flag(dec, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(qdlg, LV_OBJ_FLAG_HIDDEN);
    curPending[0] = 0;
  } else if (p->nQuestions) {
    lv_obj_add_flag(dec, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(qdlg, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(curPending, p->id)) {
      strlcpy(curPending, p->id, sizeof(curPending));
      memcpy(&curQ, p, sizeof(curQ));
      qIdx = 0;
      memset(qPicked, 0, sizeof(qPicked));
      showQuestion();
      board_beep();
    }
    lv_label_set_text_fmt(lblQSession, "%s - %ds", p->session, p->expiresIn);
  } else {
    lv_obj_add_flag(qdlg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dec, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(curPending, p->id)) {
      strlcpy(curPending, p->id, sizeof(curPending));
      if (s.nPending > 1) lv_label_set_text_fmt(lblDecTitle, "PERMISSION NEEDED  1/%d", s.nPending);
      else lv_label_set_text(lblDecTitle, "PERMISSION NEEDED");
      lv_label_set_text(lblDecSession, p->session);
      lv_label_set_text(lblDecTool, p->tool);
      lv_label_set_text(lblDecDetail, p->detail);
      setButtonsEnabled(true);
      board_beep();
    }
    lv_label_set_text_fmt(lblDecHint, "Terminal takes over in %ds", p->expiresIn);
  }
}

void ui_tick() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  const char *dotColor = linkOk ? "7FB77E" : wifiOk ? "E8B04B" : "E5544B";
  lv_label_set_text_fmt(lblStatus, "#%s " LV_SYMBOL_WIFI "#  %s  -  %d working", dotColor, clockText, workingCount);
}
