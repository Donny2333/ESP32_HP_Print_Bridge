// =============================================================================
//  usb_bridge: ESP-IDF USB Host implementation.  See usb_bridge.h.
//
//  Code moved verbatim from the original monolithic main.cpp (functions
//  epDirStr / waitForPrinter / usbHostLibTask / clientEventCb / usbClientTask /
//  descFindPrinterInterface / usbTryAttachDevice / usbDetach / ctrlXferCb /
//  usbCtrlSync / usbCtrlGetDeviceId / usbCtrlGetPortStatus / usbCtrlSoftReset /
//  writeXferCb / usbBulkOutSync / usbWriterTask / usbReaderTask / backFwdTask).
//  No logic changes.
// =============================================================================

#include "usb_bridge.h"

#include <Arduino.h>

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "usb/usb_host.h"

#include "bridge_config.h"
#include "bridge_state.h"
#include "log_sink.h"
#include "job_log.h"

// -----------------------------------------------------------------------------
// Module-private USB transfer state (used only within this file).
// -----------------------------------------------------------------------------

// Pre-allocated persistent USB OUT transfer (avoid per-chunk alloc/free)
static usb_transfer_t  *s_outXfer = nullptr;
static SemaphoreHandle_t s_outDone = nullptr;

// Bulk IN endpoint error streak counter for auto-recovery
static volatile uint32_t s_inErrorStreak = 0;

// Forward declarations for file-local helpers.
static bool usbTryAttachDevice(uint8_t devAddr);
static esp_err_t usbCtrlGetDeviceId(char *out, size_t outSz);

// -----------------------------------------------------------------------------
// Tiny helpers
// -----------------------------------------------------------------------------

static const char *epDirStr(uint8_t addr) { return (addr & 0x80) ? "IN" : "OUT"; }

// Wait until printer is attached (or timeout).  Returns true if ready.
bool waitForPrinter(uint32_t timeoutMs)
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

void usbHostLibTask(void *arg)
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

void usbClientTask(void *arg)
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

void usbDetach()
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

esp_err_t usbCtrlGetPortStatus(uint8_t *status)
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

esp_err_t usbCtrlSoftReset()
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

void usbWriterTask(void *arg)
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

void usbReaderTask(void *arg)
{
  logTeeln("[usb-in] task disabled (Bulk IN polling off for stability)");
  for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

// -----------------------------------------------------------------------------
// Back-channel forwarder task: drains s_backBuf and sends to TCP client.
// Runs on core 1 to avoid contending with USB tasks on core 0.
// -----------------------------------------------------------------------------

void backFwdTask(void *arg)
{
  logTeeln("[back-fwd] task disabled (no Bulk IN data)");
  for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
