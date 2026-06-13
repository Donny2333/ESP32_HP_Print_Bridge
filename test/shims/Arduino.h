// =============================================================================
//  Native test shim: Arduino.h
//
//  log_sink.cpp calls Serial.write(buf, len) to mirror output to the local
//  console.  On the host we capture those bytes into a buffer the tests can
//  inspect (g_serialOut), instead of touching any real UART.
//
//  Only the surface actually used by the tested modules is provided.
// =============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

// Capture buffer for everything written to "Serial".  Defined in
// test_support.cpp; tests may clear/inspect it.
extern std::string g_serialOut;

class HostSerial
{
public:
  size_t write(const uint8_t *buf, size_t len)
  {
    g_serialOut.append(reinterpret_cast<const char *>(buf), len);
    return len;
  }
};

extern HostSerial Serial;
