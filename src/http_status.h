// =============================================================================
//  http_status: HTTP :80 admin/status server.
//
//    /          handleRoot   - status page (Wi-Fi, printer, counters, last job)
//    /reset     handleReset  - send SOFT_RESET to the printer
//    /jobs      handleJobs   - JobRecord ring as JSON (or ?fmt=html table)
//    /tail      handleTail    \ last N bytes of the in-memory log ring
//    /tail.txt  handleTailTxt /
//
//  The WebServer instance is defined here; setup() registers the routes and
//  pumps it from loop().
// =============================================================================

#pragma once

#include <WebServer.h>

extern WebServer webServer;

void handleRoot();
void handleReset();
void handleJobs();
void handleTail();
void handleTailTxt();
