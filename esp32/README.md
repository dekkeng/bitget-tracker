# ESP32 LVGL Dashboard — Bitget Tracker

A standalone home/desk dashboard for the Bitget copy-trading tracker, running on a
**CYD "Cheap Yellow Display" — ESP32-2432S028R** (ESP32 + ILI9341 320×240 SPI TFT +
XPT2046 resistive touch, the board that ships with a touch pen + Type-C + TF slot).

The device **never talks to Bitget directly**. It does one HTTPS `GET` to this
project's backend every 30 s and renders the result with LVGL:

```
ESP32  ──GET /api/esp32──▶  FastAPI backend  ──(already scraped)──▶  Bitget
```

## What it shows

A **portrait (240×320), single scrolling page** that combines every income source
in one view — drag up/down with the touch pen to scroll:

- **Header** — "BITGET", status dot (green = fresh, amber = stale), data time.
- **Hero** — grand TOTAL BALANCE (everything: copy + elite + earn) and all-time P&L.
- **Quick stats** — Today P&L, Open P&L, open Positions (green/red).
- **Income sources** — balance broken down: Copy trading, Elite portfolio, Earn,
  Invested.
- **Elite portfolio** — balance, today / open / all-time P&L, AUM, followers, open
  positions (hidden if you're not an elite trader).
- **Earn** — earn balance (hidden if zero).
- **Copy traders** — one card per active trader: name (★ = open position), balance,
  today's and all-time P&L.
- **Footer** — WiFi signal, free heap, last fetch status.

Display is portrait via `tft.setRotation(0)`. To flip 180°, change it to `2` (and
set `ts.setRotation(2)` to match).

## Backend endpoint

This sketch consumes `GET {SERVER_URL}/api/esp32`, a compact flat JSON added to
`main.py` specifically for embedded clients:

```json
{
  "ok": true, "stale": false, "upd": "21:30",
  "bal": 1234.56, "inv": 1000.00, "day": 12.34,
  "open": -5.67, "npos": 1, "all": 234.56, "earn": 50.00,
  "traders": [
    {"n":"DKTrading","bal":1184.56,"day":12.34,"all":234.56,"open":-5.67,"pos":1}
  ],
  "elite": {"on":true,"bal":5000.0,"all":820.5,"day":33.2,"open":40.0,"pos":1,"aum":125000.0,"fans":87}
}
```

Test it from a PC first:  `curl https://YOUR-SERVICE-NAME.onrender.com/api/esp32`

## Required libraries (Arduino IDE → Library Manager)

| Library | Version | Author |
|---|---|---|
| `lvgl` | **8.3.x** | LVGL |
| `TFT_eSPI` | latest | Bodmer |
| `XPT2046_Touchscreen` | latest | Paul Stoffregen |
| `ArduinoJson` | **7.x** | Benoit Blanchon |

Also install the **ESP32 board package** (Espressif Systems) via Boards Manager.

> ⚠️ LVGL 9.x changed the API — this sketch targets **8.3.x**. In Library Manager
> pick a `8.3.*` version, not the newest.

## One-time setup

1. **TFT_eSPI config** — copy `bitget_dashboard/User_Setup.h` over the library's own
   config (back the original up first):
   ```
   <Arduino>/libraries/TFT_eSPI/User_Setup.h   ← replace with our file
   ```

2. **LVGL config** — copy `bitget_dashboard/lv_conf.h` to **one level above** the
   lvgl folder:
   ```
   <Arduino>/libraries/lv_conf.h               ← our file
   <Arduino>/libraries/lvgl/
   ```
   (`<Arduino>` is usually `Documents/Arduino` on Windows.)

3. **Credentials** — open `bitget_dashboard/bitget_dashboard.ino` and edit:
   ```c
   WIFI_SSID, WIFI_PASS
   SERVER_URL   // e.g. "https://bitget-tracker-xxxx.onrender.com"  (no trailing slash)
   ```

4. **Board settings** in Arduino IDE:
   - Board: **ESP32 Dev Module**
   - Upload Speed: 921600
   - Flash Size: 4MB
   - Partition Scheme: default (Huge App is fine too)

5. Upload. Open Serial Monitor @ 115200 to watch WiFi + fetch logs.

## Touch calibration

Touch is used to **scroll** the page. The default portrait calibration usually
works; if dragging/scrolling feels off or reversed:

1. Set `#define TOUCH_DEBUG 1` near the top of the `.ino`.
2. Upload, open Serial Monitor, and tap the four corners.
3. Note the raw `x`/`y` extremes and update:
   ```c
   static int TS_MINX, TS_MAXX, TS_MINY, TS_MAXY;
   ```
4. Set `TOUCH_DEBUG` back to `0`.

If the display colours look inverted (red↔blue), remove `#define TFT_RGB_ORDER TFT_BGR`
from `User_Setup.h`. If the image is upside-down, change `tft.setRotation(0)` in
`setup()` to `2` (and set `ts.setRotation(2)` to match).

## Pin reference (CYD ESP32-2432S028R)

| Function | Pin |
|---|---|
| TFT MISO / MOSI / SCLK | 12 / 13 / 14 |
| TFT CS / DC / RST | 15 / 2 / -1 |
| TFT Backlight | 21 |
| Touch CLK / CS / MOSI / MISO / IRQ | 25 / 33 / 32 / 39 / 36 |

Touch is on its own SPI bus, which is why TFT_eSPI's built-in touch is disabled and
the sketch uses the `XPT2046_Touchscreen` library directly.

## Troubleshooting

- **White/blank screen** → wrong driver. Confirm `User_Setup.h` was actually copied
  into the `TFT_eSPI` folder (the sketch folder copy is just a reference).
- **`lv_font_montserrat_28` undefined** → `lv_conf.h` isn't being picked up; verify it
  sits beside the `lvgl` folder, not inside it.
- **`HTTP -1` / `begin failed`** → `SERVER_URL` wrong, no internet, or the free Render
  instance is asleep (first request can take ~30 s to wake; it retries automatically).
- **`json error`** → hit the URL in a browser; make sure it returns the JSON above.
