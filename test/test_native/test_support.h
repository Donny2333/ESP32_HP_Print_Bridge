// =============================================================================
//  test_support: shared backing storage + small helpers for the native tests.
//
//  The header-only shims (esp_timer.h / esp_log.h / Arduino.h) declare a few
//  externs; this header centralises their declarations and the test-side
//  helpers, and support.cpp provides the single definition of each.
// =============================================================================

#pragma once

#include <stdint.h>
#include <string>

#include "esp_log.h"     // vprintf_like_t, g_espLogVprintf
#include "Arduino.h"     // g_serialOut, HostSerial

// --- Fake clock (esp_timer shim reads g_fakeTimeUs) -------------------------
extern int64_t g_fakeTimeUs;

inline void fakeClockSetMs(uint32_t ms) { g_fakeTimeUs = (int64_t)ms * 1000; }
inline void fakeClockAdvanceMs(uint32_t ms) { g_fakeTimeUs += (int64_t)ms * 1000; }

// --- Per-test reset ---------------------------------------------------------
// Clears the captured Serial output and resets the fake clock to 0.  Call from
// setUp() so every test starts from a known state.
void testSupportReset();
