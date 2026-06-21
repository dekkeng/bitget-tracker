/* ============================================================================
 *  Bitget Tracker — ESP32 LVGL Dashboard  (LANDSCAPE, 320x240)
 *  Board : CYD "Cheap Yellow Display" ESP32-2432S028R
 *          ESP32 + ILI9341 320x240 SPI TFT + XPT2046 resistive touch
 *
 *  Same data + endpoints as the portrait sketch — only the orientation and the
 *  page layouts differ. Designed for a wide, short screen:
 *      Home      → the focus. Big TODAY P&L + balance/all-time on the LEFT, a
 *                  compact OPEN/POS/EARN panel on the RIGHT, a small (muted)
 *                  menu row underneath. Everything fits with no scrolling.
 *      Traders   → per-trader cards, tiled 2-up across the width
 *      Elite     → elite portfolio + open positions, tiled 2-up
 *      Positions → all OPEN trades (copy + elite), tiled 2-up
 *      History   → recent CLOSED trades, tiled 2-up
 *      Earn      → earn balance + per-coin holdings
 *
 *  The device never talks to Bitget directly — only this project's backend.
 *  Libraries: LVGL 8.3.x · TFT_eSPI · XPT2046_Touchscreen · ArduinoJson 7.x
 *  Setup: copy User_Setup.h + lv_conf.h as per esp32/README.md, fill secrets.h.
 *
 *  Orientation: tft.setRotation(1) (landscape). Use 3 to flip 180° (and set
 *  ts.setRotation(3) to match). If touch lands on the wrong spot, calibrate
 *  with TOUCH_DEBUG=1 and adjust TS_MINX/MAXX/MINY/MAXY below.
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
#include "secrets.h"
static const uint32_t FETCH_INTERVAL_MS = 30000;

/* ── Touch (XPT2046) — own SPI bus on the CYD ────────────────────────────── */
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

#define TOUCH_DEBUG   0
static int TS_MINX = 200,  TS_MAXX = 3700;
static int TS_MINY = 240,  TS_MAXY = 3800;

/* ── Display geometry (LANDSCAPE) ────────────────────────────────────────── */
static const uint16_t SCR_W = 320;
static const uint16_t SCR_H = 240;

/* ── Theme colours ───────────────────────────────────────────────────────── */
#define COL_BG     lv_color_hex(0x0E1116)
#define COL_CARD   lv_color_hex(0x1B2027)
#define COL_TEXT   lv_color_hex(0xFFFFFF)
#define COL_MUTED  lv_color_hex(0x8A93A0)
#define COL_GREEN  lv_color_hex(0x00C47A)
#define COL_RED    lv_color_hex(0xFF4D4D)
#define COL_AMBER  lv_color_hex(0xF59E0B)
#define COL_ACCENT lv_color_hex(0x4DA3FF)

/* ── Pages ───────────────────────────────────────────────────────────────── */
enum Page { PAGE_HOME = 0, PAGE_MENU, PAGE_TRADERS, PAGE_ELITE, PAGE_POSITIONS,
            PAGE_HISTORY, PAGE_EARN, PAGE_TRADER_DETAIL };
static int  current_page = PAGE_HOME;
static int  pending_nav  = -1;       // set by a button, handled in loop()
static int  trader_detail_idx = 0;   // which trader to open on PAGE_TRADER_DETAIL
static String lastPayload;           // cached /api/esp32 JSON

// Home is built ONCE and kept alive; refreshes only update these label texts
// (no teardown/rebuild → no heap churn, no full-screen flicker every 30s).
static lv_obj_t *home_screen = NULL;
static lv_obj_t *hl_today, *hl_total, *hl_open, *hl_pos, *hl_all, *hl_earn, *hl_footer;

/* ── Globals ─────────────────────────────────────────────────────────────── */
SPIClass touchSPI(HSPI);   // touch on its OWN SPI bus (display uses VSPI)
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
// Double buffer, 30 lines each: DMA transfers one while LVGL renders the other.
// 2 x 320 x 30 x 2B = ~38KB DRAM (LVGL's own pool is on the heap via lv_conf).
static lv_color_t buf1[SCR_W * 30];
static lv_color_t buf2[SCR_W * 30];
static uint32_t last_fetch = 0;

// One persistent TLS client + warm connection reuse — avoids a full ~1-2s
// handshake (and its heap spike) on every 30s refresh and every page tap.
static WiFiClientSecure s_client;

/* ── Formatting helpers ──────────────────────────────────────────────────── */
static void fmtUSD(char *out, size_t n, double v) {
  double a = fabs(v);
  char raw[24]; snprintf(raw, sizeof(raw), "%.2f", a);
  char *dot = strchr(raw, '.');
  int intlen = dot ? (int)(dot - raw) : (int)strlen(raw);
  char g[32]; int gi = 0;
  for (int i = 0; i < intlen; i++) {
    if (i > 0 && (intlen - i) % 3 == 0) g[gi++] = ',';
    g[gi++] = raw[i];
  }
  g[gi] = '\0';
  snprintf(out, n, "$%s%s", g, dot ? dot : ".00");
}
static void fmtPnL(char *out, size_t n, double v) {
  char usd[32]; fmtUSD(usd, sizeof(usd), v);
  snprintf(out, n, "%s%s", v >= 0 ? "+" : "-", usd);
}
static lv_color_t pnlColor(double v) { return v >= 0 ? COL_GREEN : COL_RED; }

/* ── LVGL display + touch glue ───────────────────────────────────────────── */
static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixelsDMA((uint16_t *)&color_p->full, w * h);   // non-blocking, byte-swap by LVGL
  tft.endWrite();
  lv_disp_flush_ready(disp);
}
// Visual touch feedback: a small circle that flashes where you tap.
static lv_obj_t *g_touch_dot = NULL;
static uint32_t  g_dot_until = 0;

static void make_touch_dot() {
  g_touch_dot = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_touch_dot, 26, 26);
  lv_obj_set_style_radius(g_touch_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_touch_dot, COL_ACCENT, 0);
  lv_obj_set_style_bg_opa(g_touch_dot, LV_OPA_50, 0);
  lv_obj_set_style_border_color(g_touch_dot, COL_TEXT, 0);
  lv_obj_set_style_border_width(g_touch_dot, 2, 0);
  lv_obj_set_style_border_opa(g_touch_dot, LV_OPA_80, 0);
  lv_obj_clear_flag(g_touch_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(g_touch_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_touch_dot, LV_OBJ_FLAG_HIDDEN);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
#if TOUCH_DEBUG
    Serial.printf("touch raw: x=%d y=%d z=%d\n", p.x, p.y, p.z);
#endif
    int x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCR_W), 0, SCR_W - 1);
    int y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCR_H), 0, SCR_H - 1);
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (g_touch_dot) {
      lv_obj_set_pos(g_touch_dot, x - 13, y - 13);
      lv_obj_clear_flag(g_touch_dot, LV_OBJ_FLAG_HIDDEN);
      g_dot_until = millis() + 180;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    if (g_touch_dot && millis() >= g_dot_until)
      lv_obj_add_flag(g_touch_dot, LV_OBJ_FLAG_HIDDEN);
  }
}

/* ── Networking ──────────────────────────────────────────────────────────── */
// Cache the raw body — used only for the home payload (other pages stream-parse).
static bool httpGet(const String &path, String &out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.setReuse(true); http.setTimeout(12000);
  String url = String(SERVER_URL) + path;
  bool ok = url.startsWith("https") ? http.begin(s_client, url) : http.begin(url);
  if (!ok) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}
// Parse straight from the socket stream — no big intermediate String.
static bool httpGetJson(const String &path, JsonDocument &doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.setReuse(true); http.setTimeout(12000);
  String url = String(SERVER_URL) + path;
  bool ok = url.startsWith("https") ? http.begin(s_client, url) : http.begin(url);
  if (!ok) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  return !err;
}
static String urlenc(const char *s) {
  String o;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') o += c;
    else { char b[4]; snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

/* ── UI building blocks ──────────────────────────────────────────────────── */
// Full-width card (vertical flex). Pass a width pct for tiled (2-up) layouts.
static lv_obj_t *card_w(lv_obj_t *parent, lv_coord_t wpct) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_width(c, wpct);
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, COL_CARD, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 9, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 3, 0);
  return c;
}
static lv_obj_t *card(lv_obj_t *parent)  { return card_w(parent, LV_PCT(100)); }

// Transparent flex row — used to place equal-width tiles side by side on home.
static lv_obj_t *hrow(lv_obj_t *parent) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 6, 0);
  return r;
}

// Vertical stat tile: small muted title on top, big value below. Grows to fill
// its row equally. `vfont` sizes the value (use 20pt — fits $99,999.99 easily).
static lv_obj_t *vtile(lv_obj_t *parent, const char *title, const lv_font_t *vfont) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_flex_grow(c, 1);
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, COL_CARD, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 7, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 1, 0);
  lv_obj_t *t = lv_label_create(c);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, COL_MUTED, 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
  lv_obj_t *v = lv_label_create(c);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_color(v, COL_TEXT, 0);
  lv_obj_set_style_text_font(v, vfont, 0);
  return v;
}

static void section_label(lv_obj_t *parent, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, COL_MUTED, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_pad_top(l, 2, 0);
}
// label-left / value-right row; returns the value label
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
  return v;
}
static void kv_set_usd(lv_obj_t *v, double n) { char b[40]; fmtUSD(b, sizeof(b), n); lv_label_set_text(v, b); }
static void kv_set_pnl(lv_obj_t *v, double n) {
  char b[40]; fmtPnL(b, sizeof(b), n);
  lv_label_set_text(v, b); lv_obj_set_style_text_color(v, pnlColor(n), 0);
}
static void kv_pct(lv_obj_t *v, double pct) { char b[16]; snprintf(b, sizeof(b), "%.2f%%", pct); lv_label_set_text(v, b); }

static void info_label(lv_obj_t *parent, const char *text, lv_color_t col) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, col, 0);
}

/* ── Navigation ──────────────────────────────────────────────────────────── */
static void nav_event(lv_event_t *e) {
  pending_nav = (int)(intptr_t)lv_event_get_user_data(e);
}
static void trader_event(lv_event_t *e) {
  trader_detail_idx = (int)(intptr_t)lv_event_get_user_data(e);
  pending_nav = PAGE_TRADER_DETAIL;
}

// Show a detail sub-page. The previous screen is deleted UNLESS it's the
// persistent home screen, which must survive so we can return to it cheaply.
static void show_sub(lv_obj_t *s) {
  lv_obj_t *prev = lv_scr_act();
  lv_scr_load(s);
  if (prev && prev != home_screen && prev != s) lv_obj_del(prev);
}

// Fresh sub-page: header (Back + title) then a vertical scroll column.
static lv_obj_t *new_page(const char *title, bool back) {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, COL_BG, 0);
  lv_obj_set_style_pad_all(s, 8, 0);
  lv_obj_set_style_pad_row(s, 8, 0);
  lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_AUTO);

  bool has_title = (title && title[0]);
  if (back || has_title) {
    lv_obj_t *hd = lv_obj_create(s);
    lv_obj_set_width(hd, LV_PCT(100));
    lv_obj_set_height(hd, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hd, 0, 0);
    lv_obj_set_style_pad_all(hd, 0, 0);
    lv_obj_clear_flag(hd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (back) {
      lv_obj_t *b = lv_btn_create(hd);
      lv_obj_set_style_bg_color(b, COL_CARD, 0);
      lv_obj_set_height(b, 38);
      lv_obj_set_style_pad_hor(b, 14, 0);
      lv_obj_set_style_radius(b, 9, 0);
      lv_obj_add_event_cb(b, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)PAGE_HOME);
      lv_obj_t *bl = lv_label_create(b);
      lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
      lv_obj_set_style_text_color(bl, COL_TEXT, 0);
      lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
      lv_obj_center(bl);
    }
    if (has_title) {
      lv_obj_t *t = lv_label_create(hd);
      lv_label_set_text(t, title);
      lv_obj_set_style_text_color(t, COL_TEXT, 0);
      lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
      lv_obj_set_style_pad_left(t, back ? 10 : 0, 0);
    }
  }
  return s;
}

// Prominent "TODAY P&L" headline card — used on top of every detail page.
static void today_hero(lv_obj_t *parent, double day) {
  lv_obj_t *h = card(parent);
  lv_obj_set_style_pad_all(h, 12, 0);
  info_label(h, "TODAY P&L", COL_MUTED);
  char b[40]; fmtPnL(b, sizeof(b), day);
  lv_obj_t *big = lv_label_create(h);
  lv_label_set_text(big, b);
  lv_obj_set_style_text_color(big, pnlColor(day), 0);
  lv_obj_set_style_text_font(big, &lv_font_montserrat_28, 0);
}

// Compact, MUTED menu button for the home row (rarely tapped → not prominent).
static lv_obj_t *menu_btn(lv_obj_t *parent, const char *text, int target) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_height(b, 34);
  lv_obj_set_style_bg_color(b, COL_CARD, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(b, 12, 0);
  lv_obj_set_style_radius(b, 9, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)target);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, COL_MUTED, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_center(l);
  return b;
}

// Full-width list row for the Menu picker page: label left, chevron right.
static void menu_row(lv_obj_t *parent, const char *text, int target) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_width(b, LV_PCT(100));
  lv_obj_set_height(b, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(b, COL_CARD, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_pad_all(b, 14, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(b, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)target);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, COL_TEXT, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  lv_obj_t *ch = lv_label_create(b);
  lv_label_set_text(ch, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(ch, COL_MUTED, 0);
}

/* ── HOME (the focus) ────────────────────────────────────────────────────── */
// Built ONCE; refreshes only update label texts (build-once = no flicker/churn).
// Dense summary: a big TODAY P&L headline with the open-positions count, then
// four money tiles (Total / Open now / All-time / Earn) and a single small Menu
// button. Every $ value sits in a 2-across tile, wide enough that amounts into
// the hundred-thousands never clip.
static void build_home() {
  home_screen = lv_obj_create(NULL);
  lv_obj_t *s = home_screen;
  lv_obj_set_style_bg_color(s, COL_BG, 0);
  lv_obj_set_style_pad_all(s, 6, 0);
  lv_obj_set_style_pad_row(s, 6, 0);
  lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);   // everything fits → no scroll

  // Hero — TODAY P&L (everything) + open-positions count. The headline.
  lv_obj_t *hero = card(s);
  lv_obj_set_flex_grow(hero, 1);
  lv_obj_set_style_pad_all(hero, 10, 0);
  lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  info_label(hero, "TODAY P&L  (all)", COL_MUTED);
  hl_today = lv_label_create(hero);
  lv_label_set_text(hl_today, "--");
  lv_obj_set_style_text_font(hl_today, &lv_font_montserrat_28, 0);
  hl_pos = lv_label_create(hero);                 // "N positions open"
  lv_label_set_text(hl_pos, "");
  lv_obj_set_style_text_color(hl_pos, COL_MUTED, 0);
  lv_obj_set_style_text_font(hl_pos, &lv_font_montserrat_14, 0);

  // Money figures — all in 2-across tiles (wide enough for big amounts @20pt).
  lv_obj_t *rA = hrow(s);
  hl_total = vtile(rA, "TOTAL BALANCE", &lv_font_montserrat_20);
  hl_open  = vtile(rA, "OPEN P&L (now)", &lv_font_montserrat_20);
  lv_obj_t *rB = hrow(s);
  hl_all  = vtile(rB, "ALL-TIME P&L", &lv_font_montserrat_20);
  hl_earn = vtile(rB, "EARN", &lv_font_montserrat_20);

  // Footer status (grows) + a single, small Menu button (opens the picker page).
  lv_obj_t *rC = hrow(s);
  hl_footer = lv_label_create(rC);
  lv_label_set_text(hl_footer, "");
  lv_obj_set_flex_grow(hl_footer, 1);
  lv_obj_set_style_text_color(hl_footer, COL_MUTED, 0);
  lv_obj_set_style_text_font(hl_footer, &lv_font_montserrat_12, 0);
  lv_obj_t *mb = menu_btn(rC, LV_SYMBOL_LIST "  Menu", PAGE_MENU);
  lv_obj_set_height(mb, 34);
}

static void update_home() {
  JsonDocument doc;
  bool ok = lastPayload.length() && !deserializeJson(doc, lastPayload);
  bool stale = ok ? (doc["stale"] | true) : true;
  double bal = ok ? (double)(doc["bal"] | 0.0) : 0.0;
  double all = ok ? (double)(doc["all"] | 0.0) : 0.0;
  double day = ok ? (double)(doc["day"] | 0.0) : 0.0;
  double open = ok ? (double)(doc["open"] | 0.0) : 0.0;
  int npos = ok ? (int)(doc["npos"] | 0) : 0;
  double earn = ok ? (double)(doc["earn"] | 0.0) : 0.0;
  const char *upd = ok ? (const char *)(doc["upd"] | "--:--") : "--:--";

  char b[48];
  fmtPnL(b, sizeof(b), day);
  lv_label_set_text(hl_today, b);
  lv_obj_set_style_text_color(hl_today, pnlColor(day), 0);
  char nb[28]; snprintf(nb, sizeof(nb), "%d position%s open", npos, npos == 1 ? "" : "s");
  lv_label_set_text(hl_pos, nb);
  kv_set_usd(hl_total, bal);
  kv_set_pnl(hl_open, open);
  kv_set_pnl(hl_all, all);
  kv_set_usd(hl_earn, earn);

  char f[80];
  snprintf(f, sizeof(f), "%s%s  ·  heap %uKB", stale ? "stale " : "updated ", upd,
           (unsigned)(ESP.getFreeHeap() / 1024));
  lv_label_set_text(hl_footer, f);
}

/* ── MENU PICKER ─────────────────────────────────────────────────────────── */
// Single Menu button on home opens this list; each row drills into a detail page.
static void show_menu() {
  current_page = PAGE_MENU;
  lv_obj_t *s = new_page("Menu", true);
  JsonDocument doc;
  bool ok = lastPayload.length() && !deserializeJson(doc, lastPayload);
  bool eon  = ok ? (bool)(doc["elite"]["on"] | false) : false;
  double earn = ok ? (double)(doc["earn"] | 0.0) : 0.0;

  menu_row(s, "Copy Traders", PAGE_TRADERS);
  if (eon) menu_row(s, "Elite Portfolio", PAGE_ELITE);
  menu_row(s, "Open Positions", PAGE_POSITIONS);
  menu_row(s, "Trade History", PAGE_HISTORY);
  if (earn > 0.005) menu_row(s, "Earn", PAGE_EARN);
  show_sub(s);
}

static void show_home() {
  current_page = PAGE_HOME;
  if (!home_screen) build_home();
  update_home();
  lv_obj_t *prev = lv_scr_act();
  if (prev != home_screen) {              // arriving from a sub-page (or boot screen)
    lv_scr_load(home_screen);
    if (prev) lv_obj_del(prev);           // free the sub-page; home itself persists
  }
}

/* ── TRADERS ─────────────────────────────────────────────────────────────── */
static void show_traders() {
  current_page = PAGE_TRADERS;
  lv_obj_t *s = new_page("Copy Traders", true);
  JsonDocument doc;
  bool ok = lastPayload.length() && !deserializeJson(doc, lastPayload);
  JsonArray traders = ok ? doc["traders"].as<JsonArray>() : JsonArray();
  if (!ok || traders.size() == 0) { info_label(s, "No active traders", COL_MUTED); show_sub(s); return; }

  int idx = 0;
  for (JsonObject tr : traders) {
    const char *name = tr["n"] | "?";
    double bal = tr["bal"] | 0.0, day = tr["day"] | 0.0, all = tr["all"] | 0.0;
    lv_obj_t *c = card(s);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);    // tap → trader detail page
    lv_obj_add_event_cb(c, trader_event, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *nm = lv_label_create(c);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, COL_TEXT, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    kv_set_usd(kv_row(c, "Balance"), bal);
    kv_set_pnl(kv_row(c, "Today"), day);
    kv_set_pnl(kv_row(c, "All-time"), all);
    idx++;
  }
  show_sub(s);
}

/* ── TRADER DETAIL ───────────────────────────────────────────────────────── */
static void show_trader_detail() {
  current_page = PAGE_TRADER_DETAIL;
  char name[48] = "Trader";
  JsonDocument home;
  if (lastPayload.length() && !deserializeJson(home, lastPayload)) {
    JsonArray tr = home["traders"].as<JsonArray>();
    if (trader_detail_idx >= 0 && trader_detail_idx < (int)tr.size()) {
      const char *nm = tr[trader_detail_idx]["n"] | "Trader";
      strncpy(name, nm, sizeof(name) - 1);
    }
  }
  lv_obj_t *s = new_page(name, true);
  JsonDocument doc;
  String path = String("/api/esp32/trader?name=") + urlenc(name);
  if (!httpGetJson(path, doc) || !(doc["ok"] | false)) {
    info_label(s, "could not load", COL_AMBER); show_sub(s); return;
  }
  today_hero(s, doc["day"] | 0.0);

  lv_obj_t *c = card(s);
  kv_set_usd(kv_row(c, "Balance"), doc["bal"] | 0.0);
  kv_set_usd(kv_row(c, "Equity"), doc["eq"] | 0.0);
  kv_set_usd(kv_row(c, "Invested"), doc["inv"] | 0.0);
  if (!doc["roi"].isNull()) kv_pct(kv_row(c, "ROI"), doc["roi"] | 0.0);
  kv_set_pnl(kv_row(c, "Open P&L"), doc["open"] | 0.0);

  lv_obj_t *c2 = card(s);
  kv_set_pnl(kv_row(c2, "All-time (net)"), doc["all"] | 0.0);
  kv_set_pnl(kv_row(c2, "Gross P&L"), doc["gall"] | 0.0);
  kv_set_pnl(kv_row(c2, "Profit share paid"), -(double)(doc["sh"] | 0.0));
  if (!doc["sr"].isNull()) kv_pct(kv_row(c2, "Share ratio"), doc["sr"] | 0.0);

  lv_obj_t *c3 = card(s);
  char b[24];
  snprintf(b, sizeof(b), "%d", (int)(doc["pos"] | 0));
  lv_label_set_text(kv_row(c3, "Open positions"), b);
  if (!doc["fd"].isNull()) { snprintf(b, sizeof(b), "%d days", (int)(doc["fd"] | 0)); lv_label_set_text(kv_row(c3, "Following"), b); }
  if (!doc["ml"].isNull()) { snprintf(b, sizeof(b), "%.0f%%", (double)(doc["ml"] | 0.0)); lv_label_set_text(kv_row(c3, "Margin level"), b); }
  if (!doc["start"].isNull()) lv_label_set_text(kv_row(c3, "Started"), doc["start"] | "");
  show_sub(s);
}

/* ── ELITE ───────────────────────────────────────────────────────────────── */
static void show_elite() {
  current_page = PAGE_ELITE;
  lv_obj_t *s = new_page("Elite Portfolio", true);
  JsonDocument doc;
  bool ok = lastPayload.length() && !deserializeJson(doc, lastPayload);
  JsonObject el = ok ? doc["elite"].as<JsonObject>() : JsonObject();
  if (!(el["on"] | false)) { info_label(s, "Not an elite trader", COL_MUTED); show_sub(s); return; }

  today_hero(s, el["day"] | 0.0);

  lv_obj_t *c = card(s);
  kv_set_usd(kv_row(c, "Balance"), el["bal"] | 0.0);
  kv_set_pnl(kv_row(c, "Open P&L"), el["open"] | 0.0);
  kv_set_pnl(kv_row(c, "All-time"), el["all"] | 0.0);
  if (!el["roi"].isNull()) kv_pct(kv_row(c, "ROI"), el["roi"] | 0.0);

  lv_obj_t *ci = card(s);
  kv_set_usd(kv_row(ci, "Profit shared (earned)"), el["ps"] | 0.0);
  kv_set_pnl(kv_row(ci, "Copiers P&L"), el["cp"] | 0.0);
  kv_set_usd(kv_row(ci, "AUM"), el["aum"] | 0.0);
  char b[16];
  snprintf(b, sizeof(b), "%d", (int)(el["fans"] | 0));
  lv_label_set_text(kv_row(ci, "Followers"), b);
  snprintf(b, sizeof(b), "%d", (int)(el["pos"] | 0));
  lv_label_set_text(kv_row(ci, "Open positions"), b);

  section_label(s, "OPEN POSITIONS");
  JsonDocument ed;
  if (httpGetJson("/api/elite", ed)) {
    JsonArray ps = ed["positions"].as<JsonArray>();
    if (ps.size() == 0) info_label(s, "none", COL_MUTED);
    for (JsonObject p : ps) {
      lv_obj_t *pc = card(s);
      const char *sym = p["symbol"] | "?";
      const char *side = (strcmp((const char *)(p["side"] | ""), "short") == 0) ? "SHORT" : "LONG";
      char hdr[40]; snprintf(hdr, sizeof(hdr), "%s  %s", sym, side);
      lv_obj_t *v = kv_row(pc, hdr);
      kv_set_pnl(v, p["unrealized_pnl"] | 0.0);
    }
  } else {
    info_label(s, "(could not load)", COL_AMBER);
  }
  show_sub(s);
}

/* ── OPEN POSITIONS ──────────────────────────────────────────────────────── */
static void show_positions() {
  current_page = PAGE_POSITIONS;
  lv_obj_t *s = new_page("Open Positions", true);
  JsonDocument doc;
  if (!httpGetJson("/api/esp32/positions", doc)) { info_label(s, "could not load", COL_AMBER); show_sub(s); return; }
  JsonArray ps = doc["positions"].as<JsonArray>();
  if (ps.size() == 0) { info_label(s, "No open positions", COL_MUTED); show_sub(s); return; }
  for (JsonObject p : ps) {
    const char *sym = p["s"] | "?";
    bool sh = strcmp((const char *)(p["d"] | "L"), "S") == 0;
    double sz = p["sz"] | 0.0, e = p["e"] | 0.0, u = p["u"] | 0.0;
    const char *src = p["src"] | "";
    lv_obj_t *c = card(s);
    char hdr[48]; snprintf(hdr, sizeof(hdr), "%s  %s", sym, sh ? "SHORT" : "LONG");
    lv_obj_t *top = kv_row(c, hdr);
    kv_set_pnl(top, u);
    char det[64]; snprintf(det, sizeof(det), "size %.4g  @ %.2f  [%s]", sz, e, src);
    info_label(c, det, COL_MUTED);
  }
  show_sub(s);
}

/* ── TRADE HISTORY ───────────────────────────────────────────────────────── */
static void show_history() {
  current_page = PAGE_HISTORY;
  lv_obj_t *s = new_page("Trade History", true);
  JsonDocument doc;
  if (!httpGetJson("/api/esp32/history?n=30", doc)) { info_label(s, "could not load", COL_AMBER); show_sub(s); return; }
  JsonArray ts_ = doc["trades"].as<JsonArray>();
  if (ts_.size() == 0) { info_label(s, "No closed trades yet", COL_MUTED); show_sub(s); return; }
  lv_obj_t *c = card(s);
  for (JsonObject t : ts_) {
    const char *when = t["t"] | "";
    const char *sym = t["s"] | "?";
    bool sh = strcmp((const char *)(t["d"] | "L"), "S") == 0;
    double pnl = t["p"] | 0.0;
    char lbl[48]; snprintf(lbl, sizeof(lbl), "%s %s %s", when, sym, sh ? "S" : "L");
    kv_set_pnl(kv_row(c, lbl), pnl);
  }
  show_sub(s);
}

/* ── EARN ────────────────────────────────────────────────────────────────── */
static void show_earn() {
  current_page = PAGE_EARN;
  lv_obj_t *s = new_page("Earn", true);
  JsonDocument doc;
  if (!httpGetJson("/api/earn", doc)) { info_label(s, "could not load", COL_AMBER); show_sub(s); return; }
  lv_obj_t *c = card(s);
  kv_set_usd(kv_row(c, "Total balance"), doc["total"] | 0.0);
  if (!doc["interest_24h"].isNull())
    kv_set_usd(kv_row(c, "Interest 24h"), doc["interest_24h"] | 0.0);
  if (!doc["total_interest"].isNull())
    kv_set_usd(kv_row(c, "Interest total"), doc["total_interest"] | 0.0);

  JsonArray items = doc["items"].as<JsonArray>();
  if (items.size() > 0) {
    lv_obj_t *ic = card(s);
    info_label(ic, "HOLDINGS", COL_MUTED);
    for (JsonObject it : items) {
      const char *coin = it["coin"] | "?";
      kv_set_usd(kv_row(ic, coin), it["amount"] | 0.0);
    }
  }
  show_sub(s);
}

static void navigate(int page) {
  switch (page) {
    case PAGE_MENU:      show_menu();      break;
    case PAGE_TRADERS:   show_traders();   break;
    case PAGE_ELITE:     show_elite();     break;
    case PAGE_POSITIONS: show_positions(); break;
    case PAGE_HISTORY:   show_history();   break;
    case PAGE_EARN:      show_earn();      break;
    case PAGE_TRADER_DETAIL: show_trader_detail(); break;
    default:             show_home();      break;
  }
}

/* ── Arduino entry points ────────────────────────────────────────────────── */
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

void setup() {
  Serial.begin(115200);
  Serial.println("\nBitget ESP32 Dashboard (LANDSCAPE) booting...");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);             // landscape (match tft)

  tft.begin();
  tft.setRotation(1);            // landscape: 320 wide x 240 tall
  tft.initDMA();                 // enable SPI DMA used by disp_flush()
  tft.fillScreen(TFT_BLACK);

  s_client.setInsecure();        // skip cert validation; client persists/reuses

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, SCR_W * 30);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCR_W; disp_drv.ver_res = SCR_H;
  disp_drv.flush_cb = disp_flush; disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);
  make_touch_dot();

  wifi_connect();
  httpGet("/api/esp32", lastPayload);   // prime the cache
  show_home();
  last_fetch = millis();
}

void loop() {
  lv_timer_handler();
  delay(5);

  if (pending_nav >= 0) {
    int p = pending_nav;
    pending_nav = -1;
    navigate(p);
  }

  uint32_t now = millis();
  if (now - last_fetch >= FETCH_INTERVAL_MS) {
    last_fetch = now;
    if (WiFi.status() != WL_CONNECTED) wifi_connect();
    if (httpGet("/api/esp32", lastPayload) && current_page == PAGE_HOME) {
      show_home();   // refresh the summary in place
    }
  }
}
