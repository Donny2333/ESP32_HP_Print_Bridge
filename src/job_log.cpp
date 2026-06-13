// =============================================================================
//  job_log implementation
// =============================================================================

#include "job_log.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace {

constexpr size_t kJobSlots   = 16;
constexpr size_t kFirst64Cap = 64;

JobRecord       s_jobs[kJobSlots] = {};
volatile size_t s_head = 0;      // index of the most-recent job
volatile bool   s_open = false;  // is s_jobs[s_head] currently in-progress?
volatile uint32_t s_nextId = 1;

portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t nowMs()
{
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

bool startsWith(const uint8_t *buf, size_t len, const char *needle)
{
  size_t n = strlen(needle);
  if (len < n) return false;
  return memcmp(buf, needle, n) == 0;
}

bool containsAscii(const uint8_t *buf, size_t len, const char *needle)
{
  size_t n = strlen(needle);
  if (len < n) return false;
  for (size_t i = 0; i + n <= len; i++)
  {
    if (memcmp(buf + i, needle, n) == 0) return true;
  }
  return false;
}

JobSig classify(const uint8_t *buf, size_t len)
{
  if (len == 0) return SIG_UNKNOWN;

  // HP UEL: ESC % - 1 2 3 4 5 X  (1B 25 2D 31 32 33 34 35 58)
  static const uint8_t uel[] = { 0x1B, 0x25, 0x2D, 0x31, 0x32, 0x33, 0x34, 0x35, 0x58 };
  bool hasUel = (len >= sizeof(uel) && memcmp(buf, uel, sizeof(uel)) == 0);
  bool hasPjl = containsAscii(buf, len, "@PJL");
  bool hasZjs = containsAscii(buf, len, "ZJS");

  if (hasUel && hasPjl && hasZjs) return SIG_HP_UEL_PJL_ZJS;
  if (hasPjl && hasZjs)           return SIG_HP_UEL_PJL_ZJS;
  if (hasPjl)                     return SIG_PJL_NO_ZJS;

  if (startsWith(buf, len, "%!PS"))  return SIG_POSTSCRIPT;
  if (startsWith(buf, len, "%PDF"))  return SIG_PDF;
  if (startsWith(buf, len, "RaS2"))  return SIG_PWG_RASTER;
  if (startsWith(buf, len, "RaS3"))  return SIG_PWG_RASTER;

  return SIG_OTHER_BINARY;
}

} // namespace

extern "C" void jobLogInit()
{
  portENTER_CRITICAL(&s_mux);
  memset(s_jobs, 0, sizeof(s_jobs));
  s_head   = 0;
  s_open   = false;
  s_nextId = 1;
  portEXIT_CRITICAL(&s_mux);
}

extern "C" uint32_t jobBegin(const char *peer_ip)
{
  uint32_t id;
  portENTER_CRITICAL(&s_mux);

  // If the previous slot was still open (shouldn't happen normally), close
  // it abruptly so we don't lose the next one.
  if (s_open)
  {
    JobRecord &prev = s_jobs[s_head];
    if (prev.end_ms == 0) prev.end_ms = nowMs();
    if (prev.close_reason == JCR_OPEN) prev.close_reason = JCR_OTHER;
    s_open = false;
  }

  size_t next = (s_head + 1) % kJobSlots;
  if (s_nextId == 1 && s_jobs[0].id == 0)
  {
    // First-ever job: place into slot 0, don't advance s_head from 0.
    next = 0;
  }

  JobRecord &j = s_jobs[next];
  memset(&j, 0, sizeof(j));
  id              = s_nextId++;
  j.id            = id;
  j.start_ms      = nowMs();
  j.close_reason  = JCR_OPEN;
  if (peer_ip)
  {
    strncpy(j.peer_ip, peer_ip, sizeof(j.peer_ip) - 1);
  }

  s_head = next;
  s_open = true;
  portEXIT_CRITICAL(&s_mux);
  return id;
}

extern "C" void jobAppendPayload(const uint8_t *data, size_t len)
{
  if (!data || len == 0) return;

  portENTER_CRITICAL(&s_mux);
  if (s_open)
  {
    JobRecord &j = s_jobs[s_head];
    uint32_t prevBytesIn = j.bytes_in;
    j.bytes_in += (uint32_t)len;

    if (j.first64_len < kFirst64Cap)
    {
      size_t want = kFirst64Cap - j.first64_len;
      size_t take = (len < want) ? len : want;
      memcpy(j.first64 + j.first64_len, data, take);
      j.first64_len += (uint8_t)take;
    }

    // Re-classify: scan the incoming data chunk directly for signatures.
    // The first64 buffer is only 64 bytes but the ZJS token
    // ("ENTER LANGUAGE=ZJS") typically appears around byte 80-100.
    // By scanning each incoming chunk we can detect it without enlarging
    // the struct.  Use prevBytesIn (before this append) for the threshold.
    if (j.sig != SIG_HP_UEL_PJL_ZJS && prevBytesIn < 2048)
    {
      // Check the first64 buffer
      JobSig s = classify(j.first64, j.first64_len);
      if (s == SIG_HP_UEL_PJL_ZJS)
      {
        j.sig = s;
      }
      else
      {
        // Also scan the current data chunk (covers bytes beyond first64)
        bool hasPjl = containsAscii(j.first64, j.first64_len, "@PJL") ||
                      containsAscii(data, len, "@PJL");
        bool hasZjs = containsAscii(j.first64, j.first64_len, "ZJS") ||
                      containsAscii(data, len, "ZJS");
        if (hasPjl && hasZjs)
        {
          j.sig = SIG_HP_UEL_PJL_ZJS;
        }
        else if (s != SIG_UNKNOWN)
        {
          j.sig = s;  // keep best classification so far (e.g. PJL_NO_ZJS)
        }
      }
    }
  }
  portEXIT_CRITICAL(&s_mux);
}

extern "C" void jobEnd(uint8_t close_reason)
{
  portENTER_CRITICAL(&s_mux);
  if (s_open)
  {
    JobRecord &j = s_jobs[s_head];
    j.end_ms       = nowMs();
    j.close_reason = (JobCloseReason)close_reason;
    s_open = false;
  }
  portEXIT_CRITICAL(&s_mux);
}

extern "C" void jobOnUsbWrite(size_t bytes)
{
  portENTER_CRITICAL(&s_mux);
  if (s_open) s_jobs[s_head].bytes_out += (uint32_t)bytes;
  portEXIT_CRITICAL(&s_mux);
}

extern "C" void jobOnUsbErr(uint32_t err_code)
{
  portENTER_CRITICAL(&s_mux);
  if (s_open)
  {
    s_jobs[s_head].usb_err_count++;
    s_jobs[s_head].last_usb_err = err_code;
  }
  portEXIT_CRITICAL(&s_mux);
}

extern "C" void jobOnUsbTimeout()
{
  portENTER_CRITICAL(&s_mux);
  if (s_open) s_jobs[s_head].usb_timeout_count++;
  portEXIT_CRITICAL(&s_mux);
}

extern "C" void jobOnDropped(size_t bytes)
{
  portENTER_CRITICAL(&s_mux);
  if (s_open) s_jobs[s_head].bytes_dropped += (uint32_t)bytes;
  portEXIT_CRITICAL(&s_mux);
}

extern "C" void jobOnStreamHighWater(size_t used)
{
  portENTER_CRITICAL(&s_mux);
  if (s_open && used > s_jobs[s_head].peak_stream_used)
  {
    s_jobs[s_head].peak_stream_used = (uint32_t)used;
  }
  portEXIT_CRITICAL(&s_mux);
}

extern "C" size_t jobLogSnapshot(JobRecord *out, size_t maxRecords)
{
  if (!out || maxRecords == 0) return 0;

  size_t count = 0;
  portENTER_CRITICAL(&s_mux);
  // Walk most-recent first.
  size_t idx = s_head;
  for (size_t i = 0; i < kJobSlots && count < maxRecords; i++)
  {
    if (s_jobs[idx].id != 0)
    {
      out[count++] = s_jobs[idx];
    }
    // Step backwards.
    idx = (idx == 0) ? (kJobSlots - 1) : (idx - 1);
  }
  portEXIT_CRITICAL(&s_mux);
  return count;
}

extern "C" bool jobLogLatest(JobRecord *out)
{
  if (!out) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s_jobs[s_head].id != 0)
  {
    *out = s_jobs[s_head];
    ok   = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

extern "C" const char *jobSigName(uint8_t sig)
{
  switch (sig)
  {
    case SIG_HP_UEL_PJL_ZJS: return "HP_UEL_PJL_ZJS";
    case SIG_PJL_NO_ZJS:     return "PJL_NO_ZJS";
    case SIG_POSTSCRIPT:     return "POSTSCRIPT";
    case SIG_PWG_RASTER:     return "PWG_RASTER";
    case SIG_PDF:            return "PDF";
    case SIG_OTHER_BINARY:   return "OTHER_BINARY";
    case SIG_UNKNOWN:        return "UNKNOWN";
    default:                 return "?";
  }
}

extern "C" const char *jobCloseReasonName(uint8_t r)
{
  switch (r)
  {
    case JCR_OPEN:         return "open";
    case JCR_CLIENT_FIN:   return "client_fin";
    case JCR_IDLE_TIMEOUT: return "idle_timeout";
    case JCR_USB_ERROR:    return "usb_error";
    case JCR_NO_PRINTER:   return "no_printer";
    case JCR_OTHER:        return "other";
    case JCR_JOB_COMPLETE: return "job_complete";
    default:               return "?";
  }
}
