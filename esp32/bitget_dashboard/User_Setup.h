// ============================================================================
//  TFT_eSPI User_Setup.h  —  CYD "Cheap Yellow Display" ESP32-2432S028R
//  ILI9341 320x240 SPI display.
//
//  HOW TO USE:
//    Replace the file  <Arduino libraries>/TFT_eSPI/User_Setup.h  with this one
//    (back up the original first). The TFT_eSPI library reads its config from
//    that location, NOT from your sketch folder.
//
//  NOTE: The CYD's touch controller (XPT2046) is on a SEPARATE SPI bus, so the
//  TFT_eSPI built-in touch is intentionally NOT enabled here — the sketch drives
//  touch with the XPT2046_Touchscreen library instead.
// ============================================================================

#define USER_SETUP_INFO "CYD ESP32-2432S028R ILI9341"

// ── Driver ──────────────────────────────────────────────────────────────────
#define ILI9341_2_DRIVER          // CYD panels need the "_2" variant of the driver

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// Most CYD panels use BGR colour order. If reds/blues look swapped, comment this.
#define TFT_RGB_ORDER TFT_BGR

// ── Pins (display SPI = HSPI on the CYD) ─────────────────────────────────────
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1               // RST tied to the ESP32 EN line

#define TFT_BL   21               // backlight control pin
#define TFT_BACKLIGHT_ON HIGH

// ── Fonts (LVGL renders its own, but keep these for TFT_eSPI internals) ──────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ── SPI speed ────────────────────────────────────────────────────────────────
#define SPI_FREQUENCY        55000000
#define SPI_READ_FREQUENCY   20000000
