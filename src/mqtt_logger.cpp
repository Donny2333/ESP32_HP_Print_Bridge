#include "mqtt_logger.h"
#include "bridge_config.h" // For kSsid, etc.
#include "log_sink.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_client.h"

#include "secrets.h"

#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI ""
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
#define MQTT_TOPIC_LOGS "esp32_printer/logs"
#define MQTT_TOPIC_JOBS "esp32_printer/jobs"
#endif

static esp_mqtt_client_handle_t s_mqtt_client = nullptr;
static bool s_mqtt_connected = false;
static TaskHandle_t s_mqtt_log_task = nullptr;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            // Standard printf so we don't recursive loop into our own log ring / mqtt sender if something misbehaves
            printf("[mqtt] Connected to broker\n");
            s_mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            printf("[mqtt] Disconnected from broker\n");
            s_mqtt_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            printf("[mqtt] Error event\n");
            break;
        default:
            break;
    }
}

// Background task to poll the log sink and publish to MQTT
static void mqttLogTask(void *arg)
{
    uint64_t cursor = logSinkWriteCursor();
    char buf[1024];

    while (true)
    {
        if (s_mqtt_connected && s_mqtt_client)
        {
            size_t got = logSinkReadFromCursor(&cursor, buf, sizeof(buf) - 1);
            if (got > 0)
            {
                buf[got] = '\0';
                // Publish with QoS 0, no retain.
                // We use QoS 0 so we don't block/consume internal queue heavily if connection is jittery.
                esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_LOGS, buf, got, 0, 0);
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(100)); // Poll every 100ms
            }
        }
        else
        {
            // If not connected, keep advancing cursor so we don't flood on reconnect
            logSinkReadFromCursor(&cursor, buf, sizeof(buf));
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void mqttLoggerInit()
{
    if (s_mqtt_client != nullptr) return; // Already init

    if (strlen(MQTT_BROKER_URI) == 0) {
        printf("[mqtt] Broker URI empty, MQTT disabled.\n");
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.uri = MQTT_BROKER_URI;
    if (strlen(MQTT_USERNAME) > 0) mqtt_cfg.username = MQTT_USERNAME;
    if (strlen(MQTT_PASSWORD) > 0) mqtt_cfg.password = MQTT_PASSWORD;
    
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        printf("[mqtt] Client init failed\n");
        return;
    }

    esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, s_mqtt_client);
    esp_mqtt_client_start(s_mqtt_client);

    xTaskCreatePinnedToCore(mqttLogTask, "mqttLog", 4096, nullptr, 1, &s_mqtt_log_task, 1);
}

void mqttPublishJob(const char *json_payload)
{
    if (s_mqtt_connected && s_mqtt_client && json_payload)
    {
        esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_JOBS, json_payload, strlen(json_payload), 1, 0);
    }
}

void mqttLoggerDeinit()
{
    if (s_mqtt_log_task) {
        vTaskDelete(s_mqtt_log_task);
        s_mqtt_log_task = nullptr;
    }
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = nullptr;
    }
    s_mqtt_connected = false;
}
