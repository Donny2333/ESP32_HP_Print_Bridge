#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the MQTT client.
// This should be called after Wi-Fi is connected.
void mqttLoggerInit();

// Publish a single job event as JSON.
void mqttPublishJob(const char *json_payload);

// Stop and clean up the MQTT client
void mqttLoggerDeinit();

#ifdef __cplusplus
}
#endif
