// ============================================================================
//  secrets.example.h  —  TEMPLATE (this file IS committed to git)
//
//  Setup (one time):
//    1. Copy this file to "secrets.h" in the same folder.
//    2. Fill in your WiFi name, WiFi password, and backend URL below.
//    3. secrets.h is git-ignored, so your private values never get pushed.
//
//  Notes:
//    - ESP32 only joins 2.4 GHz WiFi (not 5 GHz).
//    - SERVER_URL must have NO trailing slash.
// ============================================================================
#ifndef SECRETS_H
#define SECRETS_H

static const char *WIFI_SSID  = "YOUR_WIFI_SSID";
static const char *WIFI_PASS  = "YOUR_WIFI_PASSWORD";
static const char *SERVER_URL = "https://YOUR-SERVICE-NAME.up.railway.app";  // no trailing slash

#endif // SECRETS_H
