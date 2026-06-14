// =============================================================================
//  Unit tests for job_log (per-print-job diagnostic ring).
//
//  Runs on the host via `pio test -e native`.  The real src/job_log.cpp is
//  compiled against the native shims (FreeRTOS / esp_timer); no hardware or
//  RTOS required.
// =============================================================================

#include <unity.h>

#include <string.h>

#include "job_log.h"
#include "test_support.h"

// --- helpers ----------------------------------------------------------------

static uint32_t beginOne(const char *ip = "10.0.0.1")
{
  return jobBegin(ip);
}

// Feed a NUL-terminated ASCII payload (without the trailing NUL).
static void feed(const char *s)
{
  jobAppendPayload(reinterpret_cast<const uint8_t *>(s), strlen(s));
}

// --- id allocation & slot placement ----------------------------------------

void test_first_job_lands_in_slot0_with_id1(void)
{
  jobLogInit();
  uint32_t id = beginOne();
  TEST_ASSERT_EQUAL_UINT32(1, id);

  JobRecord latest;
  TEST_ASSERT_TRUE(jobLogLatest(&latest));
  TEST_ASSERT_EQUAL_UINT32(1, latest.id);
  TEST_ASSERT_EQUAL_STRING("10.0.0.1", latest.peer_ip);
  TEST_ASSERT_EQUAL_UINT8(JCR_OPEN, latest.close_reason);
}

void test_ids_increment_monotonically(void)
{
  jobLogInit();
  TEST_ASSERT_EQUAL_UINT32(1, beginOne());
  jobEnd(JCR_CLIENT_FIN);
  TEST_ASSERT_EQUAL_UINT32(2, beginOne());
  jobEnd(JCR_CLIENT_FIN);
  TEST_ASSERT_EQUAL_UINT32(3, beginOne());
}

void test_reopen_closes_previous_open_job(void)
{
  jobLogInit();
  beginOne();                 // left open on purpose
  fakeClockAdvanceMs(50);
  beginOne();                 // should force-close the previous one

  JobRecord arr[16];
  size_t n = jobLogSnapshot(arr, 16);
  TEST_ASSERT_EQUAL_UINT32(2, n);
  // arr[0] is newest (id 2, still open); arr[1] is the force-closed one.
  TEST_ASSERT_EQUAL_UINT32(2, arr[0].id);
  TEST_ASSERT_EQUAL_UINT32(1, arr[1].id);
  TEST_ASSERT_NOT_EQUAL(0, arr[1].end_ms);
  TEST_ASSERT_EQUAL_UINT8(JCR_OTHER, arr[1].close_reason);
}

// --- timing -----------------------------------------------------------------

void test_start_and_end_timestamps(void)
{
  jobLogInit();
  fakeClockSetMs(1000);
  beginOne();
  fakeClockSetMs(1075);
  jobEnd(JCR_JOB_COMPLETE);

  JobRecord j;
  TEST_ASSERT_TRUE(jobLogLatest(&j));
  TEST_ASSERT_EQUAL_UINT32(1000, j.start_ms);
  TEST_ASSERT_EQUAL_UINT32(1075, j.end_ms);
  TEST_ASSERT_EQUAL_UINT8(JCR_JOB_COMPLETE, j.close_reason);
}

// --- payload accumulation & first64 cap -------------------------------------

void test_bytes_in_accumulates(void)
{
  jobLogInit();
  beginOne();
  feed("hello");
  feed("world!!");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT32(12, j.bytes_in);
}

void test_first64_capped_at_64_bytes(void)
{
  jobLogInit();
  beginOne();
  char big[200];
  memset(big, 'A', sizeof(big));
  jobAppendPayload(reinterpret_cast<uint8_t *>(big), sizeof(big));

  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(64, j.first64_len);
  TEST_ASSERT_EQUAL_UINT32(200, j.bytes_in);
}

// --- signature classification -----------------------------------------------

void test_classify_hp_uel_pjl_zjs(void)
{
  jobLogInit();
  beginOne();
  // HP UEL (ESC %-12345X) + PJL + ZJS token within the first chunk.
  const char *p = "\x1B%-12345X@PJL ENTER LANGUAGE=ZJS\r\n";
  feed(p);
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_HP_UEL_PJL_ZJS, j.sig);
}

void test_classify_pjl_without_zjs(void)
{
  jobLogInit();
  beginOne();
  feed("\x1B%-12345X@PJL ENTER LANGUAGE=PCL\r\n");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_PJL_NO_ZJS, j.sig);
}

void test_classify_postscript(void)
{
  jobLogInit();
  beginOne();
  feed("%!PS-Adobe-3.0\n");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_POSTSCRIPT, j.sig);
}

void test_classify_pdf(void)
{
  jobLogInit();
  beginOne();
  feed("%PDF-1.7\n");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_PDF, j.sig);
}

void test_classify_pwg_raster(void)
{
  jobLogInit();
  beginOne();
  feed("RaS2");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_PWG_RASTER, j.sig);
}

void test_classify_other_binary(void)
{
  jobLogInit();
  beginOne();
  feed("\x01\x02\x03\x04 not a known header");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_OTHER_BINARY, j.sig);
}

void test_zjs_detected_across_chunk_boundary(void)
{
  // ZJS token can land beyond the 64-byte first64 buffer; the classifier
  // also scans each incoming chunk.  Verify a late ZJS still upgrades the sig.
  jobLogInit();
  beginOne();
  char pad[80];
  memset(pad, 'X', sizeof(pad));
  pad[0] = 0x1B; // not a full UEL, but we rely on PJL+ZJS path
  feed("\x1B%-12345X@PJL JOB\r\n");
  // First64 now full of PJL preamble; ZJS arrives in a later chunk.
  jobAppendPayload(reinterpret_cast<uint8_t *>(pad), sizeof(pad));
  feed("ENTER LANGUAGE=ZJS\r\n");
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT8(SIG_HP_UEL_PJL_ZJS, j.sig);
}

// --- USB-side counters ------------------------------------------------------

void test_usb_counters(void)
{
  jobLogInit();
  beginOne();
  jobOnUsbWrite(100);
  jobOnUsbWrite(50);
  jobOnUsbErr(0x103);
  jobOnUsbErr(0x108);
  jobOnUsbTimeout();
  jobOnDropped(7);
  jobOnStreamHighWater(2048);
  jobOnStreamHighWater(1024);  // lower - must NOT overwrite the peak

  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT32(150, j.bytes_out);
  TEST_ASSERT_EQUAL_UINT16(2, j.usb_err_count);
  TEST_ASSERT_EQUAL_UINT32(0x108, j.last_usb_err);
  TEST_ASSERT_EQUAL_UINT16(1, j.usb_timeout_count);
  TEST_ASSERT_EQUAL_UINT32(7, j.bytes_dropped);
  TEST_ASSERT_EQUAL_UINT32(2048, j.peak_stream_used);
}

void test_counters_ignored_when_no_open_job(void)
{
  jobLogInit();
  beginOne();
  jobEnd(JCR_CLIENT_FIN);
  // No open job now - these must be silently dropped, not crash.
  jobOnUsbWrite(999);
  jobOnDropped(999);
  JobRecord j;
  jobLogLatest(&j);
  TEST_ASSERT_EQUAL_UINT32(0, j.bytes_out);
  TEST_ASSERT_EQUAL_UINT32(0, j.bytes_dropped);
}

// --- snapshot ordering & ring wrap ------------------------------------------

void test_snapshot_is_most_recent_first(void)
{
  jobLogInit();
  for (int i = 0; i < 3; i++) { beginOne(); jobEnd(JCR_CLIENT_FIN); }
  JobRecord arr[16];
  size_t n = jobLogSnapshot(arr, 16);
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_UINT32(3, arr[0].id);
  TEST_ASSERT_EQUAL_UINT32(2, arr[1].id);
  TEST_ASSERT_EQUAL_UINT32(1, arr[2].id);
}

void test_ring_wraps_keeping_16_newest(void)
{
  jobLogInit();
  for (int i = 0; i < 20; i++) { beginOne(); jobEnd(JCR_CLIENT_FIN); }
  JobRecord arr[16];
  size_t n = jobLogSnapshot(arr, 16);
  TEST_ASSERT_EQUAL_UINT32(16, n);          // ring holds only 16
  TEST_ASSERT_EQUAL_UINT32(20, arr[0].id);  // newest
  TEST_ASSERT_EQUAL_UINT32(5,  arr[15].id); // oldest still retained (20..5)
}

void test_snapshot_respects_maxRecords(void)
{
  jobLogInit();
  for (int i = 0; i < 10; i++) { beginOne(); jobEnd(JCR_CLIENT_FIN); }
  JobRecord arr[4];
  size_t n = jobLogSnapshot(arr, 4);
  TEST_ASSERT_EQUAL_UINT32(4, n);
  TEST_ASSERT_EQUAL_UINT32(10, arr[0].id);
  TEST_ASSERT_EQUAL_UINT32(7,  arr[3].id);
}

void test_latest_false_when_empty(void)
{
  jobLogInit();
  JobRecord j;
  TEST_ASSERT_FALSE(jobLogLatest(&j));
}

// --- name tables ------------------------------------------------------------

void test_sig_names(void)
{
  TEST_ASSERT_EQUAL_STRING("HP_UEL_PJL_ZJS", jobSigName(SIG_HP_UEL_PJL_ZJS));
  TEST_ASSERT_EQUAL_STRING("PJL_NO_ZJS",     jobSigName(SIG_PJL_NO_ZJS));
  TEST_ASSERT_EQUAL_STRING("POSTSCRIPT",     jobSigName(SIG_POSTSCRIPT));
  TEST_ASSERT_EQUAL_STRING("PWG_RASTER",     jobSigName(SIG_PWG_RASTER));
  TEST_ASSERT_EQUAL_STRING("PDF",            jobSigName(SIG_PDF));
  TEST_ASSERT_EQUAL_STRING("OTHER_BINARY",   jobSigName(SIG_OTHER_BINARY));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN",        jobSigName(SIG_UNKNOWN));
  TEST_ASSERT_EQUAL_STRING("?",              jobSigName(200));
}

void test_close_reason_names(void)
{
  TEST_ASSERT_EQUAL_STRING("open",         jobCloseReasonName(JCR_OPEN));
  TEST_ASSERT_EQUAL_STRING("client_fin",   jobCloseReasonName(JCR_CLIENT_FIN));
  TEST_ASSERT_EQUAL_STRING("idle_timeout", jobCloseReasonName(JCR_IDLE_TIMEOUT));
  TEST_ASSERT_EQUAL_STRING("usb_error",    jobCloseReasonName(JCR_USB_ERROR));
  TEST_ASSERT_EQUAL_STRING("no_printer",   jobCloseReasonName(JCR_NO_PRINTER));
  TEST_ASSERT_EQUAL_STRING("other",        jobCloseReasonName(JCR_OTHER));
  TEST_ASSERT_EQUAL_STRING("job_complete", jobCloseReasonName(JCR_JOB_COMPLETE));
  TEST_ASSERT_EQUAL_STRING("?",            jobCloseReasonName(200));
}

void run_job_log_tests(void)
{
  RUN_TEST(test_first_job_lands_in_slot0_with_id1);
  RUN_TEST(test_ids_increment_monotonically);
  RUN_TEST(test_reopen_closes_previous_open_job);
  RUN_TEST(test_start_and_end_timestamps);
  RUN_TEST(test_bytes_in_accumulates);
  RUN_TEST(test_first64_capped_at_64_bytes);
  RUN_TEST(test_classify_hp_uel_pjl_zjs);
  RUN_TEST(test_classify_pjl_without_zjs);
  RUN_TEST(test_classify_postscript);
  RUN_TEST(test_classify_pdf);
  RUN_TEST(test_classify_pwg_raster);
  RUN_TEST(test_classify_other_binary);
  RUN_TEST(test_zjs_detected_across_chunk_boundary);
  RUN_TEST(test_usb_counters);
  RUN_TEST(test_counters_ignored_when_no_open_job);
  RUN_TEST(test_snapshot_is_most_recent_first);
  RUN_TEST(test_ring_wraps_keeping_16_newest);
  RUN_TEST(test_snapshot_respects_maxRecords);
  RUN_TEST(test_latest_false_when_empty);
  RUN_TEST(test_sig_names);
  RUN_TEST(test_close_reason_names);
}
