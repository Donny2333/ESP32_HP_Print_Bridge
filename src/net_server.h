// =============================================================================
//  net_server: raw lwIP TCP servers.
//
//    :9100  rawServerTask - HP JetDirect / AppSocket print data.  Receives
//           bytes, pushes them into the TCP->USB stream buffer, and after the
//           USB side goes quiescent injects a synthetic PJL "Job End / Ready"
//           status so the macOS CUPS backend can finish the job.
//    :9101  tcpLogTask    - single-viewer telnet log stream (mirrors the log
//           ring; new connections kick the old one).
//
//  Both deliberately use POSIX/lwIP sockets instead of Arduino WiFiServer
//  (which does not deliver handshaken clients in this build).
// =============================================================================

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// FreeRTOS task entry points (created in setup()).
void rawServerTask(void *arg);
void tcpLogTask(void *arg);

#ifdef __cplusplus
}
#endif
