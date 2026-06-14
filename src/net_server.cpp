// =============================================================================
//  net_server: raw lwIP TCP servers (port 9100 print + 9101 telnet log).
//  Code moved verbatim from the original main.cpp (openListenSocket /
//  rawServerTask / tcpLogTask).  No logic changes.
// =============================================================================

#include "net_server.h"

#include <Arduino.h>

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

// POSIX-style socket API exposed by lwIP - we bypass Arduino-ESP32's
// WiFiServer entirely for the raw print port and the telnet log port,
// because WiFiServer::accept()/available() in 3.x + IDF v4.4 (arduino-as-
// component) does not deliver successfully-handshaken clients in this build.
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "bridge_config.h"
#include "bridge_state.h"
#include "usb_bridge.h"   // waitForPrinter / usbCtrlGetPortStatus
#include "log_sink.h"
#include "job_log.h"

// -----------------------------------------------------------------------------
// Module-private state (used only by the tasks in this file).
// -----------------------------------------------------------------------------

static volatile uint32_t s_lastRecvActivityMs = 0;
static volatile bool     s_syntheticInjected  = false;

// Currently-active raw9100 client fd, or -1 if none.  Shared between the raw
// server (publishes it) and the telnet task (clears it when dropping a fd).
static volatile int      s_rawClientFd        = -1;

// -----------------------------------------------------------------------------
// TCP raw server (port 9100)
//
// Implementation note: we use raw lwIP / POSIX-style socket calls here rather
// than Arduino's WiFiServer because under arduino-as-IDF-component + Arduino-
// ESP32 3.x + IDF v4.4, WiFiServer::accept()/available() does not deliver
// successfully-handshaken clients to userspace.  `nc -vz` reports the TCP
// handshake completed, but the WiFiServer wrapper never produces a non-null
// WiFiClient, so all "[raw9100] connection from" log lines were never emitted
// and /jobs stayed empty.  Going to POSIX socket avoids the wrapper entirely.
// -----------------------------------------------------------------------------

// Helper: open a listening TCP socket on the given port.  Returns fd >= 0 on
// success, -1 on failure.  Sets SO_REUSEADDR so a quick restart can rebind.
static int openListenSocket(uint16_t port, int backlog)
{
  int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (lfd < 0)
  {
    logTee("[sock] socket() failed errno=%d\n", errno);
    return -1;
  }

  int yes = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr = {};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(port);

  if (::bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    logTee("[sock] bind(:%u) failed errno=%d\n", (unsigned)port, errno);
    ::close(lfd);
    return -1;
  }

  if (::listen(lfd, backlog) < 0)
  {
    logTee("[sock] listen(:%u) failed errno=%d\n", (unsigned)port, errno);
    ::close(lfd);
    return -1;
  }
  return lfd;
}

void rawServerTask(void *arg)
{
  logTeeln("[raw9100] task start");

  // Bring up the listen socket.  If Wi-Fi happens to be down at this exact
  // moment, retry until it succeeds - we don't want to wedge forever.
  int lfd = -1;
  while (lfd < 0)
  {
    lfd = openListenSocket(kRawPort, 1);
    if (lfd < 0)
    {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  logTee("[raw9100] listening on :%u (fd=%d), entering accept loop\n",
                (unsigned)kRawPort, lfd);

  uint8_t buf[1460];

  for (;;)
  {
    struct sockaddr_in peer = {};
    socklen_t          peerLen = sizeof(peer);
    int cfd = ::accept(lfd, (struct sockaddr *)&peer, &peerLen);
    if (cfd < 0)
    {
      logTee("[raw9100] accept errno=%d (%s)\n", errno, strerror(errno));
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Format peer IP.
    char peerIp[16] = {0};
    inet_ntoa_r(peer.sin_addr, peerIp, sizeof(peerIp));
    logTee("[raw9100] connection from %s\n", peerIp);

    // Configure the client socket: TCP_NODELAY + short idle read timeout so we
    // can poll for job-complete signals.
    int yes = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    struct timeval idleTo = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &idleTo, sizeof(idleTo));

    // If no printer is attached, hold briefly then refuse the job so the
    // spooler reports failure instead of silently succeeding.
    if (!s_printerReady && !waitForPrinter(kPrinterReadyWaitMs))
    {
      logTeeln("[raw9100] no printer, dropping connection");
      s_jobsDropped++;
      uint32_t jid = jobBegin(peerIp);
      (void)jid;
      jobEnd(JCR_NO_PRINTER);
      ::close(cfd);
      continue;
    }

    s_jobsAccepted++;
    s_lastJobBytes = 0;
    uint32_t jobId = jobBegin(peerIp);
    logTee("[raw9100] job #%u start\n", (unsigned)jobId);

    s_usbConsecutiveErrors = 0;
    s_syntheticInjected = false;
    s_lastUsbWriteMs = 0;
    uint32_t nowMs  = millis();
    s_lastRecvActivityMs = nowMs;

    // Publish the active client fd to the USB reader so back-channel
    // (printer -> host) bytes can flow back over TCP.
    s_rawClientFd = cfd;

    uint8_t closeReason = JCR_CLIENT_FIN;

    for (;;)
    {
      ssize_t got = ::recv(cfd, buf, sizeof(buf), 0);
      if (got > 0)
      {
        s_totalBytesIn += got;
        s_lastJobBytes += got;
        s_lastRecvActivityMs = millis();
        jobAppendPayload(buf, (size_t)got);

        // Push to stream buffer; throttle on backpressure.
        size_t off = 0;
        while ((ssize_t)off < got)
        {
          size_t sent = xStreamBufferSend(s_stream, buf + off, got - off,
                                          pdMS_TO_TICKS(5000));
          if (sent == 0)
          {
            logTeeln("[raw9100] stream buffer full, retrying");
            continue;
          }
          off += sent;
        }
        jobOnStreamHighWater(xStreamBufferBytesAvailable(s_stream));
      }
      else if (got == 0)
      {
        // Peer closed (FIN).
        closeReason = JCR_CLIENT_FIN;
        logTeeln("[raw9100] peer sent FIN");
        break;
      }
      else
      {
        // got < 0; SO_RCVTIMEO fires every 2s.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          uint32_t now = millis();

          // USB is broken: sustained errors mean the printer can't accept
          // more data; continuing to recv from CUPS is pointless.
          if (s_usbConsecutiveErrors >= kMaxConsecutiveUsbErrors)
          {
            closeReason = JCR_USB_ERROR;
            logTee("[raw9100] USB stuck (%u consecutive errors), closing\n",
                   (unsigned)s_usbConsecutiveErrors);
            break;
          }

          // -------------------------------------------------------
          // Synthetic Ready injection: once all print data has been
          // forwarded to USB and the printer has been idle for a few
          // seconds, inject a PJL "Ready" + "Job End" status back to
          // CUPS so its readThread can clear media-needed-error and
          // finish the job.  This is necessary because Bulk IN is
          // disabled (crashes on this printer's USB host stack).
          // -------------------------------------------------------
          bool streamEmpty = (xStreamBufferBytesAvailable(s_stream) == 0);
          bool usbIdle     = s_lastUsbWriteMs != 0 &&
                             (now - s_lastUsbWriteMs) >= 8000;
          bool hasData     = s_lastJobBytes > 0;

          if (streamEmpty && usbIdle && hasData && !s_syntheticInjected)
          {
            // The HP rastertozjs readerProcess detects "end of job" by
            // finding "@PJL USTATUS JOB\r\nEND" in the back-channel data.
            // The \x0c (form feed) separates two PJL messages.
            // This exact 107-byte format was verified working in job 148.
            static const char kSyntheticStatus[] =
              "\r\n@PJL USTATUS JOB\r\nEND\r\nNAME=\"job\"\r\nPAGES=1\r\n\x0c"
              "@PJL INFO STATUS\r\nCODE=10001\r\n"
              "DISPLAY=\"Ready\"\r\nONLINE=TRUE\r\n";

            ssize_t w = ::send(cfd, kSyntheticStatus,
                               sizeof(kSyntheticStatus) - 1, MSG_DONTWAIT);
            s_syntheticInjected = true;
            logTee("[raw9100] injected synthetic Ready+JobEnd (%d bytes)\n", (int)w);

            // After injection, wait 2s for CUPS to process, then force close
            vTaskDelay(pdMS_TO_TICKS(2000));
            closeReason = JCR_JOB_COMPLETE;
            logTeeln("[raw9100] post-injection timeout, closing");
            break;
          }

          // Hard idle timeout (120s - generous for slow renderers)
          if (now - s_lastRecvActivityMs >= 120000)
          {
            logTeeln("[raw9100] idle timeout (120s), closing");
            closeReason = JCR_IDLE_TIMEOUT;
            break;
          }
          continue;
        }
        logTee("[raw9100] recv errno=%d, closing\n", errno);
        closeReason = JCR_OTHER;
        break;
      }
    }

    // Drain phase: we need to wait for the USB writer task to actually
    // finish pushing every queued byte to the printer, not just for the
    // FreeRTOS StreamBuffer to drain.  Bulk transfers can finish ~10-20 ms
    // AFTER the stream buffer is empty (the writer has already received
    // the bytes into its local chunk[] and is mid-flight).  If we finalize
    // the JobRecord before that, we get the misleading "bytes_in=N bytes_out=0"
    // result that previously confused everything.
    //
    // Termination conditions, whichever comes first:
    //  1. usbBytesOutAtJobEnd has caught up with bytesInThisJob
    //  2. 60 s elapsed (large rasterised pages can be slow)
    //  3. printer disconnected / USB error count grew
    uint32_t bytesInThisJob       = s_lastJobBytes;
    uint32_t bytesOutAtJobStart   = s_totalBytesOut - 0;  // see note below
    // Note: s_totalBytesOut is global. For per-job comparison we want the
    // delta since job start; we record it via the JobRecord's bytes_out
    // counter (updated by jobOnUsbWrite()), but the simplest reliable check
    // here is "stream buffer empty AND s_totalBytesOut hasn't moved for 200ms".
    uint32_t drainStart           = millis();
    uint32_t lastOutSeen          = s_totalBytesOut;
    uint32_t lastOutChangeMs      = millis();
    for (;;)
    {
      bool streamEmpty = (xStreamBufferBytesAvailable(s_stream) == 0);
      uint32_t now = millis();
      if (s_totalBytesOut != lastOutSeen)
      {
        lastOutSeen     = s_totalBytesOut;
        lastOutChangeMs = now;
      }
      bool usbQuiescent = streamEmpty && (now - lastOutChangeMs) >= kDrainQuiescentMs;
      if (closeReason == JCR_JOB_COMPLETE || usbQuiescent)
      {
        break;
      }
      if (now - drainStart >= 60000)
      {
        logTeeln("[raw9100] drain timeout (60s), giving up on per-job accounting");
        break;
      }
      if (!s_printerReady)
      {
        logTeeln("[raw9100] printer disappeared during drain");
        break;
      }
      if (s_usbConsecutiveErrors >= kMaxConsecutiveUsbErrors)
      {
        logTeeln("[raw9100] USB stuck during drain, aborting");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    logTeeln("[raw9100] drain complete");

    logTeeln("[raw9100] sending FIN");
    ::shutdown(cfd, SHUT_WR);

    uint32_t finStart = millis();
    bool peerClosed = false;
    for (;;)
    {
      uint8_t scratch[128];
      ssize_t r = ::recv(cfd, scratch, sizeof(scratch), MSG_DONTWAIT);
      if (r == 0)
      {
        peerClosed = true;
        logTee("[raw9100] peer closed %u ms after FIN\n",
                      (unsigned)(millis() - finStart));
        break;
      }
      if (r > 0)
      {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        if (millis() - finStart >= kFinWaitMs)
        {
          logTeeln("[raw9100] FIN wait timeout, forcing close");
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      logTee("[raw9100] recv during FIN errno=%d\n", errno);
      break;
    }

    s_rawClientFd = -1;
    ::close(cfd);

    if (!peerClosed)
    {
      logTeeln("[raw9100] peer did not close before timeout");
    }

    // Now the per-job byte counters are accurate.
    logTee("[raw9100] job #%u closed reason=%s bytes_in=%u total in=%u out=%u\n",
                  (unsigned)jobId,
                  jobCloseReasonName(closeReason),
                  (unsigned)bytesInThisJob,
                  (unsigned)s_totalBytesIn,
                  (unsigned)s_totalBytesOut);
    (void)bytesOutAtJobStart;  // currently unused; see note above

    // Refresh port status after each job (best-effort)
    if (s_printerReady)
    {
      uint8_t st = 0;
      if (usbCtrlGetPortStatus(&st) == ESP_OK) s_portStatus = st;
    }

    // Finalise the JobRecord after USB drain so bytes_out / usb counters
    // reflect the actual outcome, not just the TCP-side state.
    jobEnd(closeReason);
  }
}

// -----------------------------------------------------------------------------
// TCP :9101 telnet log task - streams log ring contents to a single client.
// New connections kick the old one.  Non-blocking writes; slow clients are
// dropped rather than back-pressuring the producer.
//
// Same POSIX socket reasoning as rawServerTask: WiFiServer is unreliable in
// this build, so we go straight to lwIP.
// -----------------------------------------------------------------------------

void tcpLogTask(void *arg)
{
  logTeeln("[log9101] task start");

  int lfd = -1;
  while (lfd < 0)
  {
    lfd = openListenSocket(9101, 1);
    if (lfd < 0) vTaskDelay(pdMS_TO_TICKS(500));
  }
  // Listening socket is non-blocking so the loop can also tend the client
  // without sleeping on accept.
  int lflags = fcntl(lfd, F_GETFL, 0);
  fcntl(lfd, F_SETFL, lflags | O_NONBLOCK);
  logTee("[log9101] listening on :9101 (fd=%d)\n", lfd);

  int      cfd    = -1;
  uint64_t cursor = 0;
  char     chunk[1024];

  for (;;)
  {
    // Accept new connection (non-blocking), kicking any existing one.
    struct sockaddr_in peer = {};
    socklen_t          peerLen = sizeof(peer);
    int newFd = ::accept(lfd, (struct sockaddr *)&peer, &peerLen);
    if (newFd >= 0)
    {
      if (cfd >= 0)
      {
        const char msg[] = "\n[log9101] superseded by new connection\n";
        ::send(cfd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        ::close(cfd);
      }
      cfd = newFd;

      // Configure new client: TCP_NODELAY + non-blocking sends.
      int yes = 1;
      setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
      int cflags = fcntl(cfd, F_GETFL, 0);
      fcntl(cfd, F_SETFL, cflags | O_NONBLOCK);

      char peerIp[16] = {0};
      inet_ntoa_r(peer.sin_addr, peerIp, sizeof(peerIp));
      logTee("[log9101] viewer connected from %s\n", peerIp);

      // Send a snapshot of recent log so the viewer sees context.
      size_t snap = logSinkSnapshot(chunk, sizeof(chunk));
      if (snap > 0)
      {
        ::send(cfd, chunk, snap, 0);
      }
      cursor = logSinkWriteCursor();
      const char banner[] = "\n--- live log (ring tail above; new lines below) ---\n";
      ::send(cfd, banner, sizeof(banner) - 1, 0);
    }

    if (cfd >= 0)
    {
      // Push any new bytes from the ring.
      size_t n = logSinkReadFromCursor(&cursor, chunk, sizeof(chunk));
      if (n > 0)
      {
        ssize_t w = ::send(cfd, chunk, n, MSG_DONTWAIT);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          // Slow / dead client: drop it.
          logTee("[log9101] viewer dropped errno=%d\n", errno);
          ::close(cfd);
          cfd = -1;
        }
      }
      // Discard anything the client sends to us; also detect FIN.
      uint8_t scratch[64];
      ssize_t r = ::recv(cfd, scratch, sizeof(scratch), MSG_DONTWAIT);
      if (r == 0)
      {
        logTeeln("[log9101] viewer closed");
        ::close(cfd);
        cfd = -1;
      }
      // r < 0 with EAGAIN is normal; ignore.
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}
