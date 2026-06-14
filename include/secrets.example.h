// Wi-Fi / mDNS credentials.  Edit and DO NOT commit (see .gitignore).
//
// Copy this file to `secrets.h` and fill in your network credentials:
//   cp include/secrets.example.h include/secrets.h
//
#pragma once

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

// mDNS hostname (esp32-printer.local)
#define MDNS_HOSTNAME   "esp32-printer"

// Friendly name advertised over mDNS / shown on the status page
#define PRINTER_FRIENDLY_NAME "ESP32 USB Bridge to HP M1136"

// ArduinoOTA password. Keep the real value only in include/secrets.h.
#define OTA_PASSWORD "change-me"
