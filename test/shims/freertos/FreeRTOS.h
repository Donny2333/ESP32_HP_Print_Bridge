// =============================================================================
//  Native test shim: freertos/FreeRTOS.h
//
//  Host-side stand-in so the real src/*.cpp can compile under `pio test -e
//  native`.  The tested modules (job_log, log_sink) only use portMUX critical
//  sections and the IRAM_ATTR attribute - both no-ops in a single-threaded
//  host test.  This shim is NEVER compiled into firmware; the embedded build
//  uses the genuine ESP-IDF headers.
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>

// portMUX critical sections collapse to no-ops: native tests are single
// threaded, so there is no concurrency to guard against.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux)  ((void)(mux))
#define portEXIT_CRITICAL(mux)   ((void)(mux))

// No instruction-RAM placement on the host.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// Tick helpers (not used by the tested modules, provided for completeness).
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif
