// =============================================================================
//  test_support: single definition of the shim-backing globals.
// =============================================================================

#include "test_support.h"

// Fake monotonic clock, in microseconds (esp_timer shim).
int64_t g_fakeTimeUs = 0;

// Captured "Serial" output (Arduino shim).
std::string g_serialOut;
HostSerial  Serial;

// Current esp_log vprintf hook (esp_log shim).  log_sink installs its own and
// chains to whatever was here before (nullptr in tests).
vprintf_like_t g_espLogVprintf = nullptr;

void testSupportReset()
{
  g_serialOut.clear();
  g_fakeTimeUs = 0;
}
