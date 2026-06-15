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

// MQTT Configuration (Optional: Leave URI empty to disable MQTT)
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883" // e.g. "mqtt://user:pass@192.168.1.100:1883" or "mqtt://192.168.1.100"
#define MQTT_USERNAME   ""                          // Leave empty if not needed
#define MQTT_PASSWORD   ""                          // Leave empty if not needed
#define MQTT_TOPIC_LOGS "esp32_printer/logs"        // Topic for continuous system logs
#define MQTT_TOPIC_JOBS "esp32_printer/jobs"        // Topic for job audit JSON events
