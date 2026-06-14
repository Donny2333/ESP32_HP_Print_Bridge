// =============================================================================
//  usb_bridge: ESP-IDF USB Host side of the print bridge.
//
//  Owns the USB Host library/client tasks, device attach/detach, the printer-
//  class control transfers (GET_DEVICE_ID / GET_PORT_STATUS / SOFT_RESET) and
//  the Bulk OUT writer.  Bulk IN reader / back-channel forwarder are present
//  but disabled (ESP-IDF v4.4 Bulk IN instability - see README).
//
//  Only the entry points needed by other modules are declared here; everything
//  else stays file-local in usb_bridge.cpp.
// =============================================================================

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// FreeRTOS task entry points (created by startUsbHost() in main).
void usbHostLibTask(void *arg);
void usbClientTask(void *arg);
void usbWriterTask(void *arg);
void usbReaderTask(void *arg);
void backFwdTask(void *arg);

// Block until the printer is attached (s_printerReady) or timeout.  Returns
// true if ready.  Used by the USB writer and the raw server.
bool waitForPrinter(uint32_t timeoutMs);

// Tear down the current USB device claim.  Called on DEV_GONE.
void usbDetach();

// Printer-class control transfers.  Used by the raw server (port status after
// each job) and the HTTP /reset handler.
esp_err_t usbCtrlGetPortStatus(uint8_t *status);
esp_err_t usbCtrlSoftReset();

#ifdef __cplusplus
}
#endif
