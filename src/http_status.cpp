// =============================================================================
//  http_status: HTTP :80 admin/status server.  See http_status.h.
//  Code moved verbatim from the original main.cpp (portStatusDescr /
//  handleRoot / handleReset / hexDumpHtml / jsonEscape / hexShort / handleJobs
//  / serveTail / handleTail / handleTailTxt) plus the WebServer definition.
//  No logic changes.
// =============================================================================

#include "http_status.h"

#include <Arduino.h>
#include <WiFi.h>

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "bridge_config.h"
#include "bridge_state.h"
#include "usb_bridge.h"   // usbCtrlSoftReset
#include "log_sink.h"
#include "job_log.h"
#include "ota_update.h"
#include "version.h"

WebServer webServer(80);

// -----------------------------------------------------------------------------
// HTTP status page
// -----------------------------------------------------------------------------

static String portStatusDescr(uint8_t st)
{
  // USB Printer Class port status bits:
  //   bit 5: Paper Empty   (1 = no paper)
  //   bit 4: Select        (1 = selected / online)
  //   bit 3: Not Error     (1 = no error)
  // All other bits reserved.
  //
  // Special sentinel kPortStatusUnknown (0xFF) means GET_PORT_STATUS never
  // succeeded - common on M1132/M1136 via a USB hub.  Bulk OUT printing
  // still works in that state; we just can't read the printer's status.
  if (st == kPortStatusUnknown)
  {
    return String("unknown (GET_PORT_STATUS not answered by printer)");
  }
  String s = "0x";
  if (st < 0x10) s += "0";
  s += String(st, HEX);
  s += " (";
  s += (st & 0x08) ? "OK" : "ERROR";
  s += (st & 0x10) ? ", online" : ", offline";
  if (st & 0x20)   s += ", PAPER EMPTY";
  s += ")";
  return s;
}

void handleRoot()
{
  if (s_printerReady && s_portStatus == kPortStatusUnknown)
  {
    uint8_t st = 0;
    if (usbCtrlGetPortStatus(&st) == ESP_OK)
    {
      s_portStatus = st;
    }
  }

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<title>ESP32 USB Print Bridge</title>";
  html += "<style>body{font-family:-apple-system,sans-serif;max-width:720px;margin:2em auto;padding:0 1em;color:#222}";
  html += "h1{margin-bottom:0}code,pre{background:#f4f4f4;padding:2px 4px;border-radius:3px}";
  html += "table{border-collapse:collapse;margin-top:1em}td{padding:4px 12px;border-bottom:1px solid #eee;vertical-align:top}";
  html += ".ok{color:#0a0;font-weight:bold}.bad{color:#a00;font-weight:bold}</style></head><body>";
  html += "<h1>ESP32 USB Print Bridge</h1>";
  html += "<p>Bridge to HP LaserJet M1136 over USB &mdash; clients print via TCP <code>:9100</code>.</p>";
  html += "<table>";
  html += "<tr><td>Wi-Fi</td><td>" + WiFi.localIP().toString() + " (" + macToString() + ")</td></tr>";
  html += "<tr><td>Firmware</td><td>" + String(FW_VERSION) +
          " (git " + String(FW_GIT_REV) + ", " + String(FW_GIT_DIRTY) +
          ", built " + String(FW_BUILD_TIME) + ")</td></tr>";
  html += "<tr><td>mDNS</td><td><code>" + String(kMdnsHostname) + ".local</code></td></tr>";
  html += "<tr><td>OTA</td><td>" + String(otaStatusText());
  if (otaInProgress())
  {
    html += " (" + String((unsigned)otaProgressPercent()) + "%)";
  }
  html += "</td></tr>";
  html += "<tr><td>Printer</td><td><span class='";
  html += s_printerReady ? "ok'>READY" : "bad'>not connected";
  html += "</span></td></tr>";
  html += "<tr><td>Identity</td><td>" + String(s_printerIdent) + "</td></tr>";
  if (s_deviceId[0])
  {
    html += "<tr><td>Device ID</td><td><code style='word-break:break-all'>" + String(s_deviceId) + "</code></td></tr>";
  }
  html += "<tr><td>Port status</td><td>" + portStatusDescr(s_portStatus) + "</td></tr>";
  html += "<tr><td>Listen port</td><td>TCP " + String(kRawPort) + " (HP JetDirect / AppSocket)</td></tr>";
  html += "<tr><td>Jobs accepted</td><td>" + String((unsigned)s_jobsAccepted) + " (dropped: " + String((unsigned)s_jobsDropped) + ")</td></tr>";
  html += "<tr><td>Last job bytes</td><td>" + String((unsigned)s_lastJobBytes) + "</td></tr>";
  html += "<tr><td>Bytes in / out / back</td><td>" + String((unsigned)s_totalBytesIn) +
          " / " + String((unsigned)s_totalBytesOut) +
          " / " + String((unsigned)s_totalBytesBack) +
          " (back\u2192client " + String((unsigned)s_totalBackToClient) + ")</td></tr>";
  html += "<tr><td>Free heap</td><td>" + String((unsigned)esp_get_free_heap_size()) +
          " (PSRAM free " + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + ")</td></tr>";
  // Internal SRAM is what String, Wi-Fi, lwIP, USB Host all carve from.
  // When it dips below ~10 KB the device is liable to crash on the next
  // big String allocation.  Watch this number.
  {
    size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t intMin  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    html += "<tr><td>Internal SRAM free</td><td>" + String((unsigned)intFree) +
            " (min seen " + String((unsigned)intMin) + ")</td></tr>";
  }
  html += "</table>";

  // --- Last job summary (most useful single block when debugging) ---
  JobRecord lastJob;
  if (jobLogLatest(&lastJob))
  {
    uint32_t now      = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t agoMs    = now - (lastJob.end_ms ? lastJob.end_ms : lastJob.start_ms);
    uint32_t durMs    = (lastJob.end_ms ? lastJob.end_ms : now) - lastJob.start_ms;
    bool inEqOut      = (lastJob.bytes_in == lastJob.bytes_out);
    bool sigGood      = (lastJob.sig == SIG_HP_UEL_PJL_ZJS);

    html += "<h3>Last job #" + String((unsigned)lastJob.id) + "</h3><table>";
    html += "<tr><td>From</td><td><code>" + String(lastJob.peer_ip) + "</code></td></tr>";
    html += "<tr><td>When</td><td>" + String((unsigned)(agoMs / 1000)) + "s ago, lasted " +
            String((unsigned)durMs) + " ms</td></tr>";
    html += "<tr><td>Close</td><td>" + String(jobCloseReasonName(lastJob.close_reason)) + "</td></tr>";
    html += "<tr><td>Bytes in / out / dropped</td><td>" +
            String((unsigned)lastJob.bytes_in) + " / " +
            String((unsigned)lastJob.bytes_out) + " / " +
            String((unsigned)lastJob.bytes_dropped) +
            (inEqOut ? " <span class='ok'>(in==out)</span>" : " <span class='bad'>(in!=out)</span>") +
            "</td></tr>";
    html += "<tr><td>USB errors / timeouts</td><td>" +
            String((unsigned)lastJob.usb_err_count) + " / " +
            String((unsigned)lastJob.usb_timeout_count);
    if (lastJob.last_usb_err)
    {
      html += " (last err 0x" + String((unsigned)lastJob.last_usb_err, HEX) + ")";
    }
    html += "</td></tr>";
    html += "<tr><td>Stream peak</td><td>" + String((unsigned)lastJob.peak_stream_used) + " bytes</td></tr>";
    html += "<tr><td>Signature</td><td><span class='";
    html += sigGood ? "ok'>" : "bad'>";
    html += String(jobSigName(lastJob.sig)) + "</span>";
    if (!sigGood && lastJob.sig != SIG_UNKNOWN)
    {
      html += " &mdash; <b>likely wrong driver on the host</b>";
    }
    html += "</td></tr>";
    html += "</table>";
    html += "<p>See <a href='/jobs?fmt=html'>/jobs</a> for full history with hex dump, or "
            "<a href='/tail'>/tail</a> for log snapshot.</p>";
  }
  else
  {
    html += "<h3>Last job</h3><p>(no jobs recorded yet)</p>";
  }
  html += "<hr><h3>How to add on macOS</h3><ol>";
  html += "<li>System Settings &rarr; Printers &amp; Scanners &rarr; Add Printer</li>";
  html += "<li>IP tab. Protocol: <b>HP JetDirect &mdash; Socket</b></li>";
  html += "<li>Address: <code>" + WiFi.localIP().toString() + "</code> &nbsp; (or <code>" + String(kMdnsHostname) + ".local</code>)</li>";
  html += "<li>Driver: <b>HP LaserJet Professional M1132 MFP</b> (works for M1136).</li>";
  html += "</ol>";
  html += "<p><a href='/reset'>Send SOFT_RESET to printer</a></p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleReset()
{
  if (!s_printerReady)
  {
    webServer.send(503, "text/plain", "printer not ready");
    return;
  }
  esp_err_t err = usbCtrlSoftReset();
  String msg = String("SOFT_RESET err=0x") + String((unsigned)err, HEX);
  logTeeln("%s", msg.c_str());
  webServer.send(err == ESP_OK ? 200 : 500, "text/plain", msg);
}

// -----------------------------------------------------------------------------
// HTTP /jobs - dump JobRecord ring
//   /jobs            -> JSON (default)
//   /jobs?fmt=html   -> human table with xxd-style hex dump of first 64 bytes
// -----------------------------------------------------------------------------

static String hexDumpHtml(const uint8_t *buf, size_t len)
{
  // xxd-style: 16 bytes per line, "OFFSET  HH HH ...  |ASCII|"
  String out;
  out.reserve(len * 6 + 16);
  char line[128];
  for (size_t off = 0; off < len; off += 16)
  {
    size_t n = (len - off > 16) ? 16 : (len - off);
    int p = snprintf(line, sizeof(line), "%04x  ", (unsigned)off);
    for (size_t i = 0; i < 16; i++)
    {
      if (i < n) p += snprintf(line + p, sizeof(line) - p, "%02x ", buf[off + i]);
      else       p += snprintf(line + p, sizeof(line) - p, "   ");
      if (i == 7) p += snprintf(line + p, sizeof(line) - p, " ");
    }
    p += snprintf(line + p, sizeof(line) - p, " |");
    for (size_t i = 0; i < n; i++)
    {
      uint8_t c = buf[off + i];
      char    ch = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
      // Escape HTML-sensitive chars.
      if      (ch == '<') p += snprintf(line + p, sizeof(line) - p, "&lt;");
      else if (ch == '>') p += snprintf(line + p, sizeof(line) - p, "&gt;");
      else if (ch == '&') p += snprintf(line + p, sizeof(line) - p, "&amp;");
      else                line[p++] = ch;
    }
    line[p++] = '|';
    line[p++] = '\n';
    line[p]   = '\0';
    out += line;
  }
  return out;
}

static String jsonEscape(const char *s)
{
  String out;
  out.reserve(strlen(s) + 2);
  for (const char *p = s; *p; p++)
  {
    char c = *p;
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  return out;
}

static String hexShort(const uint8_t *buf, size_t len)
{
  String out;
  out.reserve(len * 2);
  char b[4];
  for (size_t i = 0; i < len; i++)
  {
    snprintf(b, sizeof(b), "%02x", buf[i]);
    out += b;
  }
  return out;
}

void handleJobs()
{
  JobRecord arr[16];
  size_t n = jobLogSnapshot(arr, 16);
  bool   html = (webServer.arg("fmt") == "html");
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

  if (html)
  {
    String body = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    body += "<title>Jobs - ESP32 Print Bridge</title>";
    body += "<style>body{font-family:-apple-system,sans-serif;max-width:1100px;margin:1.5em auto;padding:0 1em;color:#222}";
    body += "h2{margin-top:1.6em}.job{border:1px solid #ddd;border-radius:6px;padding:.8em 1em;margin:1em 0;background:#fafafa}";
    body += "table.kv td{padding:2px 10px;vertical-align:top}table.kv td:first-child{color:#666;width:11em}";
    body += "pre{background:#0c0c0c;color:#d0d0d0;padding:.7em;border-radius:4px;overflow:auto;font-size:12px;line-height:1.35}";
    body += ".ok{color:#0a0;font-weight:bold}.bad{color:#a00;font-weight:bold}";
    body += "</style></head><body>";
    body += "<h1>Recent print jobs</h1>";
    body += "<p><a href='/'>&larr; status</a> &middot; <a href='/tail'>/tail</a> &middot; <code>curl /jobs | jq</code></p>";
    if (n == 0)
    {
      body += "<p>(no jobs recorded yet)</p>";
    }
    for (size_t i = 0; i < n; i++)
    {
      const JobRecord &j = arr[i];
      uint32_t agoMs = now - (j.end_ms ? j.end_ms : j.start_ms);
      uint32_t durMs = (j.end_ms ? j.end_ms : now) - j.start_ms;
      bool inEqOut = (j.bytes_in == j.bytes_out);
      bool sigGood = (j.sig == SIG_HP_UEL_PJL_ZJS);

      body += "<div class='job'><h2>#" + String((unsigned)j.id) + " from " + String(j.peer_ip) + "</h2>";
      body += "<table class='kv'>";
      body += "<tr><td>When</td><td>" + String((unsigned)(agoMs / 1000)) + "s ago &middot; lasted " +
              String((unsigned)durMs) + " ms</td></tr>";
      body += "<tr><td>Close</td><td>" + String(jobCloseReasonName(j.close_reason)) + "</td></tr>";
      body += "<tr><td>Signature</td><td><span class='";
      body += sigGood ? "ok'>" : "bad'>";
      body += String(jobSigName(j.sig)) + "</span>";
      if (!sigGood && j.sig != SIG_UNKNOWN)
      {
        body += " &mdash; likely wrong driver on the host";
      }
      body += "</td></tr>";
      body += "<tr><td>bytes_in / out / dropped</td><td>" +
              String((unsigned)j.bytes_in) + " / " +
              String((unsigned)j.bytes_out) + " / " +
              String((unsigned)j.bytes_dropped);
      body += inEqOut ? " <span class='ok'>(in==out)</span>"
                      : " <span class='bad'>(in!=out)</span>";
      body += "</td></tr>";
      body += "<tr><td>USB err / timeouts</td><td>" +
              String((unsigned)j.usb_err_count) + " / " +
              String((unsigned)j.usb_timeout_count);
      if (j.last_usb_err)
      {
        body += " (last 0x" + String((unsigned)j.last_usb_err, HEX) + ")";
      }
      body += "</td></tr>";
      body += "<tr><td>Stream peak</td><td>" + String((unsigned)j.peak_stream_used) + " bytes</td></tr>";
      body += "</table>";

      if (j.first64_len > 0)
      {
        body += "<p>First " + String((unsigned)j.first64_len) + " bytes:</p><pre>";
        body += hexDumpHtml(j.first64, j.first64_len);
        body += "</pre>";
      }
      body += "</div>";
    }
    body += "</body></html>";
    webServer.send(200, "text/html; charset=utf-8", body);
    return;
  }

  // --- JSON ---
  String body = "[";
  for (size_t i = 0; i < n; i++)
  {
    const JobRecord &j = arr[i];
    if (i) body += ",";
    body += "{";
    body += "\"id\":"               + String((unsigned)j.id);
    body += ",\"peer\":\""          + jsonEscape(j.peer_ip) + "\"";
    body += ",\"start_ms\":"        + String((unsigned)j.start_ms);
    body += ",\"end_ms\":"          + String((unsigned)j.end_ms);
    body += ",\"duration_ms\":"     + String((unsigned)((j.end_ms ? j.end_ms : now) - j.start_ms));
    body += ",\"bytes_in\":"        + String((unsigned)j.bytes_in);
    body += ",\"bytes_out\":"       + String((unsigned)j.bytes_out);
    body += ",\"bytes_dropped\":"   + String((unsigned)j.bytes_dropped);
    body += ",\"usb_err_count\":"   + String((unsigned)j.usb_err_count);
    body += ",\"usb_timeout_count\":" + String((unsigned)j.usb_timeout_count);
    body += ",\"last_usb_err\":\"0x" + String((unsigned)j.last_usb_err, HEX) + "\"";
    body += ",\"peak_stream_used\":" + String((unsigned)j.peak_stream_used);
    body += ",\"close_reason\":\""  + String(jobCloseReasonName(j.close_reason)) + "\"";
    body += ",\"sig\":\""           + String(jobSigName(j.sig)) + "\"";
    body += ",\"first64_hex\":\""   + hexShort(j.first64, j.first64_len) + "\"";
    body += ",\"first64_len\":"     + String((unsigned)j.first64_len);
    body += "}";
  }
  body += "]";
  webServer.send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// HTTP /tail  /tail.txt - return last N bytes of the in-memory log ring
//
// Both endpoints now serve plain text.  The previous HTML wrapper used
// String += in a tight loop to do HTML escaping, which reallocated the heap
// repeatedly and could exhaust internal SRAM under fast refreshes, crashing
// the device.  Browsers happily render text/plain in a fixed-width font,
// so we don't lose much by dropping the HTML chrome.  Live tail (with auto
// scroll) is the job of `nc <ip> 9101`.
// -----------------------------------------------------------------------------

static void serveTail(const char *mime)
{
  size_t want = 8192;
  if (webServer.hasArg("n"))
  {
    long v = webServer.arg("n").toInt();
    if (v > 0 && v <= 16384) want = (size_t)v;
  }
  // Stack-friendly: 16KB is the cap, matches the ring size.
  static char buf[16384];
  size_t got = logSinkSnapshot(buf, want);
  webServer.send_P(200, mime, buf, got);
}

void handleTail()    { serveTail("text/plain; charset=utf-8"); }
void handleTailTxt() { serveTail("text/plain; charset=utf-8"); }
