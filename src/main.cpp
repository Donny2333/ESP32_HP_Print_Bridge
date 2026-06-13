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
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

// POSIX-style socket API exposed by lwIP - we bypass Arduino-ESP32's
// WiFiServer entirely for the raw print port and the telnet log port,
// because WiFiServer::accept()/available() in 3.x + IDF v4.4 (arduino-as-
// component) does not deliver successfully-handshaken clients in this build
// (verified by `nc -vz` reporting "succeeded!" while the userspace task
// never observes the client).
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "secrets.h"
#include "log_sink.h"
#include "job_log.h"

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

static const char    *kSsid           = WIFI_SSID;
static const char    *kPassword       = WIFI_PASSWORD;
static const char    *kMdnsHostname   = MDNS_HOSTNAME;
static const char    *kPrinterName    = PRINTER_FRIENDLY_NAME;
static const uint16_t kRawPort        = 9100;

// USB Printer Class identifiers (USB_CLASS_PRINTER == 0x07 already defined in
// usb/usb_types_ch9.h):
//   subclass 0x01 = printer
//   protocol 0x01 = unidirectional, 0x02 = bidirectional, 0x03 = IEEE 1284.4

// Class-specific requests (USB Printer Class spec)
static constexpr uint8_t  PRINTER_REQ_GET_DEVICE_ID    = 0x00;
static constexpr uint8_t  PRINTER_REQ_GET_PORT_STATUS  = 0x01;
static constexpr uint8_t  PRINTER_REQ_SOFT_RESET       = 0x02;

// bmRequestType helpers
static constexpr uint8_t  REQ_TYPE_CLASS_IFACE_IN  = 0xA1; // 1010 0001
static constexpr uint8_t  REQ_TYPE_CLASS_IFACE_OUT = 0x21; // 0010 0001

// Stream buffer used to pipe bytes from the TCP receiver task to the
// USB writer task.  Allocated in PSRAM (large) to absorb bursts.
static const size_t kStreamBufBytes = 256 * 1024;

// Per-USB-write chunk size.  M1136 USB descriptor reports wMaxPacketSize=64
// (Full Speed). We pack multiple packets into a single transfer.
static const size_t kUsbChunkBytes  = 1024;

// Bulk-IN read buffer
static const size_t kUsbInBufBytes  = 512;
static const uint32_t kDrainQuiescentMs = 1500;
static const uint32_t kJobCompleteQuietMs = 500;
static const uint32_t kFinWaitMs = 8000;
// How long to wait for the printer to become ready (USB attached) when TCP
// data already arrived but no device is present yet.
static const uint32_t kPrinterReadyWaitMs = 5000;

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

WebServer  webServer(80);
// rawServer / s_logServer no longer use Arduino WiFiServer; the listen
// sockets live inside the respective tasks.

static StreamBufferHandle_t s_stream = nullptr;
static uint8_t              *s_streamStorage = nullptr;
static StaticStreamBuffer_t s_streamCb;

// Statistics
static volatile uint32_t s_totalBytesIn  = 0;   // received over TCP
static volatile uint32_t s_totalBytesOut = 0;   // written to USB
static volatile uint32_t s_totalBytesBack= 0;   // read from USB IN
static volatile uint32_t s_totalBackToClient = 0; // bulk IN bytes forwarded back to TCP client (back-channel)
// Currently-active raw9100 client fd, or -1 if none.  Used by usbReaderTask
// so the bulk IN data the printer sends (PJL USTATUS replies, supplies-level
// info, etc.) can be forwarded back over TCP to the macOS CUPS backend that
// is waiting for it.  Without this, jobs hang for 60+ seconds because the
// HP M1130 PPD requests USTATUS via PJL and CUPS waits for the reply.
static volatile int      s_rawClientFd       = -1;
static volatile uint32_t s_jobsAccepted  = 0;
static volatile uint32_t s_jobsDropped   = 0;
static volatile uint32_t s_lastJobBytes  = 0;
static volatile bool     s_printerReady  = false;
static char              s_printerIdent[160] = "(no printer)";
static char              s_deviceId[256]     = "";   // IEEE 1284 device ID string
static volatile uint32_t s_lastRecvActivityMs = 0;
static volatile bool     s_jobComplete        = false;
static volatile uint32_t s_jobCompleteMs      = 0;
static volatile uint32_t s_jobGeneration      = 0;
// Timestamp of last successful USB Bulk OUT write.  Used by rawServerTask
// to detect "printing finished" (USB idle) and inject synthetic status.
static volatile uint32_t s_lastUsbWriteMs     = 0;
static volatile bool     s_syntheticInjected  = false;
// Consecutive USB bulk-OUT error counter.  Reset on each successful write.
// When this exceeds a threshold, rawServerTask should abandon the job.
static volatile uint32_t s_usbConsecutiveErrors = 0;
static const    uint32_t kMaxConsecutiveUsbErrors = 5;
// Port status sentinel: 0xFF means "we tried to read but the printer didn't
// answer (timeout / stall)" - this is common on hub-attached HP M1132/M1136
// printers that don't implement class-specific control requests reliably.
// The real-world Port Status byte (USB Printer Class 1.1 §5.4) only ever
// uses bits 3, 4, 5; 0xFF cannot occur as a genuine status.
static constexpr uint8_t kPortStatusUnknown = 0xFF;
static volatile uint8_t  s_portStatus        = kPortStatusUnknown;

// Back-channel ring buffer: printer Bulk IN → this buffer → TCP client.
// Allocated in PSRAM.  usbReaderTask writes; backFwdTask reads.
static const size_t     kBackBufBytes = 16 * 1024;
static StreamBufferHandle_t s_backBuf        = nullptr;
static uint8_t             *s_backBufStorage = nullptr;
static StaticStreamBuffer_t s_backBufCb;

// Pre-allocated persistent USB OUT transfer (avoid per-chunk alloc/free)
static usb_transfer_t  *s_outXfer = nullptr;
static SemaphoreHandle_t s_outDone = nullptr;

// Mutex to serialise Bulk OUT and Bulk IN submissions on the same USB
// host client.  ESP-IDF v4.4 USB host driver is not fully re-entrant
// when two tasks submit transfers concurrently on the same device.
static SemaphoreHandle_t s_usbSubmitMux = nullptr;

// Bulk IN endpoint error streak counter for auto-recovery
static volatile uint32_t s_inErrorStreak = 0;
static const    uint32_t kInErrorRecoveryThreshold = 10;

// Macro to enable/disable synthetic PJL Ready injection (0=off)
// USB state shared between USB tasks
struct UsbState
{
  SemaphoreHandle_t        lock;
  usb_host_client_handle_t client;
  usb_device_handle_t      device;
  uint8_t                  devAddr;
  uint8_t                  ifNumber;
  uint8_t                  epOut;        // address of bulk OUT
  uint8_t                  epIn;         // address of bulk IN (0 if absent)
  uint16_t                 epOutMps;     // wMaxPacketSize for OUT
  uint16_t                 epInMps;      // wMaxPacketSize for IN
  bool                     claimed;
};

static UsbState s_usb = {};

// Forward declarations
static void usbHostLibTask(void *arg);
static void usbClientTask(void *arg);
static void usbWriterTask(void *arg);
static void usbReaderTask(void *arg);
static void backFwdTask(void *arg);
static void rawServerTask(void *arg);
static bool usbTryAttachDevice(uint8_t devAddr);
static void usbDetach();
static esp_err_t usbCtrlGetDeviceId(char *out, size_t outSz);
static esp_err_t usbCtrlGetPortStatus(uint8_t *status);
static esp_err_t usbCtrlSoftReset();

// -----------------------------------------------------------------------------
// Tiny helpers
// -----------------------------------------------------------------------------

static String macToString()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

static const char *epDirStr(uint8_t addr) { return (addr & 0x80) ? "IN" : "OUT"; }

// Wait until printer is attached (or timeout).  Returns true if ready.
static bool waitForPrinter(uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (!s_printerReady)
  {
    if (millis() - start >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

static bool bufferContains(const uint8_t *buf, size_t len, const char *needle)
{
  size_t n = strlen(needle);
  if (n == 0 || len < n) return false;
  const uint8_t *pat = reinterpret_cast<const uint8_t *>(needle);
  for (size_t i = 0; i + n <= len; i++)
  {
    if (memcmp(buf + i, pat, n) == 0) return true;
  }
  return false;
}

static bool scanForJobEnd(const uint8_t *data, size_t len, uint32_t jobGen)
{
  if (!data || len == 0) return false;

  static uint8_t  tail[64];
  static size_t   tailLen = 0;
  static uint32_t tailGen = 0;

  if (jobGen != tailGen)
  {
    tailLen = 0;
    tailGen = jobGen;
  }

  uint8_t window[sizeof(tail) + kUsbInBufBytes];
  size_t winLen = 0;

  if (tailLen > 0)
  {
    memcpy(window, tail, tailLen);
    winLen = tailLen;
  }

  if (len > 0)
  {
    size_t copyLen = len;
    if (winLen + copyLen > sizeof(window))
    {
      copyLen = sizeof(window) - winLen;
    }
    memcpy(window + winLen, data, copyLen);
    winLen += copyLen;
  }

  static const char *kNeedles[] = {
    "@PJL USTATUS JOB\r\nEND",
    "@PJL EOJ",
    "DISPLAY=\"JOB COMPLETE\"",
    "CODE=8010",
    "CODE=8011"
  };

  bool found = false;
  for (size_t i = 0; i < (sizeof(kNeedles) / sizeof(kNeedles[0])); i++)
  {
    if (bufferContains(window, winLen, kNeedles[i]))
    {
      found = true;
      break;
    }
  }

  const size_t keep = 32;
  if (winLen > keep)
  {
    tailLen = keep;
    memcpy(tail, window + winLen - tailLen, tailLen);
  }
  else
  {
    tailLen = winLen;
    memcpy(tail, window, tailLen);
  }

  if (found)
  {
    tailLen = 0;
  }

  return found;
}

// -----------------------------------------------------------------------------
// USB host: library task (drains library-level events)
// -----------------------------------------------------------------------------

static void usbHostLibTask(void *arg)
{
  logTeeln("[usb-lib] task start");
  for (;;)
  {
    uint32_t flags = 0;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
    {
      logTee("[usb-lib] handle_events err=0x%x\n", err);
    }

    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
    {
      logTeeln("[usb-lib] no clients flag");
    }
    if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
    {
      logTeeln("[usb-lib] all devices free flag");
    }
  }
}

// -----------------------------------------------------------------------------
// USB host: client event callback + client task
// -----------------------------------------------------------------------------

// Pending attach address - set by callback, consumed by task loop.
static volatile uint8_t s_pendingAttachAddr = 0;

static void clientEventCb(const usb_host_client_event_msg_t *msg, void *arg)
{
  switch (msg->event)
  {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      logTee("[usb-cli] NEW_DEV addr=%u\n", msg->new_dev.address);
      if (!s_usb.claimed)
      {
        s_pendingAttachAddr = msg->new_dev.address;
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      logTeeln("[usb-cli] DEV_GONE");
      usbDetach();
      break;
    default:
      break;
  }
}

static void usbClientTask(void *arg)
{
  logTeeln("[usb-cli] task start");

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous   = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = clientEventCb;
  cfg.async.callback_arg          = nullptr;

  esp_err_t err = usb_host_client_register(&cfg, &s_usb.client);
  if (err != ESP_OK)
  {
    logTee("[usb-cli] register err=0x%x\n", err);
    vTaskDelete(nullptr);
    return;
  }

  for (;;)
  {
    err = usb_host_client_handle_events(s_usb.client, pdMS_TO_TICKS(50));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
    {
      logTee("[usb-cli] handle_events err=0x%x\n", err);
    }

    // Process pending attach outside of the event callback so that
    // control transfers (GET_DEVICE_ID etc.) can complete — their
    // callbacks are dispatched by handle_events which we must not block.
    uint8_t addr = s_pendingAttachAddr;
    if (addr != 0)
    {
      s_pendingAttachAddr = 0;
      usbTryAttachDevice(addr);
    }
  }
}

// -----------------------------------------------------------------------------
// USB host: locate printer interface, claim, set up endpoints
// -----------------------------------------------------------------------------

static bool descFindPrinterInterface(const usb_config_desc_t *cfg,
                                     uint8_t &ifNumber,
                                     uint8_t &epOut,
                                     uint16_t &epOutMps,
                                     uint8_t &epIn,
                                     uint16_t &epInMps)
{
  const uint8_t *p   = (const uint8_t *)cfg;
  const uint8_t *end = p + cfg->wTotalLength;
  p += cfg->bLength;

  bool foundIface = false;
  uint8_t curIfNum = 0;
  uint8_t curEpOut = 0, curEpIn = 0;
  uint16_t curOutMps = 0, curInMps = 0;

  while (p + 2 <= end)
  {
    uint8_t bLen  = p[0];
    uint8_t bType = p[1];
    if (bLen < 2 || p + bLen > end) break;

    if (bType == USB_B_DESCRIPTOR_TYPE_INTERFACE && bLen >= sizeof(usb_intf_desc_t))
    {
      const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;
      logTee("[usb-cli]  intf #%u alt=%u class=0x%02X sub=0x%02X proto=0x%02X eps=%u\n",
                    intf->bInterfaceNumber, intf->bAlternateSetting,
                    intf->bInterfaceClass, intf->bInterfaceSubClass,
                    intf->bInterfaceProtocol, intf->bNumEndpoints);

      // Prefer alt 0 of a printer-class interface
      if (intf->bInterfaceClass == USB_CLASS_PRINTER &&
          intf->bInterfaceSubClass == 0x01 /* printer subclass */ &&
          intf->bAlternateSetting == 0 && !foundIface)
      {
        foundIface = true;
        curIfNum   = intf->bInterfaceNumber;
        curEpOut   = 0;
        curEpIn    = 0;
        curOutMps  = 0;
        curInMps   = 0;
      }
    }
    else if (bType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && bLen >= sizeof(usb_ep_desc_t))
    {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;
      uint8_t xferType = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
      logTee("[usb-cli]   ep 0x%02X (%s) attr=0x%02X mps=%u\n",
                    ep->bEndpointAddress, epDirStr(ep->bEndpointAddress),
                    ep->bmAttributes, ep->wMaxPacketSize);
      if (foundIface && xferType == USB_BM_ATTRIBUTES_XFER_BULK)
      {
        if ((ep->bEndpointAddress & 0x80) == 0 && curEpOut == 0)
        {
          curEpOut  = ep->bEndpointAddress;
          curOutMps = ep->wMaxPacketSize;
        }
        else if ((ep->bEndpointAddress & 0x80) != 0 && curEpIn == 0)
        {
          curEpIn  = ep->bEndpointAddress;
          curInMps = ep->wMaxPacketSize;
        }
      }
    }
    p += bLen;
  }

  if (foundIface && curEpOut != 0)
  {
    ifNumber = curIfNum;
    epOut    = curEpOut;
    epOutMps = curOutMps;
    epIn     = curEpIn;
    epInMps  = curInMps;
    return true;
  }
  return false;
}

static bool usbTryAttachDevice(uint8_t devAddr)
{
  xSemaphoreTake(s_usb.lock, portMAX_DELAY);

  if (s_usb.claimed)
  {
    xSemaphoreGive(s_usb.lock);
    return false;
  }

  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(s_usb.client, devAddr, &dev);
  if (err != ESP_OK)
  {
    logTee("[usb-cli] device_open(%u) err=0x%x\n", devAddr, err);
    xSemaphoreGive(s_usb.lock);
    return false;
  }

  const usb_device_desc_t *ddesc = nullptr;
  if (usb_host_get_device_descriptor(dev, &ddesc) == ESP_OK && ddesc)
  {
    logTee("[usb-cli] device VID=0x%04X PID=0x%04X class=0x%02X bcdUSB=0x%04X\n",
                  ddesc->idVendor, ddesc->idProduct, ddesc->bDeviceClass, ddesc->bcdUSB);
    snprintf(s_printerIdent, sizeof(s_printerIdent),
             "VID=0x%04X PID=0x%04X", ddesc->idVendor, ddesc->idProduct);
  }

  const usb_config_desc_t *cdesc = nullptr;
  if (usb_host_get_active_config_descriptor(dev, &cdesc) != ESP_OK || !cdesc)
  {
    logTeeln("[usb-cli] cannot read config descriptor");
    usb_host_device_close(s_usb.client, dev);
    xSemaphoreGive(s_usb.lock);
    return false;
  }
  logTee("[usb-cli] config #%u, %u interfaces, %u bytes\n",
                cdesc->bConfigurationValue, cdesc->bNumInterfaces, cdesc->wTotalLength);

  uint8_t  ifNum = 0, epOut = 0, epIn = 0;
  uint16_t epOutMps = 0, epInMps = 0;
  if (!descFindPrinterInterface(cdesc, ifNum, epOut, epOutMps, epIn, epInMps))
  {
    logTeeln("[usb-cli] no Printer Class interface with bulk OUT found");
    usb_host_device_close(s_usb.client, dev);
    xSemaphoreGive(s_usb.lock);
    return false;
  }

  logTee("[usb-cli] claiming if=%u epOut=0x%02X mps=%u epIn=0x%02X mps=%u\n",
                ifNum, epOut, epOutMps, epIn, epInMps);
  err = usb_host_interface_claim(s_usb.client, dev, ifNum, 0);
  if (err != ESP_OK)
  {
    logTee("[usb-cli] interface_claim err=0x%x\n", err);
    usb_host_device_close(s_usb.client, dev);
    xSemaphoreGive(s_usb.lock);
    return false;
  }

  s_usb.device     = dev;
  s_usb.devAddr    = devAddr;
  s_usb.ifNumber   = ifNum;
  s_usb.epOut      = epOut;
  s_usb.epOutMps   = epOutMps;
  s_usb.epIn       = epIn;
  s_usb.epInMps    = epInMps;
  s_usb.claimed    = true;
  // Don't flip s_printerReady=true yet.  We're about to spend up to ~4 s
  // doing class-specific control transfers (GET_DEVICE_ID, GET_PORT_STATUS).
  // If TCP clients see the printer as "ready" during that window their
  // bulk OUT writes race with the control transfers, and on this hub
  // path they consistently lose - bytes_out stays 0.  Set ready only
  // after the attach handshake settles.

  xSemaphoreGive(s_usb.lock);

  // USB Printer Class spec doesn't mandate a settle time after interface
  // claim, but real-world devices (especially M1132/M1136 via a hub) need
  // a brief pause before they reliably answer class-specific control
  // requests like GET_DEVICE_ID / GET_PORT_STATUS.  Without this delay
  // we routinely observe GET_DEVICE_ID timing out (1500ms wasted) on
  // first attach.
  vTaskDelay(pdMS_TO_TICKS(300));

  // Best-effort: query IEEE 1284 device ID string for nice logging / status page.
  // Retry once on timeout - some prints especially on hub paths NAK the
  // first request while their firmware is still finishing enumeration.
  s_deviceId[0] = '\0';
  esp_err_t didErr = usbCtrlGetDeviceId(s_deviceId, sizeof(s_deviceId));
  if (didErr != ESP_OK || !s_deviceId[0])
  {
    logTee("[usb-cli] GET_DEVICE_ID first try err=0x%x, retrying after 500ms\n",
           didErr);
    vTaskDelay(pdMS_TO_TICKS(500));
    didErr = usbCtrlGetDeviceId(s_deviceId, sizeof(s_deviceId));
  }
  if (didErr == ESP_OK && s_deviceId[0])
  {
    logTee("[usb-cli] Device ID: %s\n", s_deviceId);
    // Use device ID as the friendly identifier (truncate to fit)
    size_t identMax = sizeof(s_printerIdent) - 1;
    strncpy(s_printerIdent, s_deviceId, identMax);
    s_printerIdent[identMax] = '\0';
  }
  else
  {
    logTee("[usb-cli] GET_DEVICE_ID failed err=0x%x (printer may still work)\n",
           didErr);
  }

  uint8_t st = 0;
  esp_err_t psErr = usbCtrlGetPortStatus(&st);
  if (psErr == ESP_OK)
  {
    s_portStatus = st;
    logTee("[usb-cli] Port status: 0x%02X\n", st);
  }
  else
  {
    logTee("[usb-cli] GET_PORT_STATUS failed err=0x%x\n", psErr);
  }

  // Attach handshake done (success or graceful failure).  Now publish ready.
  s_printerReady = true;
  logTeeln("[usb-cli] printer attached and ready");

  // Pre-allocate persistent Bulk OUT transfer
  if (s_outXfer == nullptr)
  {
    if (usb_host_transfer_alloc(kUsbChunkBytes, 0, &s_outXfer) == ESP_OK)
    {
      s_outDone = xSemaphoreCreateBinary();
      logTeeln("[usb-cli] pre-allocated OUT transfer");
    }
    else
    {
      logTeeln("[usb-cli] WARN: OUT transfer pre-alloc failed, will use per-call alloc");
      s_outXfer = nullptr;
    }
  }

  s_inErrorStreak = 0;

  return true;
}

static void usbDetach()
{
  xSemaphoreTake(s_usb.lock, portMAX_DELAY);
  if (s_usb.claimed)
  {
    logTeeln("[usb-cli] detaching");
    usb_host_endpoint_halt(s_usb.device, s_usb.epOut);
    usb_host_endpoint_flush(s_usb.device, s_usb.epOut);
    if (s_usb.epIn)
    {
      usb_host_endpoint_halt(s_usb.device, s_usb.epIn);
      usb_host_endpoint_flush(s_usb.device, s_usb.epIn);
    }
    usb_host_interface_release(s_usb.client, s_usb.device, s_usb.ifNumber);
    usb_host_device_close(s_usb.client, s_usb.device);
  }
  s_usb.device   = nullptr;
  s_usb.claimed  = false;
  s_usb.epOut    = 0;
  s_usb.epIn     = 0;
  s_usb.epOutMps = 0;
  s_usb.epInMps  = 0;
  // Free persistent transfers
  if (s_outXfer)
  {
    usb_host_transfer_free(s_outXfer);
    s_outXfer = nullptr;
  }
  if (s_outDone)
  {
    vSemaphoreDelete(s_outDone);
    s_outDone = nullptr;
  }
  s_printerReady = false;
  s_deviceId[0]  = '\0';
  s_portStatus   = kPortStatusUnknown;
  snprintf(s_printerIdent, sizeof(s_printerIdent), "(no printer)");
  xSemaphoreGive(s_usb.lock);
}

// -----------------------------------------------------------------------------
// USB host: control transfers (Printer class requests)
// -----------------------------------------------------------------------------

struct CtrlCtx
{
  SemaphoreHandle_t done;
  esp_err_t         status;
  int               actual;
};

static void IRAM_ATTR ctrlXferCb(usb_transfer_t *xfer)
{
  CtrlCtx *ctx = (CtrlCtx *)xfer->context;
  ctx->status = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
  ctx->actual = xfer->actual_num_bytes;
  BaseType_t hpw = pdFALSE;
  xSemaphoreGiveFromISR(ctx->done, &hpw);
  if (hpw) portYIELD_FROM_ISR();
}

// Synchronous control transfer wrapper.  Returns ESP_OK and writes actualLen on
// success.  data may be nullptr for OUT requests with wLength=0.
static esp_err_t usbCtrlSync(uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                             uint8_t *data, int *actualLen, uint32_t timeoutMs)
{
  if (!s_usb.claimed) return ESP_ERR_INVALID_STATE;

  // Control transfer allocation must include the 8-byte setup packet.
  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(8 + wLength, 0, &xfer);
  if (err != ESP_OK) return err;

  // Setup packet (little-endian)
  xfer->data_buffer[0] = bmRequestType;
  xfer->data_buffer[1] = bRequest;
  xfer->data_buffer[2] = (uint8_t)(wValue  & 0xFF);
  xfer->data_buffer[3] = (uint8_t)(wValue  >> 8);
  xfer->data_buffer[4] = (uint8_t)(wIndex  & 0xFF);
  xfer->data_buffer[5] = (uint8_t)(wIndex  >> 8);
  xfer->data_buffer[6] = (uint8_t)(wLength & 0xFF);
  xfer->data_buffer[7] = (uint8_t)(wLength >> 8);

  // For OUT direction with data, copy payload after the setup packet.
  if ((bmRequestType & 0x80) == 0 && data && wLength)
  {
    memcpy(xfer->data_buffer + 8, data, wLength);
  }

  CtrlCtx ctx = {};
  ctx.done = xSemaphoreCreateBinary();

  xfer->num_bytes        = 8 + wLength;
  xfer->device_handle    = s_usb.device;
  xfer->bEndpointAddress = 0;   // control endpoint
  xfer->callback         = ctrlXferCb;
  xfer->context          = &ctx;
  xfer->timeout_ms       = timeoutMs;

  err = usb_host_transfer_submit_control(s_usb.client, xfer);
  if (err == ESP_OK)
  {
    // Pump the client event loop while waiting for the control transfer to
    // complete.  The completion callback (ctrlXferCb) is dispatched by
    // handle_events; if we just block on the semaphore, it will never fire
    // because nobody is calling handle_events.
    uint32_t deadline = millis() + timeoutMs + 200;
    while (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      usb_host_client_handle_events(s_usb.client, pdMS_TO_TICKS(10));
      if (millis() >= deadline)
      {
        err = ESP_ERR_TIMEOUT;
        break;
      }
    }
    if (err == ESP_OK)
    {
      err = ctx.status;
    }
  }

  if (err == ESP_OK && (bmRequestType & 0x80) && data && wLength)
  {
    // Data stage payload is at offset 8 in the buffer after the setup packet
    int dataActual = ctx.actual - 8;
    if (dataActual < 0) dataActual = 0;
    if (dataActual > (int)wLength) dataActual = wLength;
    memcpy(data, xfer->data_buffer + 8, dataActual);
    if (actualLen) *actualLen = dataActual;
  }
  else if (actualLen)
  {
    *actualLen = 0;
  }

  vSemaphoreDelete(ctx.done);
  usb_host_transfer_free(xfer);
  return err;
}

// USB Printer Class GET_DEVICE_ID  (returns IEEE 1284 string).
// The first two bytes are a big-endian length of the whole reply.
static esp_err_t usbCtrlGetDeviceId(char *out, size_t outSz)
{
  if (!out || outSz < 4) return ESP_ERR_INVALID_ARG;
  out[0] = '\0';

  // wIndex high byte = interface number, low byte = alt setting (0)
  uint16_t wIndex = ((uint16_t)s_usb.ifNumber) << 8;

  uint8_t buf[256];
  int actual = 0;
  esp_err_t err = usbCtrlSync(REQ_TYPE_CLASS_IFACE_IN,
                              PRINTER_REQ_GET_DEVICE_ID,
                              /*wValue=*/0,    // config = 0
                              wIndex,
                              sizeof(buf),
                              buf, &actual, 1500);
  if (err != ESP_OK) return err;
  if (actual < 2) return ESP_ERR_INVALID_RESPONSE;

  uint16_t len = ((uint16_t)buf[0] << 8) | buf[1];
  if (len > (uint16_t)actual) len = actual;
  if (len < 2) return ESP_ERR_INVALID_RESPONSE;

  size_t copy = len - 2;
  if (copy >= outSz) copy = outSz - 1;
  memcpy(out, buf + 2, copy);
  out[copy] = '\0';

  // The 1284 string can contain ';' separators which look fine on the web page;
  // strip CR/LF just in case.
  for (size_t i = 0; i < copy; i++)
  {
    if (out[i] == '\r' || out[i] == '\n') out[i] = ' ';
  }
  return ESP_OK;
}

static esp_err_t usbCtrlGetPortStatus(uint8_t *status)
{
  if (!status) return ESP_ERR_INVALID_ARG;
  // USB Printer Class spec: wIndex high byte = interface number, low byte = 0.
  uint16_t wIndex = ((uint16_t)s_usb.ifNumber) << 8;
  uint8_t st = 0xFF;
  int actual = 0;
  esp_err_t err = usbCtrlSync(REQ_TYPE_CLASS_IFACE_IN,
                              PRINTER_REQ_GET_PORT_STATUS,
                              0, wIndex, 1, &st, &actual, 800);
  if (err == ESP_OK && actual >= 1) *status = st;
  return err;
}

static esp_err_t usbCtrlSoftReset()
{
  // Same wIndex convention as GET_PORT_STATUS / GET_DEVICE_ID.
  uint16_t wIndex = ((uint16_t)s_usb.ifNumber) << 8;
  return usbCtrlSync(REQ_TYPE_CLASS_IFACE_OUT,
                     PRINTER_REQ_SOFT_RESET,
                     0, wIndex, 0, nullptr, nullptr, 1000);
}

// -----------------------------------------------------------------------------
// USB host: bulk OUT writer task - pulls from stream buffer and writes
// -----------------------------------------------------------------------------

struct WriteCtx
{
  SemaphoreHandle_t done;
  esp_err_t         status;
  size_t            actual;
};

static void IRAM_ATTR writeXferCb(usb_transfer_t *xfer)
{
  WriteCtx *ctx = (WriteCtx *)xfer->context;
  ctx->status = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
  ctx->actual = xfer->actual_num_bytes;
  BaseType_t hpw = pdFALSE;
  xSemaphoreGiveFromISR(ctx->done, &hpw);
  if (hpw) portYIELD_FROM_ISR();
}

static esp_err_t usbBulkOutSync(const uint8_t *data, size_t len, uint32_t timeoutMs)
{
  if (!s_usb.claimed) return ESP_ERR_INVALID_STATE;

  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(len, 0, &xfer);
  if (err != ESP_OK) return err;

  WriteCtx ctx = {};
  ctx.done = xSemaphoreCreateBinary();

  memcpy(xfer->data_buffer, data, len);
  xfer->num_bytes        = len;
  xfer->device_handle    = s_usb.device;
  xfer->bEndpointAddress = s_usb.epOut;
  xfer->callback         = writeXferCb;
  xfer->context          = &ctx;
  xfer->timeout_ms       = timeoutMs;

  if (s_usbSubmitMux) xSemaphoreTake(s_usbSubmitMux, portMAX_DELAY);
  err = usb_host_transfer_submit(xfer);
  if (s_usbSubmitMux) xSemaphoreGive(s_usbSubmitMux);
  if (err == ESP_OK)
  {
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeoutMs + 200)) != pdTRUE)
    {
      logTeeln("[usb-out] transfer timeout (semaphore)");
      err = ESP_ERR_TIMEOUT;
    }
    else
    {
      err = ctx.status;
    }
  }
  else
  {
    logTee("[usb-out] submit err=0x%x\n", err);
  }

  vSemaphoreDelete(ctx.done);
  usb_host_transfer_free(xfer);
  return err;
}

static void usbWriterTask(void *arg)
{
  logTeeln("[usb-out] task start");
  static uint8_t chunk[kUsbChunkBytes];

  for (;;)
  {
    size_t got = xStreamBufferReceive(s_stream, chunk, sizeof(chunk),
                                      pdMS_TO_TICKS(500));
    if (got == 0) continue;

    // If printer disappeared mid-job, wait briefly then drop remaining bytes.
    if (!s_printerReady)
    {
      logTee("[usb-out] printer not ready, waiting up to %u ms\n",
                    (unsigned)kPrinterReadyWaitMs);
      if (!waitForPrinter(kPrinterReadyWaitMs))
      {
        logTee("[usb-out] still no printer, discarding %u bytes\n", (unsigned)got);
        jobOnDropped(got);
        size_t drained = 0;
        while (xStreamBufferReceive(s_stream, chunk, sizeof(chunk), 0) > 0)
        {
          drained += sizeof(chunk);
          jobOnDropped(sizeof(chunk));
        }
        if (drained) logTee("[usb-out] drained extra %u bytes\n", (unsigned)drained);
        continue;
      }
    }

    size_t off = 0;
    while (off < got)
    {
      size_t n = got - off;
      if (n > kUsbChunkBytes) n = kUsbChunkBytes;

      esp_err_t err;

      // Use persistent transfer if available, else fallback to alloc
      if (s_outXfer && s_outDone)
      {
        memcpy(s_outXfer->data_buffer, chunk + off, n);
        s_outXfer->num_bytes        = n;
        s_outXfer->device_handle    = s_usb.device;
        s_outXfer->bEndpointAddress = s_usb.epOut;
        s_outXfer->callback         = writeXferCb;
        s_outXfer->context          = nullptr;  // use global semaphore
        s_outXfer->timeout_ms       = 8000;

        // writeXferCb needs to give s_outDone
        // We'll reuse WriteCtx pattern with the persistent semaphore
        WriteCtx outCtx = {};
        outCtx.done = s_outDone;
        s_outXfer->context = &outCtx;

        xSemaphoreTake(s_usbSubmitMux, portMAX_DELAY);
        err = usb_host_transfer_submit(s_outXfer);
        xSemaphoreGive(s_usbSubmitMux);
        if (err == ESP_OK)
        {
          if (xSemaphoreTake(s_outDone, pdMS_TO_TICKS(8200)) != pdTRUE)
          {
            logTeeln("[usb-out] transfer timeout (semaphore)");
            err = ESP_ERR_TIMEOUT;
          }
          else
          {
            err = outCtx.status;
          }
        }
        else
        {
          logTee("[usb-out] submit err=0x%x\n", err);
        }
      }
      else
      {
        // Fallback: per-call alloc (original path)
        err = usbBulkOutSync(chunk + off, n, 8000);
      }

      if (err != ESP_OK)
      {
        s_usbConsecutiveErrors++;
        logTee("[usb-out] bulk write err=0x%x at offset=%u (consecutive=%u)\n",
               err, (unsigned)off, (unsigned)s_usbConsecutiveErrors);
        if (err == ESP_ERR_TIMEOUT) jobOnUsbTimeout();
        jobOnUsbErr((uint32_t)err);
        if (s_usb.claimed)
        {
          usb_host_endpoint_clear(s_usb.device, s_usb.epOut);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        break;
      }
      s_usbConsecutiveErrors = 0;
      off += n;
      s_totalBytesOut += n;
      s_lastUsbWriteMs = millis();
      jobOnUsbWrite(n);
    }
  }
}

// -----------------------------------------------------------------------------
// USB host: bulk IN reader task - keeps printer-side FIFO drained
// -----------------------------------------------------------------------------

static void IRAM_ATTR readXferCb(usb_transfer_t *xfer)
{
  WriteCtx *ctx = (WriteCtx *)xfer->context;
  if (ctx)
  {
    ctx->status = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
    ctx->actual = xfer->actual_num_bytes;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(ctx->done, &hpw);
    if (hpw) portYIELD_FROM_ISR();
  }
}

static void usbReaderTask(void *arg)
{
  logTeeln("[usb-in] task disabled (Bulk IN polling off for stability)");
  for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

// -----------------------------------------------------------------------------
// Back-channel forwarder task: drains s_backBuf and sends to TCP client.
// Runs on core 1 to avoid contending with USB tasks on core 0.
// -----------------------------------------------------------------------------

static void backFwdTask(void *arg)
{
  logTeeln("[back-fwd] task disabled (no Bulk IN data)");
  for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

// -----------------------------------------------------------------------------
// TCP raw server (port 9100)
//
// Implementation note: we use raw lwIP / POSIX-style socket calls here rather
// than Arduino's WiFiServer because under arduino-as-IDF-component + Arduino-
// ESP32 3.x + IDF v4.4, WiFiServer::accept()/available() does not deliver
// successfully-handshaken clients to userspace.  `nc -vz` reports the TCP
// handshake completed, but the WiFiServer wrapper never produces a non-null
// WiFiClient, so all "[raw9100] connection from" log lines were never emitted
// and /jobs stayed empty.  Going to POSIX socket avoids the wrapper entirely.
// -----------------------------------------------------------------------------

// Helper: open a listening TCP socket on the given port.  Returns fd >= 0 on
// success, -1 on failure.  Sets SO_REUSEADDR so a quick restart can rebind.
static int openListenSocket(uint16_t port, int backlog)
{
  int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (lfd < 0)
  {
    logTee("[sock] socket() failed errno=%d\n", errno);
    return -1;
  }

  int yes = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr = {};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(port);

  if (::bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    logTee("[sock] bind(:%u) failed errno=%d\n", (unsigned)port, errno);
    ::close(lfd);
    return -1;
  }

  if (::listen(lfd, backlog) < 0)
  {
    logTee("[sock] listen(:%u) failed errno=%d\n", (unsigned)port, errno);
    ::close(lfd);
    return -1;
  }
  return lfd;
}

static void rawServerTask(void *arg)
{
  logTeeln("[raw9100] task start");

  // Bring up the listen socket.  If Wi-Fi happens to be down at this exact
  // moment, retry until it succeeds - we don't want to wedge forever.
  int lfd = -1;
  while (lfd < 0)
  {
    lfd = openListenSocket(kRawPort, 1);
    if (lfd < 0)
    {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  logTee("[raw9100] listening on :%u (fd=%d), entering accept loop\n",
                (unsigned)kRawPort, lfd);

  uint8_t buf[1460];

  for (;;)
  {
    struct sockaddr_in peer = {};
    socklen_t          peerLen = sizeof(peer);
    int cfd = ::accept(lfd, (struct sockaddr *)&peer, &peerLen);
    if (cfd < 0)
    {
      logTee("[raw9100] accept errno=%d (%s)\n", errno, strerror(errno));
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Format peer IP.
    char peerIp[16] = {0};
    inet_ntoa_r(peer.sin_addr, peerIp, sizeof(peerIp));
    logTee("[raw9100] connection from %s\n", peerIp);

    // Configure the client socket: TCP_NODELAY + short idle read timeout so we
    // can poll for job-complete signals.
    int yes = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    struct timeval idleTo = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &idleTo, sizeof(idleTo));

    // If no printer is attached, hold briefly then refuse the job so the
    // spooler reports failure instead of silently succeeding.
    if (!s_printerReady && !waitForPrinter(kPrinterReadyWaitMs))
    {
      logTeeln("[raw9100] no printer, dropping connection");
      s_jobsDropped++;
      uint32_t jid = jobBegin(peerIp);
      (void)jid;
      jobEnd(JCR_NO_PRINTER);
      ::close(cfd);
      continue;
    }

    s_jobsAccepted++;
    s_lastJobBytes = 0;
    uint32_t jobId = jobBegin(peerIp);
    logTee("[raw9100] job #%u start\n", (unsigned)jobId);

    s_jobGeneration++;
    s_jobComplete   = false;
    s_jobCompleteMs = 0;
    s_usbConsecutiveErrors = 0;
    s_syntheticInjected = false;
    s_lastUsbWriteMs = 0;
    uint32_t nowMs  = millis();
    s_lastRecvActivityMs = nowMs;

    // Publish the active client fd to the USB reader so back-channel
    // (printer -> host) bytes can flow back over TCP.
    s_rawClientFd = cfd;

    uint8_t closeReason = JCR_CLIENT_FIN;

    for (;;)
    {
      ssize_t got = ::recv(cfd, buf, sizeof(buf), 0);
      if (got > 0)
      {
        s_totalBytesIn += got;
        s_lastJobBytes += got;
        s_lastRecvActivityMs = millis();
        jobAppendPayload(buf, (size_t)got);

        // Push to stream buffer; throttle on backpressure.
        size_t off = 0;
        while ((ssize_t)off < got)
        {
          size_t sent = xStreamBufferSend(s_stream, buf + off, got - off,
                                          pdMS_TO_TICKS(5000));
          if (sent == 0)
          {
            logTeeln("[raw9100] stream buffer full, retrying");
            continue;
          }
          off += sent;
        }
        jobOnStreamHighWater(xStreamBufferBytesAvailable(s_stream));
      }
      else if (got == 0)
      {
        // Peer closed (FIN).
        closeReason = JCR_CLIENT_FIN;
        logTeeln("[raw9100] peer sent FIN");
        break;
      }
      else
      {
        // got < 0; SO_RCVTIMEO fires every 2s.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          uint32_t now = millis();

          // Fast path: PJL job-complete detected by Bulk IN parser
          if (s_jobComplete && (now - s_jobCompleteMs) >= kJobCompleteQuietMs)
          {
            closeReason = JCR_JOB_COMPLETE;
            logTeeln("[raw9100] PJL job-complete detected, closing");
            break;
          }

          // USB is broken: sustained errors mean the printer can't accept
          // more data; continuing to recv from CUPS is pointless.
          if (s_usbConsecutiveErrors >= kMaxConsecutiveUsbErrors)
          {
            closeReason = JCR_USB_ERROR;
            logTee("[raw9100] USB stuck (%u consecutive errors), closing\n",
                   (unsigned)s_usbConsecutiveErrors);
            break;
          }

          // -------------------------------------------------------
          // Synthetic Ready injection: once all print data has been
          // forwarded to USB and the printer has been idle for a few
          // seconds, inject a PJL "Ready" + "Job End" status back to
          // CUPS so its readThread can clear media-needed-error and
          // finish the job.  This is necessary because Bulk IN is
          // disabled (crashes on this printer's USB host stack).
          // -------------------------------------------------------
          bool streamEmpty = (xStreamBufferBytesAvailable(s_stream) == 0);
          bool usbIdle     = s_lastUsbWriteMs != 0 &&
                             (now - s_lastUsbWriteMs) >= 2000;
          bool hasData     = s_lastJobBytes > 0;

          if (streamEmpty && usbIdle && hasData && !s_syntheticInjected)
          {
            // The HP rastertozjs readerProcess detects "end of job" by
            // finding "@PJL USTATUS JOB\r\nEND" in the back-channel data.
            // The \x0c (form feed) separates two PJL messages.
            // This exact 107-byte format was verified working in job 148.
            static const char kSyntheticStatus[] =
              "\r\n@PJL USTATUS JOB\r\nEND\r\nNAME=\"job\"\r\nPAGES=1\r\n\x0c"
              "@PJL INFO STATUS\r\nCODE=10001\r\n"
              "DISPLAY=\"Ready\"\r\nONLINE=TRUE\r\n";

            ssize_t w = ::send(cfd, kSyntheticStatus,
                               sizeof(kSyntheticStatus) - 1, MSG_DONTWAIT);
            s_syntheticInjected = true;
            logTee("[raw9100] injected synthetic Ready+JobEnd (%d bytes)\n", (int)w);

            // After injection, wait 2s for CUPS to process, then force close
            vTaskDelay(pdMS_TO_TICKS(2000));
            closeReason = JCR_JOB_COMPLETE;
            logTeeln("[raw9100] post-injection timeout, closing");
            break;
          }

          // Hard idle timeout (120s - generous for slow renderers)
          if (now - s_lastRecvActivityMs >= 120000)
          {
            logTeeln("[raw9100] idle timeout (120s), closing");
            closeReason = JCR_IDLE_TIMEOUT;
            break;
          }
          continue;
        }
        logTee("[raw9100] recv errno=%d, closing\n", errno);
        closeReason = JCR_OTHER;
        break;
      }
    }

    // Drain phase: we need to wait for the USB writer task to actually
    // finish pushing every queued byte to the printer, not just for the
    // FreeRTOS StreamBuffer to drain.  Bulk transfers can finish ~10-20 ms
    // AFTER the stream buffer is empty (the writer has already received
    // the bytes into its local chunk[] and is mid-flight).  If we finalize
    // the JobRecord before that, we get the misleading "bytes_in=N bytes_out=0"
    // result that previously confused everything.
    //
    // Termination conditions, whichever comes first:
    //  1. usbBytesOutAtJobEnd has caught up with bytesInThisJob
    //  2. 60 s elapsed (large rasterised pages can be slow)
    //  3. printer disconnected / USB error count grew
    uint32_t bytesInThisJob       = s_lastJobBytes;
    uint32_t bytesOutAtJobStart   = s_totalBytesOut - 0;  // see note below
    // Note: s_totalBytesOut is global. For per-job comparison we want the
    // delta since job start; we record it via the JobRecord's bytes_out
    // counter (updated by jobOnUsbWrite()), but the simplest reliable check
    // here is "stream buffer empty AND s_totalBytesOut hasn't moved for 200ms".
    uint32_t drainStart           = millis();
    uint32_t lastOutSeen          = s_totalBytesOut;
    uint32_t lastOutChangeMs      = millis();
    for (;;)
    {
      bool streamEmpty = (xStreamBufferBytesAvailable(s_stream) == 0);
      uint32_t now = millis();
      if (s_totalBytesOut != lastOutSeen)
      {
        lastOutSeen     = s_totalBytesOut;
        lastOutChangeMs = now;
      }
      bool usbQuiescent = streamEmpty && (now - lastOutChangeMs) >= kDrainQuiescentMs;
      bool jobDoneSignal = s_jobComplete && (now - s_jobCompleteMs) >= kJobCompleteQuietMs;
      if (closeReason == JCR_JOB_COMPLETE || jobDoneSignal || usbQuiescent)
      {
        break;
      }
      if (now - drainStart >= 60000)
      {
        logTeeln("[raw9100] drain timeout (60s), giving up on per-job accounting");
        break;
      }
      if (!s_printerReady)
      {
        logTeeln("[raw9100] printer disappeared during drain");
        break;
      }
      if (s_usbConsecutiveErrors >= kMaxConsecutiveUsbErrors)
      {
        logTeeln("[raw9100] USB stuck during drain, aborting");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    logTeeln("[raw9100] drain complete");

    logTeeln("[raw9100] sending FIN");
    ::shutdown(cfd, SHUT_WR);

    uint32_t finStart = millis();
    bool peerClosed = false;
    for (;;)
    {
      uint8_t scratch[128];
      ssize_t r = ::recv(cfd, scratch, sizeof(scratch), MSG_DONTWAIT);
      if (r == 0)
      {
        peerClosed = true;
        logTee("[raw9100] peer closed %u ms after FIN\n",
                      (unsigned)(millis() - finStart));
        break;
      }
      if (r > 0)
      {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        if (millis() - finStart >= kFinWaitMs)
        {
          logTeeln("[raw9100] FIN wait timeout, forcing close");
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      logTee("[raw9100] recv during FIN errno=%d\n", errno);
      break;
    }

    s_rawClientFd = -1;
    ::close(cfd);

    if (!peerClosed)
    {
      logTeeln("[raw9100] peer did not close before timeout");
    }

    // Now the per-job byte counters are accurate.
    logTee("[raw9100] job #%u closed reason=%s bytes_in=%u total in=%u out=%u\n",
                  (unsigned)jobId,
                  jobCloseReasonName(closeReason),
                  (unsigned)bytesInThisJob,
                  (unsigned)s_totalBytesIn,
                  (unsigned)s_totalBytesOut);
    (void)bytesOutAtJobStart;  // currently unused; see note above

    // Refresh port status after each job (best-effort)
    if (s_printerReady)
    {
      uint8_t st = 0;
      if (usbCtrlGetPortStatus(&st) == ESP_OK) s_portStatus = st;
    }

    // Finalise the JobRecord after USB drain so bytes_out / usb counters
    // reflect the actual outcome, not just the TCP-side state.
    jobEnd(closeReason);
    s_jobComplete = false;
  }
}

// -----------------------------------------------------------------------------
// HTTP status page
// -----------------------------------------------------------------------------

static String portStatusDescr(uint8_t st)
{
  // USB Printer Class port status bits:
  //   bit 5: Paper Empty   (1 = no paper)
  //   bit 4: Select        (1 = selected / online)
  //   bit 3: Not Error     (1 = no error)
  // All other bits reserved.
  //
  // Special sentinel kPortStatusUnknown (0xFF) means GET_PORT_STATUS never
  // succeeded - common on M1132/M1136 via a USB hub.  Bulk OUT printing
  // still works in that state; we just can't read the printer's status.
  if (st == kPortStatusUnknown)
  {
    return String("unknown (GET_PORT_STATUS not answered by printer)");
  }
  String s = "0x";
  if (st < 0x10) s += "0";
  s += String(st, HEX);
  s += " (";
  s += (st & 0x08) ? "OK" : "ERROR";
  s += (st & 0x10) ? ", online" : ", offline";
  if (st & 0x20)   s += ", PAPER EMPTY";
  s += ")";
  return s;
}

static void handleRoot()
{
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<title>ESP32 USB Print Bridge</title>";
  html += "<style>body{font-family:-apple-system,sans-serif;max-width:720px;margin:2em auto;padding:0 1em;color:#222}";
  html += "h1{margin-bottom:0}code,pre{background:#f4f4f4;padding:2px 4px;border-radius:3px}";
  html += "table{border-collapse:collapse;margin-top:1em}td{padding:4px 12px;border-bottom:1px solid #eee;vertical-align:top}";
  html += ".ok{color:#0a0;font-weight:bold}.bad{color:#a00;font-weight:bold}</style></head><body>";
  html += "<h1>ESP32 USB Print Bridge</h1>";
  html += "<p>Bridge to HP LaserJet M1136 over USB &mdash; clients print via TCP <code>:9100</code>.</p>";
  html += "<table>";
  html += "<tr><td>Wi-Fi</td><td>" + WiFi.localIP().toString() + " (" + macToString() + ")</td></tr>";
  html += "<tr><td>mDNS</td><td><code>" + String(kMdnsHostname) + ".local</code></td></tr>";
  html += "<tr><td>Printer</td><td><span class='";
  html += s_printerReady ? "ok'>READY" : "bad'>not connected";
  html += "</span></td></tr>";
  html += "<tr><td>Identity</td><td>" + String(s_printerIdent) + "</td></tr>";
  if (s_deviceId[0])
  {
    html += "<tr><td>Device ID</td><td><code style='word-break:break-all'>" + String(s_deviceId) + "</code></td></tr>";
  }
  html += "<tr><td>Port status</td><td>" + portStatusDescr(s_portStatus) + "</td></tr>";
  html += "<tr><td>Listen port</td><td>TCP " + String(kRawPort) + " (HP JetDirect / AppSocket)</td></tr>";
  html += "<tr><td>Jobs accepted</td><td>" + String((unsigned)s_jobsAccepted) + " (dropped: " + String((unsigned)s_jobsDropped) + ")</td></tr>";
  html += "<tr><td>Last job bytes</td><td>" + String((unsigned)s_lastJobBytes) + "</td></tr>";
  html += "<tr><td>Bytes in / out / back</td><td>" + String((unsigned)s_totalBytesIn) +
          " / " + String((unsigned)s_totalBytesOut) +
          " / " + String((unsigned)s_totalBytesBack) +
          " (back\u2192client " + String((unsigned)s_totalBackToClient) + ")</td></tr>";
  html += "<tr><td>Free heap</td><td>" + String((unsigned)esp_get_free_heap_size()) +
          " (PSRAM free " + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + ")</td></tr>";
  // Internal SRAM is what String, Wi-Fi, lwIP, USB Host all carve from.
  // When it dips below ~10 KB the device is liable to crash on the next
  // big String allocation.  Watch this number.
  {
    size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t intMin  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    html += "<tr><td>Internal SRAM free</td><td>" + String((unsigned)intFree) +
            " (min seen " + String((unsigned)intMin) + ")</td></tr>";
  }
  html += "</table>";

  // --- Last job summary (most useful single block when debugging) ---
  JobRecord lastJob;
  if (jobLogLatest(&lastJob))
  {
    uint32_t now      = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t agoMs    = now - (lastJob.end_ms ? lastJob.end_ms : lastJob.start_ms);
    uint32_t durMs    = (lastJob.end_ms ? lastJob.end_ms : now) - lastJob.start_ms;
    bool inEqOut      = (lastJob.bytes_in == lastJob.bytes_out);
    bool sigGood      = (lastJob.sig == SIG_HP_UEL_PJL_ZJS);

    html += "<h3>Last job #" + String((unsigned)lastJob.id) + "</h3><table>";
    html += "<tr><td>From</td><td><code>" + String(lastJob.peer_ip) + "</code></td></tr>";
    html += "<tr><td>When</td><td>" + String((unsigned)(agoMs / 1000)) + "s ago, lasted " +
            String((unsigned)durMs) + " ms</td></tr>";
    html += "<tr><td>Close</td><td>" + String(jobCloseReasonName(lastJob.close_reason)) + "</td></tr>";
    html += "<tr><td>Bytes in / out / dropped</td><td>" +
            String((unsigned)lastJob.bytes_in) + " / " +
            String((unsigned)lastJob.bytes_out) + " / " +
            String((unsigned)lastJob.bytes_dropped) +
            (inEqOut ? " <span class='ok'>(in==out)</span>" : " <span class='bad'>(in!=out)</span>") +
            "</td></tr>";
    html += "<tr><td>USB errors / timeouts</td><td>" +
            String((unsigned)lastJob.usb_err_count) + " / " +
            String((unsigned)lastJob.usb_timeout_count);
    if (lastJob.last_usb_err)
    {
      html += " (last err 0x" + String((unsigned)lastJob.last_usb_err, HEX) + ")";
    }
    html += "</td></tr>";
    html += "<tr><td>Stream peak</td><td>" + String((unsigned)lastJob.peak_stream_used) + " bytes</td></tr>";
    html += "<tr><td>Signature</td><td><span class='";
    html += sigGood ? "ok'>" : "bad'>";
    html += String(jobSigName(lastJob.sig)) + "</span>";
    if (!sigGood && lastJob.sig != SIG_UNKNOWN)
    {
      html += " &mdash; <b>likely wrong driver on the host</b>";
    }
    html += "</td></tr>";
    html += "</table>";
    html += "<p>See <a href='/jobs?fmt=html'>/jobs</a> for full history with hex dump, or "
            "<a href='/tail'>/tail</a> for log snapshot.</p>";
  }
  else
  {
    html += "<h3>Last job</h3><p>(no jobs recorded yet)</p>";
  }
  html += "<hr><h3>How to add on macOS</h3><ol>";
  html += "<li>System Settings &rarr; Printers &amp; Scanners &rarr; Add Printer</li>";
  html += "<li>IP tab. Protocol: <b>HP JetDirect &mdash; Socket</b></li>";
  html += "<li>Address: <code>" + WiFi.localIP().toString() + "</code> &nbsp; (or <code>" + String(kMdnsHostname) + ".local</code>)</li>";
  html += "<li>Driver: <b>HP LaserJet Professional M1132 MFP</b> (works for M1136).</li>";
  html += "</ol>";
  html += "<p><a href='/reset'>Send SOFT_RESET to printer</a></p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

static void handleReset()
{
  if (!s_printerReady)
  {
    webServer.send(503, "text/plain", "printer not ready");
    return;
  }
  esp_err_t err = usbCtrlSoftReset();
  String msg = String("SOFT_RESET err=0x") + String((unsigned)err, HEX);
  logTeeln("%s", msg.c_str());
  webServer.send(err == ESP_OK ? 200 : 500, "text/plain", msg);
}

// -----------------------------------------------------------------------------
// HTTP /jobs - dump JobRecord ring
//   /jobs            -> JSON (default)
//   /jobs?fmt=html   -> human table with xxd-style hex dump of first 64 bytes
// -----------------------------------------------------------------------------

static String hexDumpHtml(const uint8_t *buf, size_t len)
{
  // xxd-style: 16 bytes per line, "OFFSET  HH HH ...  |ASCII|"
  String out;
  out.reserve(len * 6 + 16);
  char line[128];
  for (size_t off = 0; off < len; off += 16)
  {
    size_t n = (len - off > 16) ? 16 : (len - off);
    int p = snprintf(line, sizeof(line), "%04x  ", (unsigned)off);
    for (size_t i = 0; i < 16; i++)
    {
      if (i < n) p += snprintf(line + p, sizeof(line) - p, "%02x ", buf[off + i]);
      else       p += snprintf(line + p, sizeof(line) - p, "   ");
      if (i == 7) p += snprintf(line + p, sizeof(line) - p, " ");
    }
    p += snprintf(line + p, sizeof(line) - p, " |");
    for (size_t i = 0; i < n; i++)
    {
      uint8_t c = buf[off + i];
      char    ch = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
      // Escape HTML-sensitive chars.
      if      (ch == '<') p += snprintf(line + p, sizeof(line) - p, "&lt;");
      else if (ch == '>') p += snprintf(line + p, sizeof(line) - p, "&gt;");
      else if (ch == '&') p += snprintf(line + p, sizeof(line) - p, "&amp;");
      else                line[p++] = ch;
    }
    line[p++] = '|';
    line[p++] = '\n';
    line[p]   = '\0';
    out += line;
  }
  return out;
}

static String jsonEscape(const char *s)
{
  String out;
  out.reserve(strlen(s) + 2);
  for (const char *p = s; *p; p++)
  {
    char c = *p;
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  return out;
}

static String hexShort(const uint8_t *buf, size_t len)
{
  String out;
  out.reserve(len * 2);
  char b[4];
  for (size_t i = 0; i < len; i++)
  {
    snprintf(b, sizeof(b), "%02x", buf[i]);
    out += b;
  }
  return out;
}

static void handleJobs()
{
  JobRecord arr[16];
  size_t n = jobLogSnapshot(arr, 16);
  bool   html = (webServer.arg("fmt") == "html");
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

  if (html)
  {
    String body = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    body += "<title>Jobs - ESP32 Print Bridge</title>";
    body += "<style>body{font-family:-apple-system,sans-serif;max-width:1100px;margin:1.5em auto;padding:0 1em;color:#222}";
    body += "h2{margin-top:1.6em}.job{border:1px solid #ddd;border-radius:6px;padding:.8em 1em;margin:1em 0;background:#fafafa}";
    body += "table.kv td{padding:2px 10px;vertical-align:top}table.kv td:first-child{color:#666;width:11em}";
    body += "pre{background:#0c0c0c;color:#d0d0d0;padding:.7em;border-radius:4px;overflow:auto;font-size:12px;line-height:1.35}";
    body += ".ok{color:#0a0;font-weight:bold}.bad{color:#a00;font-weight:bold}";
    body += "</style></head><body>";
    body += "<h1>Recent print jobs</h1>";
    body += "<p><a href='/'>&larr; status</a> &middot; <a href='/tail'>/tail</a> &middot; <code>curl /jobs | jq</code></p>";
    if (n == 0)
    {
      body += "<p>(no jobs recorded yet)</p>";
    }
    for (size_t i = 0; i < n; i++)
    {
      const JobRecord &j = arr[i];
      uint32_t agoMs = now - (j.end_ms ? j.end_ms : j.start_ms);
      uint32_t durMs = (j.end_ms ? j.end_ms : now) - j.start_ms;
      bool inEqOut = (j.bytes_in == j.bytes_out);
      bool sigGood = (j.sig == SIG_HP_UEL_PJL_ZJS);

      body += "<div class='job'><h2>#" + String((unsigned)j.id) + " from " + String(j.peer_ip) + "</h2>";
      body += "<table class='kv'>";
      body += "<tr><td>When</td><td>" + String((unsigned)(agoMs / 1000)) + "s ago &middot; lasted " +
              String((unsigned)durMs) + " ms</td></tr>";
      body += "<tr><td>Close</td><td>" + String(jobCloseReasonName(j.close_reason)) + "</td></tr>";
      body += "<tr><td>Signature</td><td><span class='";
      body += sigGood ? "ok'>" : "bad'>";
      body += String(jobSigName(j.sig)) + "</span>";
      if (!sigGood && j.sig != SIG_UNKNOWN)
      {
        body += " &mdash; likely wrong driver on the host";
      }
      body += "</td></tr>";
      body += "<tr><td>bytes_in / out / dropped</td><td>" +
              String((unsigned)j.bytes_in) + " / " +
              String((unsigned)j.bytes_out) + " / " +
              String((unsigned)j.bytes_dropped);
      body += inEqOut ? " <span class='ok'>(in==out)</span>"
                      : " <span class='bad'>(in!=out)</span>";
      body += "</td></tr>";
      body += "<tr><td>USB err / timeouts</td><td>" +
              String((unsigned)j.usb_err_count) + " / " +
              String((unsigned)j.usb_timeout_count);
      if (j.last_usb_err)
      {
        body += " (last 0x" + String((unsigned)j.last_usb_err, HEX) + ")";
      }
      body += "</td></tr>";
      body += "<tr><td>Stream peak</td><td>" + String((unsigned)j.peak_stream_used) + " bytes</td></tr>";
      body += "</table>";

      if (j.first64_len > 0)
      {
        body += "<p>First " + String((unsigned)j.first64_len) + " bytes:</p><pre>";
        body += hexDumpHtml(j.first64, j.first64_len);
        body += "</pre>";
      }
      body += "</div>";
    }
    body += "</body></html>";
    webServer.send(200, "text/html; charset=utf-8", body);
    return;
  }

  // --- JSON ---
  String body = "[";
  for (size_t i = 0; i < n; i++)
  {
    const JobRecord &j = arr[i];
    if (i) body += ",";
    body += "{";
    body += "\"id\":"               + String((unsigned)j.id);
    body += ",\"peer\":\""          + jsonEscape(j.peer_ip) + "\"";
    body += ",\"start_ms\":"        + String((unsigned)j.start_ms);
    body += ",\"end_ms\":"          + String((unsigned)j.end_ms);
    body += ",\"duration_ms\":"     + String((unsigned)((j.end_ms ? j.end_ms : now) - j.start_ms));
    body += ",\"bytes_in\":"        + String((unsigned)j.bytes_in);
    body += ",\"bytes_out\":"       + String((unsigned)j.bytes_out);
    body += ",\"bytes_dropped\":"   + String((unsigned)j.bytes_dropped);
    body += ",\"usb_err_count\":"   + String((unsigned)j.usb_err_count);
    body += ",\"usb_timeout_count\":" + String((unsigned)j.usb_timeout_count);
    body += ",\"last_usb_err\":\"0x" + String((unsigned)j.last_usb_err, HEX) + "\"";
    body += ",\"peak_stream_used\":" + String((unsigned)j.peak_stream_used);
    body += ",\"close_reason\":\""  + String(jobCloseReasonName(j.close_reason)) + "\"";
    body += ",\"sig\":\""           + String(jobSigName(j.sig)) + "\"";
    body += ",\"first64_hex\":\""   + hexShort(j.first64, j.first64_len) + "\"";
    body += ",\"first64_len\":"     + String((unsigned)j.first64_len);
    body += "}";
  }
  body += "]";
  webServer.send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// HTTP /tail  /tail.txt - return last N bytes of the in-memory log ring
//
// Both endpoints now serve plain text.  The previous HTML wrapper used
// String += in a tight loop to do HTML escaping, which reallocated the heap
// repeatedly and could exhaust internal SRAM under fast refreshes, crashing
// the device.  Browsers happily render text/plain in a fixed-width font,
// so we don't lose much by dropping the HTML chrome.  Live tail (with auto
// scroll) is the job of `nc <ip> 9101`.
// -----------------------------------------------------------------------------

static void serveTail(const char *mime)
{
  size_t want = 8192;
  if (webServer.hasArg("n"))
  {
    long v = webServer.arg("n").toInt();
    if (v > 0 && v <= 16384) want = (size_t)v;
  }
  // Stack-friendly: 16KB is the cap, matches the ring size.
  static char buf[16384];
  size_t got = logSinkSnapshot(buf, want);
  webServer.send_P(200, mime, buf, got);
}

static void handleTail()    { serveTail("text/plain; charset=utf-8"); }
static void handleTailTxt() { serveTail("text/plain; charset=utf-8"); }

// -----------------------------------------------------------------------------
// TCP :9101 telnet log task - streams log ring contents to a single client.
// New connections kick the old one.  Non-blocking writes; slow clients are
// dropped rather than back-pressuring the producer.
//
// Same POSIX socket reasoning as rawServerTask: WiFiServer is unreliable in
// this build, so we go straight to lwIP.
// -----------------------------------------------------------------------------

static void tcpLogTask(void *arg)
{
  logTeeln("[log9101] task start");

  int lfd = -1;
  while (lfd < 0)
  {
    lfd = openListenSocket(9101, 1);
    if (lfd < 0) vTaskDelay(pdMS_TO_TICKS(500));
  }
  // Listening socket is non-blocking so the loop can also tend the client
  // without sleeping on accept.
  int lflags = fcntl(lfd, F_GETFL, 0);
  fcntl(lfd, F_SETFL, lflags | O_NONBLOCK);
  logTee("[log9101] listening on :9101 (fd=%d)\n", lfd);

  int      cfd    = -1;
  uint64_t cursor = 0;
  char     chunk[1024];

  for (;;)
  {
    // Accept new connection (non-blocking), kicking any existing one.
    struct sockaddr_in peer = {};
    socklen_t          peerLen = sizeof(peer);
    int newFd = ::accept(lfd, (struct sockaddr *)&peer, &peerLen);
    if (newFd >= 0)
    {
      if (cfd >= 0)
      {
        const char msg[] = "\n[log9101] superseded by new connection\n";
        ::send(cfd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        ::close(cfd);
      }
      cfd = newFd;

      // Configure new client: TCP_NODELAY + non-blocking sends.
      int yes = 1;
      setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
      int cflags = fcntl(cfd, F_GETFL, 0);
      fcntl(cfd, F_SETFL, cflags | O_NONBLOCK);

      char peerIp[16] = {0};
      inet_ntoa_r(peer.sin_addr, peerIp, sizeof(peerIp));
      logTee("[log9101] viewer connected from %s\n", peerIp);

      // Send a snapshot of recent log so the viewer sees context.
      size_t snap = logSinkSnapshot(chunk, sizeof(chunk));
      if (snap > 0)
      {
        ::send(cfd, chunk, snap, 0);
      }
      cursor = logSinkWriteCursor();
      const char banner[] = "\n--- live log (ring tail above; new lines below) ---\n";
      ::send(cfd, banner, sizeof(banner) - 1, 0);
    }

    if (cfd >= 0)
    {
      // Push any new bytes from the ring.
      size_t n = logSinkReadFromCursor(&cursor, chunk, sizeof(chunk));
      if (n > 0)
      {
        ssize_t w = ::send(cfd, chunk, n, MSG_DONTWAIT);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          // Slow / dead client: drop it.
          logTee("[log9101] viewer dropped errno=%d\n", errno);
    // Stop the back-channel from writing to a closing fd, then close.
    s_rawClientFd = -1;
    ::close(cfd);
          cfd = -1;
        }
      }
      // Discard anything the client sends to us; also detect FIN.
      uint8_t scratch[64];
      ssize_t r = ::recv(cfd, scratch, sizeof(scratch), MSG_DONTWAIT);
      if (r == 0)
      {
        logTeeln("[log9101] viewer closed");
        ::close(cfd);
        cfd = -1;
      }
      // r < 0 with EAGAIN is normal; ignore.
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}

// -----------------------------------------------------------------------------
// Wi-Fi / mDNS / setup / loop
// -----------------------------------------------------------------------------

static void startWifi()
{
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

static void startMdns()
{
  if (!MDNS.begin(kMdnsHostname))
  {
    logTeeln("mDNS begin failed");
    return;
  }
  logTee("mDNS: %s.local\n", kMdnsHostname);

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
  delay(2);
}
