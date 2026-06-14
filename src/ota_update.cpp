#include "ota_update.h"

#include <Arduino.h>
#include <ArduinoOTA.h>

#include "bridge_config.h"
#include "log_sink.h"

namespace {

volatile bool    s_otaInProgress = false;
volatile uint8_t s_otaProgress   = 0;
const char      *s_otaStatus     = "idle";

const char *otaErrorName(ota_error_t error)
{
  switch (error)
  {
    case OTA_AUTH_ERROR:    return "auth";
    case OTA_BEGIN_ERROR:   return "begin";
    case OTA_CONNECT_ERROR: return "connect";
    case OTA_RECEIVE_ERROR: return "receive";
    case OTA_END_ERROR:     return "end";
    default:                return "unknown";
  }
}

} // namespace

void otaInit()
{
  ArduinoOTA.setHostname(kMdnsHostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    s_otaInProgress = true;
    s_otaProgress = 0;
    s_otaStatus = "updating";
    logTeeln("[ota] start");
  });

  ArduinoOTA.onEnd([]() {
    s_otaProgress = 100;
    s_otaStatus = "rebooting";
    logTeeln("[ota] end, rebooting");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) return;
    uint8_t pct = (uint8_t)((progress * 100U) / total);
    if (pct != s_otaProgress && (pct == 100 || pct >= s_otaProgress + 10))
    {
      s_otaProgress = pct;
      logTee("[ota] progress %u%%\n", (unsigned)pct);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    s_otaInProgress = false;
    s_otaStatus = otaErrorName(error);
    logTee("[ota] error %u (%s)\n", (unsigned)error, s_otaStatus);
  });

  ArduinoOTA.begin();
  s_otaStatus = "ready";
  logTee("[ota] ready host=%s.local\n", kMdnsHostname);
}

void otaHandle()
{
  ArduinoOTA.handle();
}

bool otaInProgress()
{
  return s_otaInProgress;
}

uint8_t otaProgressPercent()
{
  return s_otaProgress;
}

const char *otaStatusText()
{
  return s_otaStatus;
}
