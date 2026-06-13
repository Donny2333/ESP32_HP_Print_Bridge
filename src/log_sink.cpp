// =============================================================================
//  log_sink implementation
// =============================================================================

#include "log_sink.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr size_t kRingBytes = 16 * 1024;

// Storage for the ring.  Defined as a regular static so it lives in the
// regular BSS (internal SRAM) - we deliberately avoid PSRAM here so that
// writes from ESP_LOG hooks remain cheap.
uint8_t        s_ring[kRingBytes];

// Monotonic byte counter.  We never reset it: the position inside the ring
// is `cursor % kRingBytes`.  64-bit wrap is effectively never reached.
volatile uint64_t s_writeCursor = 0;

portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;

// Captured original vprintf so we can chain to it (keep USB-Serial-JTAG
// logs working).
vprintf_like_t s_origVprintf = nullptr;

bool           s_initialised  = false;

// Append into the ring with mux held.  Never blocks, never fails.
void IRAM_ATTR appendLocked(const char *buf, size_t len)
{
  if (len == 0) return;

  // If `len` is larger than the ring, only the last kRingBytes are useful.
  if (len > kRingBytes)
  {
    buf += (len - kRingBytes);
    len  = kRingBytes;
  }

  size_t pos = (size_t)(s_writeCursor % kRingBytes);
  size_t first = kRingBytes - pos;
  if (first > len) first = len;
  memcpy(s_ring + pos, buf, first);

  size_t rest = len - first;
  if (rest > 0)
  {
    memcpy(s_ring, buf + first, rest);
  }

  s_writeCursor += len;
}

// vprintf hook: format into a stack buffer (truncating long lines), then
// fan out to (a) the original sink so USB-Serial-JTAG keeps working, and
// (b) our ring for remote viewers.
int IRAM_ATTR sinkVprintf(const char *fmt, va_list ap)
{
  // Chain to the original first so the local console output happens
  // exactly when it would have without our hook.
  va_list ap2;
  va_copy(ap2, ap);
  int origRet = 0;
  if (s_origVprintf)
  {
    origRet = s_origVprintf(fmt, ap2);
  }
  va_end(ap2);

  // Now format our own copy into a local buffer to feed the ring.
  char   buf[256];
  int    n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0)
  {
    return origRet ? origRet : n;
  }
  size_t take = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

  portENTER_CRITICAL(&s_mux);
  appendLocked(buf, take);
  portEXIT_CRITICAL(&s_mux);

  return origRet ? origRet : n;
}

} // namespace

extern "C" void logSinkInit()
{
  if (s_initialised) return;
  s_initialised = true;

  // Install our vprintf hook.  esp_log_set_vprintf returns the previous
  // function pointer; remember it so we can chain.
  s_origVprintf = esp_log_set_vprintf(sinkVprintf);
}

extern "C" void logSinkWrite(const char *buf, size_t len)
{
  if (!buf || len == 0) return;

  portENTER_CRITICAL(&s_mux);
  appendLocked(buf, len);
  portEXIT_CRITICAL(&s_mux);
}

// We forward-declare a weak link to Arduino's Serial.write so this file can
// build with or without Arduino present.  In practice the project always
// builds with Arduino-as-IDF-component, so the symbol resolves at link time.
#include <Arduino.h>

namespace {

// Helper used by all logTee* variants: write `body` of length `bodyLen` to
// Serial as-is, then write a "[<ms>] " timestamped copy to the ring.  This
// keeps local USB-Serial-JTAG output byte-identical to the original
// Serial.printf behaviour while giving remote viewers easy chronology.
inline void teeBodyToBoth(const char *body, size_t bodyLen)
{
  if (bodyLen == 0) return;

  // Local console.
  Serial.write((const uint8_t *)body, bodyLen);

  // Ring: prefix every "logical line" with [<ms>].  We only prefix at the
  // start of the formatted chunk (cheap, mostly correct for one-call-per-
  // line code which is how the rest of the project is written).
  char     stamp[16];
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  int      slen = snprintf(stamp, sizeof(stamp), "[%lu] ", (unsigned long)ms);
  if (slen < 0) slen = 0;
  if ((size_t)slen > sizeof(stamp) - 1) slen = sizeof(stamp) - 1;

  portENTER_CRITICAL(&s_mux);
  appendLocked(stamp, (size_t)slen);
  appendLocked(body, bodyLen);
  portEXIT_CRITICAL(&s_mux);
}

} // namespace

extern "C" void logTee(const char *fmt, ...)
{
  char    buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  size_t take = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
  teeBodyToBoth(buf, take);
}

extern "C" void logTeeln(const char *fmt, ...)
{
  char    buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);  // leave room for '\n'
  va_end(ap);
  if (n < 0) return;
  size_t take = (n < (int)sizeof(buf) - 1) ? (size_t)n : sizeof(buf) - 2;
  buf[take++] = '\n';
  teeBodyToBoth(buf, take);
}

extern "C" void logTeeNewline()
{
  static const char nl = '\n';
  teeBodyToBoth(&nl, 1);
}

extern "C" size_t logSinkSnapshot(char *out, size_t maxLen)
{
  if (!out || maxLen == 0) return 0;

  portENTER_CRITICAL(&s_mux);
  uint64_t wc = s_writeCursor;
  size_t   have = (wc < kRingBytes) ? (size_t)wc : kRingBytes;
  size_t   take = (have < maxLen) ? have : maxLen;

  // Copy the last `take` bytes ending at wc.
  uint64_t startCur = wc - take;
  size_t   pos      = (size_t)(startCur % kRingBytes);
  size_t   first    = kRingBytes - pos;
  if (first > take) first = take;
  memcpy(out, s_ring + pos, first);
  size_t rest = take - first;
  if (rest > 0)
  {
    memcpy(out + first, s_ring, rest);
  }
  portEXIT_CRITICAL(&s_mux);

  return take;
}

extern "C" uint64_t logSinkWriteCursor()
{
  portENTER_CRITICAL(&s_mux);
  uint64_t wc = s_writeCursor;
  portEXIT_CRITICAL(&s_mux);
  return wc;
}

extern "C" size_t logSinkReadFromCursor(uint64_t *cursor, char *out, size_t maxLen)
{
  if (!cursor || !out || maxLen == 0) return 0;

  portENTER_CRITICAL(&s_mux);
  uint64_t wc = s_writeCursor;
  uint64_t rc = *cursor;

  // Consumer ahead of producer (initial state where cursor==0 is fine).
  if (rc > wc) rc = wc;

  uint64_t behind = wc - rc;
  // If consumer fell more than ring size behind, skip ahead to the oldest
  // still-valid byte.
  if (behind > kRingBytes)
  {
    rc     = wc - kRingBytes;
    behind = kRingBytes;
  }

  size_t take = (behind < maxLen) ? (size_t)behind : maxLen;
  if (take == 0)
  {
    portEXIT_CRITICAL(&s_mux);
    *cursor = wc;
    return 0;
  }

  size_t pos   = (size_t)(rc % kRingBytes);
  size_t first = kRingBytes - pos;
  if (first > take) first = take;
  memcpy(out, s_ring + pos, first);
  size_t rest = take - first;
  if (rest > 0)
  {
    memcpy(out + first, s_ring, rest);
  }

  *cursor = rc + take;
  portEXIT_CRITICAL(&s_mux);
  return take;
}
