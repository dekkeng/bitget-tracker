/* ============================================================================
 *  Bitget Tracker — ESP32 LVGL Dashboard
 *  Board : CYD "Cheap Yellow Display" ESP32-2432S028R
 *          ESP32 + ILI9341 320x240 SPI TFT + XPT2046 resistive touch
 *
 *  Pulls a compact JSON summary from the tracker backend (NOT from Bitget
 *  directly) and renders a multi-tab dashboard with LVGL.
 *
 *  Endpoint used:  GET  {SERVER_URL}/api/esp32
 *
 *  Libraries (install via Arduino Library Manager):
 *    - LVGL                 (8.3.x)        by kisvegabor / LVGL
 *    - TFT_eSPI             (latest)       by Bodmer
 *    - XPT2046_Touchscreen  (latest)       by Paul Stoffregen
 *    - ArduinoJson          (7.x)          by Benoit Blanchon
 *
 *  Setup steps — read esp32/README.md. In short:
 *    1. Copy esp32/bitget_dashboard/User_Setup.h over the one in the
 *       TFT_eSPI library folder (or point User_Setup_Select.h at it).
 *    2. Copy esp32/bitget_dashboard/lv_conf.h next to the lvgl library
 *       folder (i.e. .../libraries/lv_conf.h) and keep LV_CONF_INCLUDE_SIMPLE.
 *    3. Fill in WIFI_SSID / WIFI_PASS / SERVER_URL below.
 *    4. Board: "ESP32 Dev Module", upload speed 921600.
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

// Your deployed backend, no trailing slash. The sketch appends /api/esp32.
static const char *SERVER_URL = "https://YOUR-SERVICE-NAME.onrender.com";

static const uint32_t FETCH_INTERVAL_MS = 30000;  // poll backend every 30 s

/* ── Touch (XPT2046) — own SPI bus on the CYD ────────────────────────────── */
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

// Touch calibration for landscape rotation (1). If touch is off / mirrored,
// run with TOUCH_DEBUG = 1, read the raw values from Serial, and adjust.
#define TOUCH_DEBUG   0
static int TS_MINX = 200,  TS_MAXX = 3700;
static int TS_MINY = 240,  TS_MAXY = 3800;

/* ── Display geometry (landscape) ────────────────────────────────────────── */
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

/* ── Globals ─────────────────────────────────────────────────────────────── */
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCR_W * 10];

// UI handles updated on each fetch
static lv_obj_t *lbl_balance;
static lv_obj_t *lbl_updated;
static lv_obj_t *led_status;           // small status dot (green/amber/red)
static lv_obj_t *val_today, *val_open, *val_alltime, *val_inv, *val_pos, *val_earn;
static lv_obj_t *traders_list;
static lv_obj_t *lbl_net_ssid, *lbl_net_ip, *lbl_net_rssi, *lbl_net_upd, *lbl_net_heap, *lbl_net_state;

static uint32_t last_fetch = 0;

/* ── Formatting helpers ──────────────────────────────────────────────────── */
static void fmtUSD(char *out, size_t n, double v) {
  // "$1,234.56" with thousands separators, no sign
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
    x = constrain(x, 0, SCR_W - 1);
    y = constrain(y, 0, SCR_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* ── UI construction ─────────────────────────────────────────────────────── */

// A labelled stat "card": small muted title on top, large coloured value below.
static lv_obj_t *make_stat_card(lv_obj_t *parent, const char *title, lv_obj_t **value_out) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(card, COL_CARD, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 6, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *t = lv_label_create(card);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, COL_MUTED, 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);

  lv_obj_t *v = lv_label_create(card);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_color(v, COL_TEXT, 0);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
  *value_out = v;
  return card;
}

static void build_overview(lv_obj_t *tab) {
  lv_obj_set_style_pad_all(tab, 6, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 6, 0);

  /* Header row: balance + status dot + time */
  lv_obj_t *header = lv_obj_create(tab);
  lv_obj_set_size(header, LV_PCT(100), 64);
  lv_obj_set_style_bg_color(header, COL_CARD, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 8, 0);
  lv_obj_set_style_pad_all(header, 8, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *cap = lv_label_create(header);
  lv_label_set_text(cap, "TOTAL BALANCE");
  lv_obj_set_style_text_color(cap, COL_MUTED, 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

  lbl_balance = lv_label_create(header);
  lv_label_set_text(lbl_balance, "$--");
  lv_obj_set_style_text_color(lbl_balance, COL_TEXT, 0);
  lv_obj_set_style_text_font(lbl_balance, &lv_font_montserrat_28, 0);
  lv_obj_align(lbl_balance, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  led_status = lv_led_create(header);
  lv_obj_set_size(led_status, 12, 12);
  lv_obj_align(led_status, LV_ALIGN_TOP_RIGHT, 0, 2);
  lv_led_set_color(led_status, COL_AMBER);
  lv_led_on(led_status);

  lbl_updated = lv_label_create(header);
  lv_label_set_text(lbl_updated, "--:--");
  lv_obj_set_style_text_color(lbl_updated, COL_MUTED, 0);
  lv_obj_set_style_text_font(lbl_updated, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_updated, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

  /* 2x3 grid of stat cards */
  static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

  lv_obj_t *grid = lv_obj_create(tab);
  lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 0, 0);
  lv_obj_set_flex_grow(grid, 1);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
  lv_obj_set_style_pad_column(grid, 6, 0);
  lv_obj_set_style_pad_row(grid, 6, 0);

  struct { const char *title; lv_obj_t **val; } cells[] = {
    {"TODAY",    &val_today},
    {"OPEN P&L", &val_open},
    {"ALL-TIME", &val_alltime},
    {"INVESTED", &val_inv},
    {"POS",      &val_pos},
    {"EARN",     &val_earn},
  };
  for (int i = 0; i < 6; i++) {
    lv_obj_t *card = make_stat_card(grid, cells[i].title, cells[i].val);
    lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                               LV_GRID_ALIGN_STRETCH, i / 3, 1);
  }
}

static void build_traders(lv_obj_t *tab) {
  lv_obj_set_style_pad_all(tab, 4, 0);
  traders_list = lv_obj_create(tab);
  lv_obj_set_size(traders_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(traders_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(traders_list, 0, 0);
  lv_obj_set_style_pad_all(traders_list, 0, 0);
  lv_obj_set_flex_flow(traders_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(traders_list, 6, 0);

  lv_obj_t *ph = lv_label_create(traders_list);
  lv_label_set_text(ph, "Loading traders...");
  lv_obj_set_style_text_color(ph, COL_MUTED, 0);
}

static void make_net_row(lv_obj_t *parent, const char *label, lv_obj_t **value_out) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 2, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *l = lv_label_create(row);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_color(l, COL_MUTED, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *v = lv_label_create(row);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_color(v, COL_TEXT, 0);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  *value_out = v;
}

static void build_status(lv_obj_t *tab) {
  lv_obj_set_style_pad_all(tab, 8, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 2, 0);
  make_net_row(tab, "WiFi",        &lbl_net_ssid);
  make_net_row(tab, "IP",          &lbl_net_ip);
  make_net_row(tab, "Signal",      &lbl_net_rssi);
  make_net_row(tab, "Data time",   &lbl_net_upd);
  make_net_row(tab, "Free heap",   &lbl_net_heap);
  make_net_row(tab, "Last fetch",  &lbl_net_state);
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);

  lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 32);
  lv_obj_set_style_bg_color(tv, COL_BG, 0);

  lv_obj_t *tb = lv_tabview_get_tab_btns(tv);
  lv_obj_set_style_bg_color(tb, COL_BG, 0);
  lv_obj_set_style_text_color(tb, COL_MUTED, 0);
  lv_obj_set_style_text_color(tb, COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tb, COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_font(tb, &lv_font_montserrat_12, 0);

  lv_obj_t *t1 = lv_tabview_add_tab(tv, "OVERVIEW");
  lv_obj_t *t2 = lv_tabview_add_tab(tv, "TRADERS");
  lv_obj_t *t3 = lv_tabview_add_tab(tv, "STATUS");
  lv_obj_set_style_bg_color(t1, COL_BG, 0);
  lv_obj_set_style_bg_color(t2, COL_BG, 0);
  lv_obj_set_style_bg_color(t3, COL_BG, 0);

  build_overview(t1);
  build_traders(t2);
  build_status(t3);
}

/* ── Apply fetched data to the UI ────────────────────────────────────────── */
static void set_pnl_label(lv_obj_t *lbl, double v) {
  char buf[40];
  fmtPnL(buf, sizeof(buf), v);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, pnlColor(v), 0);
}

static void update_traders(JsonArray traders) {
  lv_obj_clean(traders_list);
  if (traders.size() == 0) {
    lv_obj_t *ph = lv_label_create(traders_list);
    lv_label_set_text(ph, "No active traders");
    lv_obj_set_style_text_color(ph, COL_MUTED, 0);
    return;
  }
  for (JsonObject tr : traders) {
    const char *name = tr["n"] | "?";
    double bal  = tr["bal"]  | 0.0;
    double day  = tr["day"]  | 0.0;
    double all  = tr["all"]  | 0.0;
    int    pos  = tr["pos"]  | 0;

    lv_obj_t *card = lv_obj_create(traders_list);
    lv_obj_set_size(card, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(card);
    char nmbuf[40];
    snprintf(nmbuf, sizeof(nmbuf), "%s%s", name, pos > 0 ? " *" : "");
    lv_label_set_text(nm, nmbuf);
    lv_obj_set_style_text_color(nm, COL_TEXT, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 0);

    char balbuf[32];
    fmtUSD(balbuf, sizeof(balbuf), bal);
    lv_obj_t *bl = lv_label_create(card);
    lv_label_set_text(bl, balbuf);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_align(bl, LV_ALIGN_TOP_RIGHT, 0, 0);

    char line[64], dbuf[32], abuf[32];
    fmtPnL(dbuf, sizeof(dbuf), day);
    fmtPnL(abuf, sizeof(abuf), all);
    lv_obj_t *today = lv_label_create(card);
    snprintf(line, sizeof(line), "Today %s", dbuf);
    lv_label_set_text(today, line);
    lv_obj_set_style_text_color(today, pnlColor(day), 0);
    lv_obj_set_style_text_font(today, &lv_font_montserrat_12, 0);
    lv_obj_align(today, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *atime = lv_label_create(card);
    snprintf(line, sizeof(line), "All %s", abuf);
    lv_label_set_text(atime, line);
    lv_obj_set_style_text_color(atime, pnlColor(all), 0);
    lv_obj_set_style_text_font(atime, &lv_font_montserrat_12, 0);
    lv_obj_align(atime, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  }
}

static void apply_data(JsonDocument &doc) {
  bool stale = doc["stale"] | true;
  const char *upd = doc["upd"] | "--:--";
  double bal  = doc["bal"]  | 0.0;
  double inv  = doc["inv"]  | 0.0;
  double day  = doc["day"]  | 0.0;
  double open = doc["open"] | 0.0;
  double all  = doc["all"]  | 0.0;
  double earn = doc["earn"] | 0.0;
  int npos    = doc["npos"] | 0;

  char buf[40];
  fmtUSD(buf, sizeof(buf), bal);
  lv_label_set_text(lbl_balance, buf);

  lv_label_set_text(lbl_updated, upd);
  lv_led_set_color(led_status, stale ? COL_AMBER : COL_GREEN);

  set_pnl_label(val_today, day);
  set_pnl_label(val_open, open);
  set_pnl_label(val_alltime, all);

  fmtUSD(buf, sizeof(buf), inv);
  lv_label_set_text(val_inv, buf);
  fmtUSD(buf, sizeof(buf), earn);
  lv_label_set_text(val_earn, buf);
  snprintf(buf, sizeof(buf), "%d", npos);
  lv_label_set_text(val_pos, buf);

  update_traders(doc["traders"].as<JsonArray>());

  // Status tab data-time
  char dt[40];
  snprintf(dt, sizeof(dt), "%s%s", upd, stale ? " (stale)" : "");
  lv_label_set_text(lbl_net_upd, dt);
}

/* ── Networking ──────────────────────────────────────────────────────────── */
static void fetch_data() {
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(lbl_net_state, "no wifi");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation (Render TLS); fine for read-only data
  HTTPClient http;
  http.setTimeout(12000);

  String url = String(SERVER_URL) + "/api/esp32";
  bool ok = url.startsWith("https") ? http.begin(client, url) : http.begin(url);
  if (!ok) {
    lv_label_set_text(lbl_net_state, "begin failed");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    char s[32];
    snprintf(s, sizeof(s), "HTTP %d", code);
    lv_label_set_text(lbl_net_state, s);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    lv_label_set_text(lbl_net_state, "json error");
    return;
  }
  apply_data(doc);
  lv_label_set_text(lbl_net_state, "OK");
}

static void update_net_status() {
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text(lbl_net_ssid, WiFi.SSID().c_str());
    lv_label_set_text(lbl_net_ip, WiFi.localIP().toString().c_str());
    char r[16];
    snprintf(r, sizeof(r), "%d dBm", WiFi.RSSI());
    lv_label_set_text(lbl_net_rssi, r);
  } else {
    lv_label_set_text(lbl_net_ssid, "disconnected");
  }
  char h[24];
  snprintf(h, sizeof(h), "%u B", (unsigned)ESP.getFreeHeap());
  lv_label_set_text(lbl_net_heap, h);
}

static void wifi_connect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300);
    Serial.print(".");
    lv_timer_handler();  // keep UI alive while connecting
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ok" : " timeout");
}

/* ── Arduino entry points ────────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  Serial.println("\nBitget ESP32 Dashboard booting...");

  // Backlight on
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Touch on its own SPI bus
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  // Display
  tft.begin();
  tft.setRotation(1);   // landscape, 320x240
  tft.fillScreen(TFT_BLACK);

  // LVGL
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
  update_net_status();
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
    update_net_status();
    fetch_data();
  }
}
