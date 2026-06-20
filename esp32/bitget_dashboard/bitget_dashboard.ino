/* ============================================================================
 *  Bitget Tracker — ESP32 LVGL Dashboard  (PORTRAIT, single scroll page)
 *  Board : CYD "Cheap Yellow Display" ESP32-2432S028R
 *          ESP32 + ILI9341 320x240 SPI TFT + XPT2046 resistive touch
 *
 *  Vertical 240x320 dashboard that combines ALL of your Bitget income in one
 *  scrollable view: grand total balance, all-time profit, today/open P&L,
 *  income-source breakdown (copy trading / elite / earn), full elite portfolio,
 *  earn balance and a per-trader list.
 *
 *  Pulls one compact JSON from the tracker backend (NOT from Bitget directly):
 *      GET  {SERVER_URL}/api/esp32
 *
 *  Libraries (Arduino Library Manager):
 *    - LVGL 8.3.x · TFT_eSPI · XPT2046_Touchscreen · ArduinoJson 7.x
 *  Setup: see esp32/README.md (copy User_Setup.h + lv_conf.h, fill creds below).
 * ========================================================================== */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

/* ── USER CONFIG ─────────────────────────────────────────────────────────── */
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
static const char *SERVER_URL = "https://YOUR-SERVICE-NAME.onrender.com";  // no trailing slash
static const uint32_t FETCH_INTERVAL_MS = 30000;

/* ── Touch (XPT2046) — own SPI bus on the CYD ────────────────────────────── */
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

// PORTRAIT calibration. If touch/scroll is off, set TOUCH_DEBUG 1, read raw
// values from Serial at the four corners, and adjust these.
#define TOUCH_DEBUG   0
static int TS_MINX = 200,  TS_MAXX = 3700;
static int TS_MINY = 240,  TS_MAXY = 3800;

/* ── Display geometry (PORTRAIT) ─────────────────────────────────────────── */
static const uint16_t SCR_W = 240;
static const uint16_t SCR_H = 320;

/* ── Theme colours ───────────────────────────────────────────────────────── */
#define COL_BG     lv_color_hex(0x0E1116)
#define COL_CARD   lv_color_hex(0x1B2027)
#define COL_CARD2  lv_color_hex(0x232A33)
#define COL_TEXT   lv_color_hex(0xFFFFFF)
#define COL_MUTED  lv_color_hex(0x8A93A0)
#define COL_GREEN  lv_color_hex(0x00C47A)
#define COL_RED    lv_color_hex(0xFF4D4D)
#define COL_AMBER  lv_color_hex(0xF59E0B)
#define COL_ACCENT lv_color_hex(0x4DA3FF)

/* ── Globals ─────────────────────────────────────────────────────────────── */
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCR_W * 10];

// Header
static lv_obj_t *lbl_updated, *led_status;
// Hero
static lv_obj_t *lbl_total, *lbl_alltime;
// Quick stats
static lv_obj_t *val_today, *val_open, *val_pos;
// Income breakdown
static lv_obj_t *bd_copy, *bd_elite, *bd_earn, *bd_invested;
// Elite section
static lv_obj_t *elite_card, *e_bal, *e_today, *e_open, *e_all, *e_aum, *e_fans, *e_pos;
// Earn section
static lv_obj_t *earn_card, *earn_total_lbl;
// Traders
static lv_obj_t *traders_box;
// Footer status
static lv_obj_t *foot_lbl;

static uint32_t last_fetch = 0;

/* ── Formatting helpers ──────────────────────────────────────────────────── */
static void fmtUSD(char *out, size_t n, double v) {
  double a = fabs(v);
  char raw[24];
  snprintf(raw, sizeof(raw), "%.2f", a);
  char *dot = strchr(raw, '.');
  int intlen = dot ? (int)(dot - raw) : (int)strlen(raw);
  char grouped[32];
  int gi = 0;
  for (int i = 0; i < intlen; i++) {
    if (i > 0 && (intlen - i) % 3 == 0) grouped[gi++] = ',';
    grouped[gi++] = raw[i];
  }
  grouped[gi] = '\0';
  snprintf(out, n, "$%s%s", grouped, dot ? dot : ".00");
}

static void fmtPnL(char *out, size_t n, double v) {
  char usd[32];
  fmtUSD(usd, sizeof(usd), v);
  snprintf(out, n, "%s%s", v >= 0 ? "+" : "-", usd);
}

static lv_color_t pnlColor(double v) { return v >= 0 ? COL_GREEN : COL_RED; }

static void set_pnl(lv_obj_t *lbl, double v) {
  char buf[40]; fmtPnL(buf, sizeof(buf), v);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, pnlColor(v), 0);
}
static void set_usd(lv_obj_t *lbl, double v) {
  char buf[40]; fmtUSD(buf, sizeof(buf), v);
  lv_label_set_text(lbl, buf);
}

/* ── LVGL display + touch glue ───────────────────────────────────────────── */
static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
#if TOUCH_DEBUG
    Serial.printf("touch raw: x=%d y=%d z=%d\n", p.x, p.y, p.z);
#endif
    int x = map(p.x, TS_MINX, TS_MAXX, 0, SCR_W);
    int y = map(p.y, TS_MINY, TS_MAXY, 0, SCR_H);
    data->point.x = constrain(x, 0, SCR_W - 1);
    data->point.y = constrain(y, 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* ── UI building blocks ──────────────────────────────────────────────────── */
static lv_obj_t *make_card(lv_obj_t *parent, lv_color_t bg) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, bg, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 4, 0);
  return c;
}

// section heading (muted, small, uppercase)
static void section_label(lv_obj_t *parent, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, COL_MUTED, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_pad_top(l, 4, 0);
}

// label-left / value-right row inside a card
static lv_obj_t *kv_row(lv_obj_t *parent, const char *label) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 2, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *l = lv_label_create(row);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_color(l, COL_MUTED, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

  lv_obj_t *v = lv_label_create(row);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_color(v, COL_TEXT, 0);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
  return v;  // caller stores the value label
}

// one of the 3 small stat tiles (TODAY / OPEN / POS)
static lv_obj_t *stat_tile(lv_obj_t *parent, const char *title) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_flex_grow(c, 1);
  lv_obj_set_height(c, 56);
  lv_obj_set_style_bg_color(c, COL_CARD, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 4, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *t = lv_label_create(c);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, COL_MUTED, 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);

  lv_obj_t *v = lv_label_create(c);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_color(v, COL_TEXT, 0);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
  return v;
}

/* ── Build the scrolling dashboard ───────────────────────────────────────── */
static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_pad_all(scr, 8, 0);
  lv_obj_set_style_pad_row(scr, 8, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(scr, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);

  /* Header: title + status dot + time */
  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_set_width(header, LV_PCT(100));
  lv_obj_set_height(header, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(header);
  lv_label_set_text(title, "BITGET");
  lv_obj_set_style_text_color(title, COL_TEXT, 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_flex_grow(title, 1);

  led_status = lv_led_create(header);
  lv_obj_set_size(led_status, 10, 10);
  lv_led_set_color(led_status, COL_AMBER);
  lv_led_on(led_status);

  lbl_updated = lv_label_create(header);
  lv_label_set_text(lbl_updated, "--:--");
  lv_obj_set_style_text_color(lbl_updated, COL_MUTED, 0);
  lv_obj_set_style_text_font(lbl_updated, &lv_font_montserrat_12, 0);
  lv_obj_set_style_pad_left(lbl_updated, 6, 0);

  /* Hero: grand total balance + all-time profit */
  lv_obj_t *hero = make_card(scr, COL_CARD);
  lv_obj_set_style_pad_all(hero, 12, 0);
  lv_obj_t *cap = lv_label_create(hero);
  lv_label_set_text(cap, "TOTAL BALANCE");
  lv_obj_set_style_text_color(cap, COL_MUTED, 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);

  lbl_total = lv_label_create(hero);
  lv_label_set_text(lbl_total, "$--");
  lv_obj_set_style_text_color(lbl_total, COL_TEXT, 0);
  lv_obj_set_style_text_font(lbl_total, &lv_font_montserrat_28, 0);

  lv_obj_t *atrow = lv_obj_create(hero);
  lv_obj_set_width(atrow, LV_PCT(100));
  lv_obj_set_height(atrow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(atrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(atrow, 0, 0);
  lv_obj_set_style_pad_all(atrow, 0, 0);
  lv_obj_clear_flag(atrow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(atrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(atrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *atl = lv_label_create(atrow);
  lv_label_set_text(atl, "All-time P&L");
  lv_obj_set_style_text_color(atl, COL_MUTED, 0);
  lv_obj_set_style_text_font(atl, &lv_font_montserrat_14, 0);
  lbl_alltime = lv_label_create(atrow);
  lv_label_set_text(lbl_alltime, "--");
  lv_obj_set_style_text_font(lbl_alltime, &lv_font_montserrat_16, 0);

  /* Quick stats: TODAY / OPEN / POS */
  lv_obj_t *stats = lv_obj_create(scr);
  lv_obj_set_width(stats, LV_PCT(100));
  lv_obj_set_height(stats, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(stats, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(stats, 0, 0);
  lv_obj_set_style_pad_all(stats, 0, 0);
  lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(stats, 8, 0);
  val_today = stat_tile(stats, "TODAY");
  val_open  = stat_tile(stats, "OPEN P&L");
  val_pos   = stat_tile(stats, "POSITIONS");

  /* Income sources breakdown */
  section_label(scr, "INCOME SOURCES");
  lv_obj_t *bd = make_card(scr, COL_CARD);
  bd_copy     = kv_row(bd, "Copy trading");
  bd_elite    = kv_row(bd, "Elite portfolio");
  bd_earn     = kv_row(bd, "Earn");
  bd_invested = kv_row(bd, "Invested");

  /* Elite portfolio (hidden until data) */
  section_label(scr, "ELITE PORTFOLIO");
  elite_card = make_card(scr, COL_CARD);
  lv_obj_t *ebrow = lv_obj_create(elite_card);
  lv_obj_set_width(ebrow, LV_PCT(100));
  lv_obj_set_height(ebrow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(ebrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ebrow, 0, 0);
  lv_obj_set_style_pad_all(ebrow, 0, 0);
  lv_obj_clear_flag(ebrow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(ebrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ebrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_t *ebcap = lv_label_create(ebrow);
  lv_label_set_text(ebcap, "Balance");
  lv_obj_set_style_text_color(ebcap, COL_MUTED, 0);
  lv_obj_set_style_text_font(ebcap, &lv_font_montserrat_14, 0);
  e_bal = lv_label_create(ebrow);
  lv_label_set_text(e_bal, "$--");
  lv_obj_set_style_text_color(e_bal, COL_TEXT, 0);
  lv_obj_set_style_text_font(e_bal, &lv_font_montserrat_16, 0);
  e_today = kv_row(elite_card, "Today");
  e_open  = kv_row(elite_card, "Open P&L");
  e_all   = kv_row(elite_card, "All-time");
  e_aum   = kv_row(elite_card, "AUM");
  e_fans  = kv_row(elite_card, "Followers");
  e_pos   = kv_row(elite_card, "Open positions");

  /* Earn (hidden until data) */
  section_label(scr, "EARN");
  earn_card = make_card(scr, COL_CARD);
  earn_total_lbl = kv_row(earn_card, "Balance");

  /* Traders list */
  section_label(scr, "COPY TRADERS");
  traders_box = lv_obj_create(scr);
  lv_obj_set_width(traders_box, LV_PCT(100));
  lv_obj_set_height(traders_box, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(traders_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(traders_box, 0, 0);
  lv_obj_set_style_pad_all(traders_box, 0, 0);
  lv_obj_clear_flag(traders_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(traders_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(traders_box, 8, 0);

  /* Footer status line */
  foot_lbl = lv_label_create(scr);
  lv_label_set_text(foot_lbl, "connecting...");
  lv_obj_set_style_text_color(foot_lbl, COL_MUTED, 0);
  lv_obj_set_style_text_font(foot_lbl, &lv_font_montserrat_12, 0);
}

/* ── Apply data ──────────────────────────────────────────────────────────── */
static void update_traders(JsonArray traders) {
  lv_obj_clean(traders_box);
  if (traders.size() == 0) {
    lv_obj_t *ph = lv_label_create(traders_box);
    lv_label_set_text(ph, "No active traders");
    lv_obj_set_style_text_color(ph, COL_MUTED, 0);
    return;
  }
  for (JsonObject tr : traders) {
    const char *name = tr["n"] | "?";
    double bal = tr["bal"] | 0.0, day = tr["day"] | 0.0, all = tr["all"] | 0.0;
    int pos = tr["pos"] | 0;

    lv_obj_t *c = make_card(traders_box, COL_CARD);
    lv_obj_t *top = lv_obj_create(c);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *nm = lv_label_create(top);
    char nmbuf[40];
    snprintf(nmbuf, sizeof(nmbuf), "%s%s", name, pos > 0 ? " *" : "");
    lv_label_set_text(nm, nmbuf);
    lv_obj_set_style_text_color(nm, COL_TEXT, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);

    char balbuf[32]; fmtUSD(balbuf, sizeof(balbuf), bal);
    lv_obj_t *bl = lv_label_create(top);
    lv_label_set_text(bl, balbuf);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);

    lv_obj_t *bot = lv_obj_create(c);
    lv_obj_set_width(bot, LV_PCT(100));
    lv_obj_set_height(bot, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char line[48], pbuf[32];
    fmtPnL(pbuf, sizeof(pbuf), day);
    lv_obj_t *td = lv_label_create(bot);
    snprintf(line, sizeof(line), "Today %s", pbuf);
    lv_label_set_text(td, line);
    lv_obj_set_style_text_color(td, pnlColor(day), 0);
    lv_obj_set_style_text_font(td, &lv_font_montserrat_12, 0);
    fmtPnL(pbuf, sizeof(pbuf), all);
    lv_obj_t *at = lv_label_create(bot);
    snprintf(line, sizeof(line), "All %s", pbuf);
    lv_label_set_text(at, line);
    lv_obj_set_style_text_color(at, pnlColor(all), 0);
    lv_obj_set_style_text_font(at, &lv_font_montserrat_12, 0);
  }
}

static void apply_data(JsonDocument &doc) {
  bool stale = doc["stale"] | true;
  const char *upd = doc["upd"] | "--:--";
  double bal = doc["bal"] | 0.0, inv = doc["inv"] | 0.0;
  double day = doc["day"] | 0.0, open = doc["open"] | 0.0;
  double all = doc["all"] | 0.0, earn = doc["earn"] | 0.0;
  int npos = doc["npos"] | 0;

  lv_label_set_text(lbl_updated, upd);
  lv_led_set_color(led_status, stale ? COL_AMBER : COL_GREEN);

  set_usd(lbl_total, bal);
  set_pnl(lbl_alltime, all);
  set_pnl(val_today, day);
  set_pnl(val_open, open);
  char nb[16]; snprintf(nb, sizeof(nb), "%d", npos);
  lv_label_set_text(val_pos, nb);

  /* Income breakdown: sum trader balances; elite + earn from their blocks */
  JsonArray traders = doc["traders"].as<JsonArray>();
  double copy_total = 0;
  for (JsonObject tr : traders) copy_total += (double)(tr["bal"] | 0.0);
  JsonObject el = doc["elite"].as<JsonObject>();
  bool eon = el["on"] | false;
  double ebal = el["bal"] | 0.0;
  set_usd(bd_copy, copy_total);
  set_usd(bd_elite, eon ? ebal : 0.0);
  set_usd(bd_earn, earn);
  set_usd(bd_invested, inv);

  /* Elite section */
  if (eon) {
    lv_obj_clear_flag(elite_card, LV_OBJ_FLAG_HIDDEN);
    set_usd(e_bal, ebal);
    set_pnl(e_today, el["day"] | 0.0);
    set_pnl(e_open, el["open"] | 0.0);
    set_pnl(e_all, el["all"] | 0.0);
    set_usd(e_aum, el["aum"] | 0.0);
    char b[16];
    snprintf(b, sizeof(b), "%d", (int)(el["fans"] | 0));
    lv_label_set_text(e_fans, b);
    snprintf(b, sizeof(b), "%d", (int)(el["pos"] | 0));
    lv_label_set_text(e_pos, b);
  } else {
    lv_obj_add_flag(elite_card, LV_OBJ_FLAG_HIDDEN);
  }

  /* Earn section — hide if zero */
  if (earn > 0.005) {
    lv_obj_clear_flag(earn_card, LV_OBJ_FLAG_HIDDEN);
    set_usd(earn_total_lbl, earn);
  } else {
    lv_obj_add_flag(earn_card, LV_OBJ_FLAG_HIDDEN);
  }

  update_traders(traders);
}

/* ── Networking ──────────────────────────────────────────────────────────── */
static void fetch_data() {
  if (WiFi.status() != WL_CONNECTED) { lv_label_set_text(foot_lbl, "no wifi"); return; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);

  String url = String(SERVER_URL) + "/api/esp32";
  bool ok = url.startsWith("https") ? http.begin(client, url) : http.begin(url);
  if (!ok) { lv_label_set_text(foot_lbl, "begin failed"); return; }
  int code = http.GET();
  if (code != 200) {
    char s[40]; snprintf(s, sizeof(s), "HTTP %d", code);
    lv_label_set_text(foot_lbl, s);
    http.end(); return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) { lv_label_set_text(foot_lbl, "json error"); return; }
  apply_data(doc);

  char s[64];
  snprintf(s, sizeof(s), "WiFi %ddBm · heap %uKB · OK",
           WiFi.RSSI(), (unsigned)(ESP.getFreeHeap() / 1024));
  lv_label_set_text(foot_lbl, s);
}

static void wifi_connect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print("."); lv_timer_handler();
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ok" : " timeout");
}

/* ── Arduino entry points ────────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  Serial.println("\nBitget ESP32 Dashboard (portrait) booting...");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);   // portrait

  tft.begin();
  tft.setRotation(0);  // portrait, 240x320
  tft.fillScreen(TFT_BLACK);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCR_W * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCR_W;
  disp_drv.ver_res = SCR_H;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  build_ui();

  wifi_connect();
  fetch_data();
  last_fetch = millis();
}

void loop() {
  lv_timer_handler();
  delay(5);
  uint32_t now = millis();
  if (now - last_fetch >= FETCH_INTERVAL_MS) {
    last_fetch = now;
    if (WiFi.status() != WL_CONNECTED) wifi_connect();
    fetch_data();
  }
}
