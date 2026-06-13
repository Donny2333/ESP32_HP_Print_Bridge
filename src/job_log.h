// =============================================================================
//  job_log: per-print-job ring buffer for post-mortem debugging.
//
//  Each accepted TCP:9100 connection produces one JobRecord with:
//    - timing
//    - bytes in / out / dropped
//    - the first 64 bytes of the payload (so we can identify the format
//      without packet capture)
//    - a guess at the data signature (HP UEL / PJL / PostScript / ...)
//    - USB error / timeout counters captured while writing this job
//    - close reason (client FIN / idle timeout / USB error / no printer)
//
//  Records are kept in a 16-slot ring.  Multiple writer threads use a
//  portMUX to serialise updates.
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus

class String; // Arduino forward decl (Arduino.h pulls it; we only use ptr)

enum JobSig : uint8_t
{
  SIG_UNKNOWN        = 0,
  SIG_HP_UEL_PJL_ZJS = 1, // 1B 25 2D 31 32 33 34 35 58 ... @PJL ... ZJS
  SIG_PJL_NO_ZJS     = 2, // @PJL but no ENTER LANGUAGE=ZJS
  SIG_POSTSCRIPT     = 3, // "%!PS"
  SIG_PWG_RASTER     = 4, // "RaS2" / "RaS3"
  SIG_PDF            = 5, // "%PDF"
  SIG_OTHER_BINARY   = 6, // non-empty, not matching anything above
};

enum JobCloseReason : uint8_t
{
  JCR_OPEN          = 0,
  JCR_CLIENT_FIN    = 1,
  JCR_IDLE_TIMEOUT  = 2,
  JCR_USB_ERROR     = 3,
  JCR_NO_PRINTER    = 4,
  JCR_OTHER         = 5,
};

struct JobRecord
{
  uint32_t       id;                 // monotonic, 1-based; 0 means empty slot
  uint32_t       start_ms;
  uint32_t       end_ms;             // 0 if still open
  uint32_t       bytes_in;
  uint32_t       bytes_out;
  uint32_t       bytes_dropped;      // dropped by writer (printer not ready)
  uint32_t       last_usb_err;       // last esp_err_t seen on bulk OUT
  uint32_t       peak_stream_used;   // high-water mark on TCP->USB stream buf
  uint16_t       usb_err_count;
  uint16_t       usb_timeout_count;
  uint8_t        first64[64];
  uint8_t        first64_len;
  JobSig         sig;
  JobCloseReason close_reason;
  char           peer_ip[16];
};

extern "C" {
#endif

void        jobLogInit();

// Lifecycle (called from raw9100 task).
//   jobBegin(): allocate a new slot, return the id (1-based).
//   jobAppendPayload(): feed bytes that came over TCP (used to fill first64
//     and update bytes_in)
//   jobEnd(): close the job with a reason.  bytes_dropped is the value the
//     writer task observed (passed in atomically); pass 0 if unknown.
uint32_t    jobBegin(const char *peer_ip);
void        jobAppendPayload(const uint8_t *data, size_t len);
void        jobEnd(uint8_t close_reason);

// USB writer-side hooks.  These all attribute to the currently-open job (if
// any).  If no job is open they are silently dropped.
void        jobOnUsbWrite(size_t bytes);
void        jobOnUsbErr(uint32_t err_code);
void        jobOnUsbTimeout();
void        jobOnDropped(size_t bytes);
void        jobOnStreamHighWater(size_t used);

// Read-only access.  Snapshots N most-recent jobs into the caller's array,
// most-recent first.  Returns number of records copied.
size_t      jobLogSnapshot(struct JobRecord *out, size_t maxRecords);

// Convenience: copy just the latest job into `out`.  Returns true if there
// was at least one.
bool        jobLogLatest(struct JobRecord *out);

// Signature name (short, stable identifier).
const char *jobSigName(uint8_t sig);

// Close-reason name.
const char *jobCloseReasonName(uint8_t r);

#ifdef __cplusplus
} // extern "C"
#endif
