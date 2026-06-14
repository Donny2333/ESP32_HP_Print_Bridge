// =============================================================================
//  Native test shim: esp_timer.h
//
//  Provides a controllable fake clock so tests can assert on timing-derived
//  fields (start_ms / end_ms / duration).  test_helpers.h exposes
//  fakeClockSet() to drive it.
// =============================================================================

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Backing store for the fake clock, in microseconds.  Defined in
// test_support.cpp (single definition across the native test binary).
extern int64_t g_fakeTimeUs;

static inline int64_t esp_timer_get_time(void) { return g_fakeTimeUs; }

#ifdef __cplusplus
}
#endif
