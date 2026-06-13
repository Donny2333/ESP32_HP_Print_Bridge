// =============================================================================
//  Unit tests for log_sink (in-memory log ring + tee helpers).
//
//  Runs on the host via `pio test -e native`, compiling the real
//  src/log_sink.cpp against the native shims (esp_log / esp_timer / Arduino).
//
//  Note: the ring's write cursor is a process-global that only ever grows and
//  has no reset hook (mirroring the firmware).  Tests therefore anchor on the
//  cursor BEFORE writing, or assert on the tail, so they are independent of
//  whatever earlier tests left in the ring.
// =============================================================================

#include <unity.h>

#include <string.h>
#include <string>

#include "log_sink.h"
#include "test_support.h"

static const size_t kRingBytes = 16 * 1024;  // must match log_sink.cpp

// Read everything written to the ring since `cursor` into a std::string.
static std::string drainFromCursor(uint64_t *cursor)
{
  std::string out;
  char buf[1024];
  size_t n;
  while ((n = logSinkReadFromCursor(cursor, buf, sizeof(buf))) > 0)
  {
    out.append(buf, n);
  }
  return out;
}

// --- raw write + snapshot ---------------------------------------------------

void test_write_then_snapshot_returns_tail(void)
{
  const char *marker = "MARKER-ABC-123";
  logSinkWrite(marker, strlen(marker));

  char out[64];
  size_t got = logSinkSnapshot(out, strlen(marker));
  TEST_ASSERT_EQUAL_UINT32(strlen(marker), got);
  TEST_ASSERT_EQUAL_MEMORY(marker, out, got);
}

void test_snapshot_caps_at_requested_len(void)
{
  logSinkWrite("0123456789", 10);
  char out[4];
  size_t got = logSinkSnapshot(out, 4);
  TEST_ASSERT_EQUAL_UINT32(4, got);
  // Returns the LAST 4 bytes ending at the write cursor.
  TEST_ASSERT_EQUAL_MEMORY("6789", out, 4);
}

void test_snapshot_never_exceeds_ring(void)
{
  // Write more than the ring holds; snapshot must cap at ring size and keep
  // the most-recent bytes.
  std::string big(kRingBytes + 5000, 'A');
  big.replace(big.size() - 4, 4, "TAIL");
  logSinkWrite(big.data(), big.size());

  static char out[kRingBytes];
  size_t got = logSinkSnapshot(out, kRingBytes);
  TEST_ASSERT_EQUAL_UINT32(kRingBytes, got);
  // Last 4 bytes must be the tail we wrote.
  TEST_ASSERT_EQUAL_MEMORY("TAIL", out + kRingBytes - 4, 4);
}

void test_write_wraps_preserving_recent_bytes(void)
{
  // Fill nearly the whole ring, then write a small marker that forces a wrap.
  std::string filler(kRingBytes - 3, 'F');
  logSinkWrite(filler.data(), filler.size());
  logSinkWrite("WRAPPED", 7);  // crosses the ring boundary

  char out[7];
  size_t got = logSinkSnapshot(out, 7);
  TEST_ASSERT_EQUAL_UINT32(7, got);
  TEST_ASSERT_EQUAL_MEMORY("WRAPPED", out, 7);
}

// --- cursor streaming -------------------------------------------------------

void test_cursor_streams_only_new_bytes(void)
{
  uint64_t cur = logSinkWriteCursor();
  logSinkWrite("AAA", 3);
  logSinkWrite("BBB", 3);

  std::string got = drainFromCursor(&cur);
  TEST_ASSERT_EQUAL_STRING("AAABBB", got.c_str());

  // Cursor is now caught up: a second drain yields nothing.
  std::string again = drainFromCursor(&cur);
  TEST_ASSERT_EQUAL_UINT32(0, again.size());
}

void test_cursor_far_behind_skips_ahead(void)
{
  // A consumer that fell more than a ring-size behind should be fast-forwarded
  // to the oldest still-valid byte (no duplication, no overrun).
  uint64_t cur = logSinkWriteCursor();        // anchor, then go way past it
  std::string big(kRingBytes * 2, 'Z');
  logSinkWrite(big.data(), big.size());

  std::string got = drainFromCursor(&cur);
  // Can recover at most one ring's worth.
  TEST_ASSERT_EQUAL_UINT32(kRingBytes, got.size());
  // Cursor must now equal the write cursor.
  TEST_ASSERT_EQUAL_UINT64(logSinkWriteCursor(), cur);
}

// --- tee helpers ------------------------------------------------------------

void test_logTee_writes_serial_verbatim(void)
{
  testSupportReset();
  logTee("hello %d", 42);
  // Serial gets the formatted body with NO timestamp prefix.
  TEST_ASSERT_EQUAL_STRING("hello 42", g_serialOut.c_str());
}

void test_logTee_ring_copy_is_timestamped(void)
{
  fakeClockSetMs(7);
  uint64_t cur = logSinkWriteCursor();
  logTee("xy");
  std::string ring = drainFromCursor(&cur);
  // Ring copy is prefixed with "[<ms>] ".
  TEST_ASSERT_EQUAL_STRING("[7] xy", ring.c_str());
}

void test_logTeeln_appends_newline(void)
{
  testSupportReset();
  logTeeln("line");
  TEST_ASSERT_EQUAL_STRING("line\n", g_serialOut.c_str());
}

void test_logTeeNewline_writes_single_lf(void)
{
  testSupportReset();
  logTeeNewline();
  TEST_ASSERT_EQUAL_STRING("\n", g_serialOut.c_str());
}

void run_log_sink_tests(void)
{
  RUN_TEST(test_write_then_snapshot_returns_tail);
  RUN_TEST(test_snapshot_caps_at_requested_len);
  RUN_TEST(test_snapshot_never_exceeds_ring);
  RUN_TEST(test_write_wraps_preserving_recent_bytes);
  RUN_TEST(test_cursor_streams_only_new_bytes);
  RUN_TEST(test_cursor_far_behind_skips_ahead);
  RUN_TEST(test_logTee_writes_serial_verbatim);
  RUN_TEST(test_logTee_ring_copy_is_timestamped);
  RUN_TEST(test_logTeeln_appends_newline);
  RUN_TEST(test_logTeeNewline_writes_single_lf);
}
