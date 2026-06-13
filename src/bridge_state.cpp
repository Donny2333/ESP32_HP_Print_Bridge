// =============================================================================
//  bridge_state: single definition of the cross-module shared globals declared
//  in bridge_state.h.  Initial values are identical to the originals in
//  main.cpp.
// =============================================================================

#include "bridge_state.h"

#include <WiFi.h>

#include "bridge_config.h"   // kPortStatusUnknown

UsbState s_usb = {};

SemaphoreHandle_t   s_usbSubmitMux = nullptr;
StreamBufferHandle_t s_stream      = nullptr;

volatile uint32_t s_totalBytesIn      = 0;
volatile uint32_t s_totalBytesOut     = 0;
volatile uint32_t s_totalBytesBack    = 0;
volatile uint32_t s_totalBackToClient = 0;
volatile uint32_t s_jobsAccepted      = 0;
volatile uint32_t s_jobsDropped       = 0;
volatile uint32_t s_lastJobBytes      = 0;

volatile bool     s_printerReady       = false;
char              s_printerIdent[160]  = "(no printer)";
char              s_deviceId[256]      = "";
volatile uint8_t  s_portStatus         = kPortStatusUnknown;

volatile uint32_t s_lastUsbWriteMs       = 0;
volatile uint32_t s_usbConsecutiveErrors = 0;

String macToString()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}
