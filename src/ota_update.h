#pragma once

#include <stddef.h>
#include <stdint.h>

void otaInit();
void otaHandle();
bool otaInProgress();
uint8_t otaProgressPercent();
const char *otaStatusText();
