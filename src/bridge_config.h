// =============================================================================
//  bridge_config: compile-time configuration constants shared across modules.
//
//  These were previously file-scope `static const` / `static constexpr` values
//  at the top of main.cpp.  They are pure constants (each translation unit gets
//  its own copy; the compiler folds them), so moving them into a shared header
//  changes nothing about behaviour.
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD / MDNS_HOSTNAME / PRINTER_FRIENDLY_NAME

// -----------------------------------------------------------------------------
// Wi-Fi / mDNS / network identity
// -----------------------------------------------------------------------------

extern const char    * const kSsid;
extern const char    * const kPassword;
extern const char    * const kMdnsHostname;
extern const char    * const kPrinterName;
extern const uint16_t       kRawPort;

// -----------------------------------------------------------------------------
// USB Printer Class identifiers (USB_CLASS_PRINTER == 0x07 already defined in
// usb/usb_types_ch9.h):
//   subclass 0x01 = printer
//   protocol 0x01 = unidirectional, 0x02 = bidirectional, 0x03 = IEEE 1284.4
// -----------------------------------------------------------------------------

// Class-specific requests (USB Printer Class spec)
static constexpr uint8_t  PRINTER_REQ_GET_DEVICE_ID    = 0x00;
static constexpr uint8_t  PRINTER_REQ_GET_PORT_STATUS  = 0x01;
static constexpr uint8_t  PRINTER_REQ_SOFT_RESET       = 0x02;

// bmRequestType helpers
static constexpr uint8_t  REQ_TYPE_CLASS_IFACE_IN  = 0xA1; // 1010 0001
static constexpr uint8_t  REQ_TYPE_CLASS_IFACE_OUT = 0x21; // 0010 0001

// -----------------------------------------------------------------------------
// Buffering / chunking
// -----------------------------------------------------------------------------

// Stream buffer used to pipe bytes from the TCP receiver task to the
// USB writer task.  Allocated in PSRAM (large) to absorb bursts.
static const size_t kStreamBufBytes = 256 * 1024;

// Per-USB-write chunk size.  M1136 USB descriptor reports wMaxPacketSize=64
// (Full Speed). We pack multiple packets into a single transfer.
static const size_t kUsbChunkBytes  = 1024;

// Back-channel ring buffer: printer Bulk IN -> this buffer -> TCP client.
static const size_t kBackBufBytes = 16 * 1024;

// -----------------------------------------------------------------------------
// Timing / thresholds
// -----------------------------------------------------------------------------

static const uint32_t kDrainQuiescentMs = 1500;
static const uint32_t kFinWaitMs = 8000;
// How long to wait for the printer to become ready (USB attached) when TCP
// data already arrived but no device is present yet.
static const uint32_t kPrinterReadyWaitMs = 5000;

// Consecutive USB bulk-OUT error count beyond which the raw server abandons
// the job.
static const uint32_t kMaxConsecutiveUsbErrors = 5;

// Bulk IN endpoint error streak threshold for auto-recovery (Bulk IN is
// currently disabled, but the constant is retained for when it is restored).
static const uint32_t kInErrorRecoveryThreshold = 10;

// Port status sentinel: 0xFF means "we tried to read but the printer didn't
// answer (timeout / stall)" - this is common on hub-attached HP M1132/M1136
// printers that don't implement class-specific control requests reliably.
// The real-world Port Status byte (USB Printer Class 1.1 §5.4) only ever
// uses bits 3, 4, 5; 0xFF cannot occur as a genuine status.
static constexpr uint8_t kPortStatusUnknown = 0xFF;
