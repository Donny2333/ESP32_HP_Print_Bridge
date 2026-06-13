// =============================================================================
//  log_sink: in-memory ring buffer that captures everything written via the
//  ESP-IDF logging facilities (ESP_LOGx) and Arduino's Serial printf.
//
//  Purpose:
//    - Allow a remote viewer (TCP :9101 telnet, HTTP /tail) to see the same
//      log stream that goes out to USB-Serial-JTAG, without requiring a USB
//      cable attached during normal operation.
//
//  Properties:
//    - 8 KB ring, internal SRAM
//    - Lock-protected with portMUX (FreeRTOS critical section); calls are
//      cheap and safe from any task context (NOT from ISR)
//    - Writes never block, never allocate: a full ring drops oldest bytes
//    - Read side is poll-based (consumeFrom) so a stalled consumer cannot
//      back-pressure the rest of the system
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tee printf-style helper: writes formatted text to BOTH the local Serial
// console (USB-Serial-JTAG) and the in-memory log ring.  Use this in place
// of Serial.printf / Serial.println / Serial.print so that remote viewers
// (telnet :9101, HTTP /tail) see the same stream.
//
// Lines should normally end with '\n'.  No automatic newline is added.
//
// Behaviour:
//   - Local Serial output is byte-identical to what Serial.printf would have
//     written, so your USB-Serial-JTAG monitor is unaffected.
//   - The ring copy is prefixed with "[<millis>] " so remote viewers can
//     correlate events in time without distinct timestamps in source.
//   - Never blocks, never allocates beyond a 256-byte stack buffer.
void logTee(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Same as logTee but appends '\n'.  Drop-in replacement for Serial.println.
void logTeeln(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Print just a newline (drop-in for bare Serial.println()).
void logTeeNewline();

// Initialise the sink and install the vprintf hook.  Safe to call once at
// boot, very early (before Wi-Fi / USB).  Idempotent.
void logSinkInit();

// Append raw bytes to the ring.  Intended for code paths that already format
// into a buffer (e.g. telnet acceptor banners).  Drops oldest bytes if full.
// Never blocks.
void logSinkWrite(const char *buf, size_t len);

// Snapshot the most recent up to `maxLen` bytes from the ring into `out`.
// Returns number of bytes written.  Does not advance any cursor.  Used by
// HTTP /tail to give a one-shot dump.
size_t logSinkSnapshot(char *out, size_t maxLen);

// Read-cursor API for streaming consumers (telnet task):
//   - getWriteCursor()       absolute write cursor (monotonic, wraps at 64-bit)
//   - readFromCursor(cur,..) copies bytes produced since `*cur` and advances it
//
// If the consumer falls more than the ring size behind, it will skip ahead
// to the oldest still-valid byte.  No back-pressure on producers.
uint64_t logSinkWriteCursor();
size_t   logSinkReadFromCursor(uint64_t *cursor, char *out, size_t maxLen);

#ifdef __cplusplus
}
#endif
