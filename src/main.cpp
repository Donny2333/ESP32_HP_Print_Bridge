// =============================================================================
//  ESP32-S3-N16R8 USB Host Print Bridge for HP LaserJet M1136
//
//  Architecture:
//    [macOS HP driver]  --TCP:9100-->  [ESP32-S3]  --USB Bulk OUT-->  [M1136]
//
//  - On macOS, add the printer as:
//      System Settings -> Printers & Scanners -> Add Printer
//      IP tab, Protocol = "HP JetDirect - Socket"
//      Address = this device's IP (or esp32-printer.local), Port = 9100
//      Driver  = "HP LaserJet Professional M1132 MFP" (works for M1136).
//  - macOS rasterizes PDF -> ZJStream via the HP driver and pipes the bytes
//    over TCP:9100. We forward every byte 1:1 to the printer's USB Bulk OUT
//    endpoint.
//  - This sketch deliberately does NOT implement IPP/AirPrint - that path
//    cannot produce ZJStream. Use socket://9100 only.
//
//  Board: ESP32-S3-N16R8 (16MB flash + 8MB octal PSRAM, dual USB)
//   * USB-OTG port      -> printer cable
//   * USB-Serial-JTAG   -> log console / flashing
//
//  This file is the boot/wiring layer only: Wi-Fi, mDNS, buffer allocation,
//  task creation and the Arduino setup()/loop().  The subsystems live in:
//    bridge_config.h  - compile-time constants
//    bridge_state.*   - cross-module shared globals
//    usb_bridge.*     - USB Host (attach, control transfers, bulk OUT writer)
//    net_server.*     - raw TCP servers (:9100 print, :9101 telnet log)
//    http_status.*    - HTTP :80 status / jobs / tail / reset
//    job_log.* / log_sink.* - diagnostics ring buffers
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp_heap_caps.h"
#include "usb/usb_host.h"

#include "bridge_config.h"
#include "bridge_state.h"
#include "usb_bridge.h"
#include "net_server.h"
#include "http_status.h"
#include "log_sink.h"
#include "job_log.h"
#include "ota_update.h"
#include "version.h"
#include "mqtt_logger.h"

// -----------------------------------------------------------------------------
// Back-channel ring buffer: printer Bulk IN → this buffer → TCP client.
// Allocated in PSRAM.  Currently unused (Bulk IN disabled) but kept allocated
// so re-enabling the reader needs no boot-path change.  Owned by main.
// -----------------------------------------------------------------------------
static StreamBufferHandle_t s_backBuf        = nullptr;
static uint8_t             *s_backBufStorage = nullptr;
static StaticStreamBuffer_t s_backBufCb;

// Stream buffer storage (the handle s_stream itself lives in bridge_state).
static uint8_t              *s_streamStorage = nullptr;
static StaticStreamBuffer_t  s_streamCb;

// -----------------------------------------------------------------------------
// Wi-Fi / mDNS / USB host bring-up
// -----------------------------------------------------------------------------

static void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info);

static void startWifi()
{
  WiFi.onEvent(wifiEventHandler);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // better USB Host stability under WiFi traffic
  WiFi.begin(kSsid, kPassword);
  logTee("Connecting to Wi-Fi '%s'", kSsid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    logTee(".");
    delay(500);
    if (millis() - start > 30000)
    {
      logTeeln(" timeout, retrying...");
      WiFi.disconnect();
      delay(500);
      WiFi.begin(kSsid, kPassword);
      start = millis();
    }
  }
  logTeeNewline();
  logTee("Wi-Fi OK, IP=%s MAC=%s\n",
                WiFi.localIP().toString().c_str(), macToString().c_str());
}

static bool s_mdnsStarted = false;

static void startMdns()
{
  if (s_mdnsStarted)
  {
    MDNS.end();
    s_mdnsStarted = false;
  }

  if (!MDNS.begin(kMdnsHostname))
  {
    logTeeln("mDNS begin failed");
    return;
  }
  logTee("mDNS: %s.local\n", kMdnsHostname);

  s_mdnsStarted = true;

  // Advertise as a JetDirect / pdl-datastream printer.
  MDNS.addService("pdl-datastream", "tcp", kRawPort);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "ty",       kPrinterName);
  MDNS.addServiceTxt("pdl-datastream", "tcp", "product",  "(ESP32 USB Bridge)");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "note",     "USB bridge to HP M1136");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "rp",       "raw");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "txtvers",  "1");
  MDNS.addServiceTxt("pdl-datastream", "tcp", "qtotal",   "1");

  // Also advertise as a generic "printer" (LPR-style) for older clients.
  MDNS.addService("printer", "tcp", kRawPort);
  MDNS.addServiceTxt("printer", "tcp", "txtvers", "1");
  MDNS.addServiceTxt("printer", "tcp", "ty",      kPrinterName);
  MDNS.addServiceTxt("printer", "tcp", "rp",      "auto");

  // HTTP admin
  MDNS.addService("http", "tcp", 80);
}

static void wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      logTee("[wifi] got ip: %s\n", WiFi.localIP().toString().c_str());
      startMdns();
      mqttLoggerInit();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      logTeeln("[wifi] disconnected, waiting for reconnect...");
      break;
    default:
      break;
  }
}

static void startUsbHost()
{
  s_usb.lock = xSemaphoreCreateMutex();
  s_usbSubmitMux = xSemaphoreCreateMutex();

  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;
  esp_err_t err = usb_host_install(&cfg);
  if (err != ESP_OK)
  {
    logTee("usb_host_install err=0x%x\n", err);
    return;
  }
  logTeeln("usb_host installed");

  // Library task on core 0, priority 5
  xTaskCreatePinnedToCore(usbHostLibTask, "usb-lib", 4096, nullptr, 5, nullptr, 0);
  // Client task on core 0, priority 4
  xTaskCreatePinnedToCore(usbClientTask, "usb-cli", 4096, nullptr, 4, nullptr, 0);
  // Writer task on core 0, priority 4
  xTaskCreatePinnedToCore(usbWriterTask, "usb-out", 4096, nullptr, 4, nullptr, 0);
  // Reader task on core 0, priority 3 (lower than writer)
  // Reader task MUST be lower priority than usb-cli (4) so that the USB
  // client event loop can dispatch control-transfer callbacks during device
  // attach.  Same-priority round-robin on core 0 starves handle_events.
  xTaskCreatePinnedToCore(usbReaderTask, "usb-in",  4096, nullptr, 3, nullptr, 0);
  // Back-channel forwarder on core 1
  xTaskCreatePinnedToCore(backFwdTask, "back-fwd", 4096, nullptr, 3, nullptr, 1);
}

// Allocate the stream buffer storage in PSRAM if available.
static bool createStreamBuffer()
{
  // Prefer PSRAM (we have 8MB), fall back to internal SRAM.
  s_streamStorage = (uint8_t *)heap_caps_malloc(kStreamBufBytes + 1, MALLOC_CAP_SPIRAM);
  if (!s_streamStorage)
  {
    logTeeln("PSRAM alloc for stream buffer failed, trying internal heap");
    s_streamStorage = (uint8_t *)heap_caps_malloc(kStreamBufBytes + 1, MALLOC_CAP_8BIT);
  }
  if (!s_streamStorage) return false;

  s_stream = xStreamBufferCreateStatic(kStreamBufBytes, 1, s_streamStorage, &s_streamCb);
  if (!s_stream) return false;

  // Back-channel ring buffer for printer→host status forwarding
  s_backBufStorage = (uint8_t *)heap_caps_malloc(kBackBufBytes + 1, MALLOC_CAP_SPIRAM);
  if (!s_backBufStorage)
  {
    s_backBufStorage = (uint8_t *)heap_caps_malloc(kBackBufBytes + 1, MALLOC_CAP_8BIT);
  }
  if (s_backBufStorage)
  {
    s_backBuf = xStreamBufferCreateStatic(kBackBufBytes, 1, s_backBufStorage, &s_backBufCb);
    if (s_backBuf)
    {
      logTee("Back-channel buffer: %u bytes\n", (unsigned)kBackBufBytes);
    }
    else
    {
      logTeeln("Back-channel buffer create failed (disabled)");
    }
  }
  else
  {
    logTeeln("Back-channel buffer alloc failed (disabled)");
  }

  return true;
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  // Bring up the in-memory log ring as early as possible so we capture the
  // boot banner.  This installs an esp_log vprintf hook; subsequent
  // ESP_LOG / Serial.printf calls fan out to both USB-Serial-JTAG and the
  // ring (consumed by TCP :9101 and HTTP /tail).
  logSinkInit();
  jobLogInit();

  logTeeNewline();
  logTeeln("=== ESP32-S3-N16R8 USB Print Bridge ===");
  logTee("Firmware: %s (git %s, %s, built %s)\n",
         FW_VERSION, FW_GIT_REV, FW_GIT_DIRTY, FW_BUILD_TIME);
  logTee("PSRAM total: %u  free: %u\n",
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!createStreamBuffer())
  {
    logTeeln("stream buffer alloc failed!");
    while (true) delay(1000);
  }
  logTee("Stream buffer: %u bytes (PSRAM)\n", (unsigned)kStreamBufBytes);

  startWifi();
  startMdns();
  otaInit();

  webServer.on("/", handleRoot);
  webServer.on("/reset", handleReset);
  webServer.on("/jobs", handleJobs);
  webServer.on("/tail", handleTail);
  webServer.on("/tail.txt", handleTailTxt);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not Found"); });
  webServer.begin();

  // The :9100 raw print and :9101 telnet log listeners are now created
  // inside their own tasks via raw lwIP sockets - no global state needed.
  logTee("Will listen on TCP %u (HP JetDirect / AppSocket)\n", kRawPort);

  startUsbHost();

  // Raw 9100 server runs on core 1, lower priority than USB tasks
  xTaskCreatePinnedToCore(rawServerTask, "raw9100", 6144, nullptr, 3, nullptr, 1);

  // Telnet log streamer on core 1, low priority - never blocks producers.
  xTaskCreatePinnedToCore(tcpLogTask,    "log9101", 4096, nullptr, 2, nullptr, 1);
  logTeeln("Listening on TCP 9101 (telnet log: nc <ip> 9101)");
}

void loop()
{
  webServer.handleClient();
  otaHandle();
  delay(2);
}
