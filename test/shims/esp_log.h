// =============================================================================
//  Native test shim: esp_log.h
//
//  log_sink installs a vprintf hook via esp_log_set_vprintf().  On the host we
//  keep a single hook pointer; the shim's set function returns the previous one
//  (mirroring the real API contract that log_sink relies on for chaining).
// =============================================================================

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*vprintf_like_t)(const char *, va_list);

// Backing store defined in test_support.cpp.
extern vprintf_like_t g_espLogVprintf;

static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t f)
{
  vprintf_like_t prev = g_espLogVprintf;
  g_espLogVprintf = f;
  return prev;
}

#ifdef __cplusplus
}
#endif
