// ============================================================================
//  lv_conf.h  —  minimal LVGL 8.3 config for the Bitget ESP32 dashboard (CYD)
//
//  HOW TO USE:
//    Place this file ONE LEVEL ABOVE the lvgl library folder, i.e.:
//        <Arduino>/libraries/lv_conf.h
//        <Arduino>/libraries/lvgl/
//    LVGL finds it because LV_CONF_INCLUDE_SIMPLE is the default include mode.
//
//  Only the options the sketch relies on are overridden here; every other
//  option falls back to LVGL's built-in default (see lv_conf_internal.h).
// ============================================================================
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// ── Colour: 16-bit. SWAP must be 1 because the flush callback now uses
//    tft.pushPixelsDMA(), which does NOT byte-swap (unlike the old
//    pushColors(...,true)). LVGL byte-swaps here instead, exactly once.
//    (If reds/blues invert, this flag and the flush method are out of sync.)
#define LV_COLOR_DEPTH      16
#define LV_COLOR_16_SWAP    1

// ── Memory for LVGL objects (portrait page builds many cards/rows) ──────────
#define LV_MEM_SIZE         (64U * 1024U)

// ── Tick source: use Arduino millis() so we don't need lv_tick_inc() ────────
#define LV_TICK_CUSTOM              1
#define LV_TICK_CUSTOM_INCLUDE      "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// ── Default refresh / input read periods ────────────────────────────────────
#define LV_DISP_DEF_REFR_PERIOD    20
#define LV_INDEV_DEF_READ_PERIOD   20

// ── Fonts used by the sketch ────────────────────────────────────────────────
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

// ── Widgets the dashboard uses (most default to 1 anyway) ───────────────────
#define LV_USE_LED          1
#define LV_USE_TABVIEW      1
#define LV_USE_LABEL        1

#endif // LV_CONF_H
