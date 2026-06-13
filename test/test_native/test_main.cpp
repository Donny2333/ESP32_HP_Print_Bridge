// =============================================================================
//  Native test entry point.  Aggregates every module's test group under one
//  Unity runner so `pio test -e native` executes the whole regression suite.
//
//  Each test_<module>.cpp exposes a run_<module>_tests() that issues its
//  RUN_TEST() calls; this file owns UNITY_BEGIN()/UNITY_END() and setUp().
// =============================================================================

#include <unity.h>

#include "test_support.h"

// Provided by the per-module test files.
void run_job_log_tests(void);
void run_log_sink_tests(void);

// Unity calls these around every RUN_TEST.
void setUp(void)    { testSupportReset(); }
void tearDown(void) {}

int main(int, char **)
{
  UNITY_BEGIN();
  run_job_log_tests();
  run_log_sink_tests();
  return UNITY_END();
}
