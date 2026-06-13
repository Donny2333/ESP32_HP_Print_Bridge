// =============================================================================
//  bridge_state: shared mutable state used by more than one module.
//
//  These globals were previously file-scope `static` variables in main.cpp.
//  Because they are referenced across what are now several translation units
//  (usb_bridge / net_server / http_status / main), they must have a single
//  definition (in bridge_state.cpp) and `extern` declarations here.  No
//  behaviour changes: same variables, same initial values, same access.
//
//  Module-private state (used by only one .cpp) is NOT here - it stays file-
//  local in its owning module.
// =============================================================================

#pragma once

#include <stdint.h>

#include <Arduino.h>      // String (macToString)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "usb/usb_host.h"

// -----------------------------------------------------------------------------
// USB device/endpoint state shared between the USB tasks, the raw server and
// the HTTP status page.
// -----------------------------------------------------------------------------
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

extern UsbState s_usb;

// Mutex to serialise Bulk OUT and Bulk IN submissions on the same USB host
// client.  ESP-IDF v4.4 USB host driver is not fully re-entrant when two
// tasks submit transfers concurrently on the same device.  Created in
// startUsbHost(), taken by the USB writer.
extern SemaphoreHandle_t s_usbSubmitMux;

// TCP -> USB pipe.  Created in createStreamBuffer(); raw server writes,
// USB writer reads.
extern StreamBufferHandle_t s_stream;

// -----------------------------------------------------------------------------
// Statistics (volatile, non-atomic - same weak synchronisation as before).
// -----------------------------------------------------------------------------
extern volatile uint32_t s_totalBytesIn;       // received over TCP
extern volatile uint32_t s_totalBytesOut;      // written to USB
extern volatile uint32_t s_totalBytesBack;     // read from USB IN
extern volatile uint32_t s_totalBackToClient;  // bulk IN bytes forwarded back to TCP client
extern volatile uint32_t s_jobsAccepted;
extern volatile uint32_t s_jobsDropped;
extern volatile uint32_t s_lastJobBytes;

// -----------------------------------------------------------------------------
// Printer identity / readiness (set by USB attach, read by raw server + HTTP).
// -----------------------------------------------------------------------------
extern volatile bool     s_printerReady;
extern char              s_printerIdent[160];  // friendly identifier
extern char              s_deviceId[256];      // IEEE 1284 device ID string
extern volatile uint8_t  s_portStatus;

// Timestamp of last successful USB Bulk OUT write.  Used by the raw server to
// detect "printing finished" (USB idle) and inject synthetic status.
extern volatile uint32_t s_lastUsbWriteMs;

// Consecutive USB bulk-OUT error counter.  Reset on each successful write.
// When this exceeds kMaxConsecutiveUsbErrors, the raw server abandons the job.
extern volatile uint32_t s_usbConsecutiveErrors;

// -----------------------------------------------------------------------------
// Small shared helper.
// -----------------------------------------------------------------------------
String macToString();
