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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include "esp_heap_caps.h"
#include "usb/usb_host.h"

#include "secrets.h"

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

// How long to wait for the printer to become ready (USB attached) when TCP
// data already arrived but no device is present yet.
static const uint32_t kPrinterReadyWaitMs = 5000;

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

WebServer  webServer(80);
WiFiServer rawServer(kRawPort);

static StreamBufferHandle_t s_stream = nullptr;
static uint8_t              *s_streamStorage = nullptr;
static StaticStreamBuffer_t s_streamCb;

// Statistics
static volatile uint32_t s_totalBytesIn  = 0;   // received over TCP
static volatile uint32_t s_totalBytesOut = 0;   // written to USB
static volatile uint32_t s_totalBytesBack= 0;   // read from USB IN
static volatile uint32_t s_jobsAccepted  = 0;
static volatile uint32_t s_jobsDropped   = 0;
static volatile uint32_t s_lastJobBytes  = 0;
static volatile bool     s_printerReady  = false;
static char              s_printerIdent[160] = "(no printer)";
static char              s_deviceId[256]     = "";   // IEEE 1284 device ID string
static volatile uint8_t  s_portStatus        = 0x00;

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

// -----------------------------------------------------------------------------
// USB host: library task (drains library-level events)
// -----------------------------------------------------------------------------

static void usbHostLibTask(void *arg)
{
  Serial.println("[usb-lib] task start");
  for (;;)
  {
    uint32_t flags = 0;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
    {
      Serial.printf("[usb-lib] handle_events err=0x%x\n", err);
    }

    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
    {
      Serial.println("[usb-lib] no clients flag");
    }
    if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
    {
      Serial.println("[usb-lib] all devices free flag");
    }
  }
}

// -----------------------------------------------------------------------------
// USB host: client event callback + client task
// -----------------------------------------------------------------------------

static void clientEventCb(const usb_host_client_event_msg_t *msg, void *arg)
{
  switch (msg->event)
  {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      Serial.printf("[usb-cli] NEW_DEV addr=%u\n", msg->new_dev.address);
      if (!s_usb.claimed)
      {
        usbTryAttachDevice(msg->new_dev.address);
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.println("[usb-cli] DEV_GONE");
      usbDetach();
      break;
    default:
      break;
  }
}

static void usbClientTask(void *arg)
{
  Serial.println("[usb-cli] task start");

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous   = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = clientEventCb;
  cfg.async.callback_arg          = nullptr;

  esp_err_t err = usb_host_client_register(&cfg, &s_usb.client);
  if (err != ESP_OK)
  {
    Serial.printf("[usb-cli] register err=0x%x\n", err);
    vTaskDelete(nullptr);
    return;
  }

  for (;;)
  {
    err = usb_host_client_handle_events(s_usb.client, pdMS_TO_TICKS(1000));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
    {
      Serial.printf("[usb-cli] handle_events err=0x%x\n", err);
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
      Serial.printf("[usb-cli]  intf #%u alt=%u class=0x%02X sub=0x%02X proto=0x%02X eps=%u\n",
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
      Serial.printf("[usb-cli]   ep 0x%02X (%s) attr=0x%02X mps=%u\n",
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
    Serial.printf("[usb-cli] device_open(%u) err=0x%x\n", devAddr, err);
    xSemaphoreGive(s_usb.lock);
    return false;
  }

  const usb_device_desc_t *ddesc = nullptr;
  if (usb_host_get_device_descriptor(dev, &ddesc) == ESP_OK && ddesc)
  {
    Serial.printf("[usb-cli] device VID=0x%04X PID=0x%04X class=0x%02X bcdUSB=0x%04X\n",
                  ddesc->idVendor, ddesc->idProduct, ddesc->bDeviceClass, ddesc->bcdUSB);
    snprintf(s_printerIdent, sizeof(s_printerIdent),
             "VID=0x%04X PID=0x%04X", ddesc->idVendor, ddesc->idProduct);
  }

  const usb_config_desc_t *cdesc = nullptr;
  if (usb_host_get_active_config_descriptor(dev, &cdesc) != ESP_OK || !cdesc)
  {
    Serial.println("[usb-cli] cannot read config descriptor");
    usb_host_device_close(s_usb.client, dev);
    xSemaphoreGive(s_usb.lock);
    return false;
  }
  Serial.printf("[usb-cli] config #%u, %u interfaces, %u bytes\n",
                cdesc->bConfigurationValue, cdesc->bNumInterfaces, cdesc->wTotalLength);

  uint8_t  ifNum = 0, epOut = 0, epIn = 0;
  uint16_t epOutMps = 0, epInMps = 0;
  if (!descFindPrinterInterface(cdesc, ifNum, epOut, epOutMps, epIn, epInMps))
  {
    Serial.println("[usb-cli] no Printer Class interface with bulk OUT found");
    usb_host_device_close(s_usb.client, dev);
    xSemaphoreGive(s_usb.lock);
    return false;
  }

  Serial.printf("[usb-cli] claiming if=%u epOut=0x%02X mps=%u epIn=0x%02X mps=%u\n",
                ifNum, epOut, epOutMps, epIn, epInMps);
  err = usb_host_interface_claim(s_usb.client, dev, ifNum, 0);
  if (err != ESP_OK)
  {
    Serial.printf("[usb-cli] interface_claim err=0x%x\n", err);
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
  s_printerReady   = true;

  xSemaphoreGive(s_usb.lock);

  // Best-effort: query IEEE 1284 device ID string for nice logging / status page.
  s_deviceId[0] = '\0';
  if (usbCtrlGetDeviceId(s_deviceId, sizeof(s_deviceId)) == ESP_OK && s_deviceId[0])
  {
    Serial.printf("[usb-cli] Device ID: %s\n", s_deviceId);
    // Use device ID as the friendly identifier (truncate to fit)
    size_t identMax = sizeof(s_printerIdent) - 1;
    strncpy(s_printerIdent, s_deviceId, identMax);
    s_printerIdent[identMax] = '\0';
  }
  else
  {
    Serial.println("[usb-cli] GET_DEVICE_ID failed (printer may still work)");
  }

  uint8_t st = 0;
  if (usbCtrlGetPortStatus(&st) == ESP_OK)
  {
    s_portStatus = st;
    Serial.printf("[usb-cli] Port status: 0x%02X\n", st);
  }

  Serial.println("[usb-cli] printer attached and ready");
  return true;
}

static void usbDetach()
{
  xSemaphoreTake(s_usb.lock, portMAX_DELAY);
  if (s_usb.claimed)
  {
    Serial.println("[usb-cli] detaching");
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
  s_printerReady = false;
  s_deviceId[0]  = '\0';
  s_portStatus   = 0x00;
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
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeoutMs + 200)) != pdTRUE)
    {
      err = ESP_ERR_TIMEOUT;
    }
    else
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
  uint16_t wIndex = ((uint16_t)s_usb.ifNumber);
  uint8_t st = 0;
  int actual = 0;
  esp_err_t err = usbCtrlSync(REQ_TYPE_CLASS_IFACE_IN,
                              PRINTER_REQ_GET_PORT_STATUS,
                              0, wIndex, 1, &st, &actual, 800);
  if (err == ESP_OK && actual >= 1) *status = st;
  return err;
}

static esp_err_t usbCtrlSoftReset()
{
  uint16_t wIndex = ((uint16_t)s_usb.ifNumber);
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

  err = usb_host_transfer_submit(xfer);
  if (err == ESP_OK)
  {
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeoutMs + 200)) != pdTRUE)
    {
      Serial.println("[usb-out] transfer timeout (semaphore)");
      err = ESP_ERR_TIMEOUT;
    }
    else
    {
      err = ctx.status;
    }
  }
  else
  {
    Serial.printf("[usb-out] submit err=0x%x\n", err);
  }

  vSemaphoreDelete(ctx.done);
  usb_host_transfer_free(xfer);
  return err;
}

static void usbWriterTask(void *arg)
{
  Serial.println("[usb-out] task start");
  static uint8_t chunk[kUsbChunkBytes];

  for (;;)
  {
    size_t got = xStreamBufferReceive(s_stream, chunk, sizeof(chunk),
                                      pdMS_TO_TICKS(500));
    if (got == 0) continue;

    // If printer disappeared mid-job, wait briefly then drop remaining bytes.
    if (!s_printerReady)
    {
      Serial.printf("[usb-out] printer not ready, waiting up to %u ms\n",
                    (unsigned)kPrinterReadyWaitMs);
      if (!waitForPrinter(kPrinterReadyWaitMs))
      {
        Serial.printf("[usb-out] still no printer, discarding %u bytes\n", (unsigned)got);
        // Drain remaining buffered data too so we don't loop forever
        size_t drained = 0;
        while (xStreamBufferReceive(s_stream, chunk, sizeof(chunk), 0) > 0) drained += sizeof(chunk);
        if (drained) Serial.printf("[usb-out] drained extra %u bytes\n", (unsigned)drained);
        continue;
      }
    }

    size_t off = 0;
    while (off < got)
    {
      size_t n = got - off;
      if (n > kUsbChunkBytes) n = kUsbChunkBytes;
      esp_err_t err = usbBulkOutSync(chunk + off, n, 8000);
      if (err != ESP_OK)
      {
        Serial.printf("[usb-out] bulk write err=0x%x at offset=%u\n", err, (unsigned)off);
        // Try to clear stall
        if (s_usb.claimed)
        {
          usb_host_endpoint_clear(s_usb.device, s_usb.epOut);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        break;
      }
      off += n;
      s_totalBytesOut += n;
    }
  }
}

// -----------------------------------------------------------------------------
// USB host: bulk IN reader task - keeps printer-side FIFO drained
// -----------------------------------------------------------------------------

static void IRAM_ATTR readXferCb(usb_transfer_t *xfer)
{
  WriteCtx *ctx = (WriteCtx *)xfer->context;
  ctx->status = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
  ctx->actual = xfer->actual_num_bytes;
  BaseType_t hpw = pdFALSE;
  xSemaphoreGiveFromISR(ctx->done, &hpw);
  if (hpw) portYIELD_FROM_ISR();
}

static void usbReaderTask(void *arg)
{
  Serial.println("[usb-in] task start");

  for (;;)
  {
    if (!s_printerReady || s_usb.epIn == 0)
    {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    usb_transfer_t *xfer = nullptr;
    if (usb_host_transfer_alloc(kUsbInBufBytes, 0, &xfer) != ESP_OK)
    {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    WriteCtx ctx = {};
    ctx.done = xSemaphoreCreateBinary();

    xfer->num_bytes        = kUsbInBufBytes;
    xfer->device_handle    = s_usb.device;
    xfer->bEndpointAddress = s_usb.epIn;
    xfer->callback         = readXferCb;
    xfer->context          = &ctx;
    xfer->timeout_ms       = 2000;

    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err == ESP_OK)
    {
      // Wait a bit longer than the USB timeout
      if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(2500)) == pdTRUE)
      {
        if (ctx.status == ESP_OK && ctx.actual > 0)
        {
          s_totalBytesBack += ctx.actual;
          // Log a small hexdump of the first response only - some printers
          // return ASCII status strings here.
          char preview[80];
          size_t plen = ctx.actual < 32 ? ctx.actual : 32;
          for (size_t i = 0; i < plen; i++)
          {
            uint8_t c = xfer->data_buffer[i];
            preview[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
          }
          preview[plen] = '\0';
          Serial.printf("[usb-in] %u bytes: %s\n", (unsigned)ctx.actual, preview);
        }
      }
    }

    vSemaphoreDelete(ctx.done);
    usb_host_transfer_free(xfer);

    // Tiny gap to prevent monopolising the bus
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// -----------------------------------------------------------------------------
// TCP raw server (port 9100)
// -----------------------------------------------------------------------------

static void rawServerTask(void *arg)
{
  Serial.println("[raw9100] task start");
  rawServer.setNoDelay(true);

  uint8_t buf[1460];

  for (;;)
  {
    WiFiClient client = rawServer.accept();
    if (!client)
    {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    client.setTimeout(30000);
    Serial.printf("[raw9100] connection from %s\n", client.remoteIP().toString().c_str());

    // If no printer is attached, hold briefly then refuse the job so the
    // spooler reports failure instead of silently succeeding.
    if (!s_printerReady && !waitForPrinter(kPrinterReadyWaitMs))
    {
      Serial.println("[raw9100] no printer, dropping connection");
      s_jobsDropped++;
      client.stop();
      continue;
    }

    s_jobsAccepted++;
    s_lastJobBytes = 0;

    unsigned long lastByteAt = millis();
    while (client.connected() || client.available())
    {
      int avail = client.available();
      if (avail > 0)
      {
        int want = (int)sizeof(buf);
        if (want > avail) want = avail;
        int got = client.read(buf, want);
        if (got > 0)
        {
          s_totalBytesIn += got;
          s_lastJobBytes += got;
          // push to stream buffer; if full, throttle
          size_t off = 0;
          while ((int)off < got)
          {
            size_t sent = xStreamBufferSend(s_stream, buf + off, got - off,
                                            pdMS_TO_TICKS(5000));
            if (sent == 0)
            {
              Serial.println("[raw9100] stream buffer full, retrying");
              continue;
            }
            off += sent;
          }
          lastByteAt = millis();
        }
      }
      else
      {
        if (!client.connected())
        {
          // No more data and disconnected -> done
          break;
        }
        // Idle timeout: client connected but sent nothing for 10s
        if (millis() - lastByteAt > 10000)
        {
          Serial.println("[raw9100] idle timeout, closing");
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }

    Serial.printf("[raw9100] connection closed, job bytes=%u total in=%u out=%u\n",
                  (unsigned)s_lastJobBytes,
                  (unsigned)s_totalBytesIn,
                  (unsigned)s_totalBytesOut);
    client.stop();

    // Wait for the writer to drain the stream buffer before refreshing status.
    // This is best-effort: we just poll until the buffer is empty (or timeout).
    uint32_t drainStart = millis();
    while (xStreamBufferBytesAvailable(s_stream) > 0 && millis() - drainStart < 30000)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Refresh port status after each job (best-effort)
    if (s_printerReady)
    {
      uint8_t st = 0;
      if (usbCtrlGetPortStatus(&st) == ESP_OK) s_portStatus = st;
    }
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
          " / " + String((unsigned)s_totalBytesBack) + "</td></tr>";
  html += "<tr><td>Free heap</td><td>" + String((unsigned)esp_get_free_heap_size()) +
          " (PSRAM free " + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + ")</td></tr>";
  html += "</table>";
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
  Serial.println(msg);
  webServer.send(err == ESP_OK ? 200 : 500, "text/plain", msg);
}

// -----------------------------------------------------------------------------
// Wi-Fi / mDNS / setup / loop
// -----------------------------------------------------------------------------

static void startWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // better USB Host stability under WiFi traffic
  WiFi.begin(kSsid, kPassword);
  Serial.printf("Connecting to Wi-Fi '%s'", kSsid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print('.');
    delay(500);
    if (millis() - start > 30000)
    {
      Serial.println(" timeout, retrying...");
      WiFi.disconnect();
      delay(500);
      WiFi.begin(kSsid, kPassword);
      start = millis();
    }
  }
  Serial.println();
  Serial.printf("Wi-Fi OK, IP=%s MAC=%s\n",
                WiFi.localIP().toString().c_str(), macToString().c_str());
}

static void startMdns()
{
  if (!MDNS.begin(kMdnsHostname))
  {
    Serial.println("mDNS begin failed");
    return;
  }
  Serial.printf("mDNS: %s.local\n", kMdnsHostname);

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

  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;
  esp_err_t err = usb_host_install(&cfg);
  if (err != ESP_OK)
  {
    Serial.printf("usb_host_install err=0x%x\n", err);
    return;
  }
  Serial.println("usb_host installed");

  // Library task on core 0, priority 5
  xTaskCreatePinnedToCore(usbHostLibTask, "usb-lib", 4096, nullptr, 5, nullptr, 0);
  // Client task on core 0, priority 4
  xTaskCreatePinnedToCore(usbClientTask, "usb-cli", 4096, nullptr, 4, nullptr, 0);
  // Writer task on core 0, priority 4
  xTaskCreatePinnedToCore(usbWriterTask, "usb-out", 4096, nullptr, 4, nullptr, 0);
  // Reader task on core 0, priority 3 (lower than writer)
  xTaskCreatePinnedToCore(usbReaderTask, "usb-in",  4096, nullptr, 3, nullptr, 0);
}

// Allocate the stream buffer storage in PSRAM if available.
static bool createStreamBuffer()
{
  // Prefer PSRAM (we have 8MB), fall back to internal SRAM.
  s_streamStorage = (uint8_t *)heap_caps_malloc(kStreamBufBytes + 1, MALLOC_CAP_SPIRAM);
  if (!s_streamStorage)
  {
    Serial.println("PSRAM alloc for stream buffer failed, trying internal heap");
    s_streamStorage = (uint8_t *)heap_caps_malloc(kStreamBufBytes + 1, MALLOC_CAP_8BIT);
  }
  if (!s_streamStorage) return false;

  s_stream = xStreamBufferCreateStatic(kStreamBufBytes, 1, s_streamStorage, &s_streamCb);
  return s_stream != nullptr;
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32-S3-N16R8 USB Print Bridge ===");
  Serial.printf("PSRAM total: %u  free: %u\n",
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  if (!createStreamBuffer())
  {
    Serial.println("stream buffer alloc failed!");
    while (true) delay(1000);
  }
  Serial.printf("Stream buffer: %u bytes (PSRAM)\n", (unsigned)kStreamBufBytes);

  startWifi();
  startMdns();

  webServer.on("/", handleRoot);
  webServer.on("/reset", handleReset);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not Found"); });
  webServer.begin();

  rawServer.begin();
  rawServer.setNoDelay(true);
  Serial.printf("Listening on TCP %u (HP JetDirect / AppSocket)\n", kRawPort);

  startUsbHost();

  // Raw 9100 server runs on core 1, lower priority than USB tasks
  xTaskCreatePinnedToCore(rawServerTask, "raw9100", 6144, nullptr, 3, nullptr, 1);
}

void loop()
{
  webServer.handleClient();
  delay(2);
}
