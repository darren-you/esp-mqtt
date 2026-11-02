// SPDX-License-Identifier: Apache-2.0
/* Real emqtt, allocator, FreeRTOS and CPU; SDK client calls/events are injected.
 * No MQTT network, Flash, NVS or production configuration is accessed. */
#include "emqtt.h"
#include "mqtt_client.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_event_handler_t handler;
static void *handler_arg;
static bool started;
static unsigned stopped;
static int client_storage;
static emqtt_event_t out;

static esp_mqtt_client_handle_t client(void)
{
    return (esp_mqtt_client_handle_t)&client_storage;
}

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg)
{
    assert(cfg);
    return client();
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c,
    esp_mqtt_event_id_t id, esp_event_handler_t cb, void *arg)
{
    assert(c == client() && id == MQTT_EVENT_ANY);
    handler = cb;
    handler_arg = arg;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c)
{
    assert(c == client() && !started);
    started = true;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c)
{
    assert(c == client() && started);
    started = false;
    stopped++;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c)
{
    assert(c == client() && !started);
    return ESP_OK;
}

int esp_mqtt_client_subscribe_multiple(esp_mqtt_client_handle_t c,
    const esp_mqtt_topic_t *topics, int count)
{
    assert(c == client() && topics && count == 1);
    return 1;
}

int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t c, const char *topic)
{
    (void)c; (void)topic;
    return 2;
}

int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic,
    const char *data, int length, int qos, int retain, bool store)
{
    (void)c; (void)topic; (void)data; (void)length;
    (void)qos; (void)retain; (void)store;
    return 3;
}

int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t c)
{
    assert(c == client());
    return 0;
}

static void emit(esp_mqtt_event_id_t id, esp_mqtt_event_t event)
{
    assert(started);
    handler(handler_arg, "test", id, &event);
}

static void ready(emqtt_runtime_t *r)
{
    assert(emqtt_start(r, true, true) == ESP_OK);
    emit(MQTT_EVENT_CONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &out) && out.kind == EMQTT_EVENT_CONNECTED);
    char grant = 1;
    emit(MQTT_EVENT_SUBSCRIBED,
        (esp_mqtt_event_t){.msg_id = 1, .data = &grant, .data_len = 1});
    assert(emqtt_poll(r, &out) && out.kind == EMQTT_EVENT_READY);
}

void app_main(void)
{
    emqtt_config_t *cfg = calloc(1, sizeof(*cfg));
    assert(cfg);
    strcpy(cfg->hostname, "test.invalid");
    cfg->port = 1883;
    strcpy(cfg->client_id, "iram-overflow-test");
    strcpy(cfg->will_topic, "test/status");
    cfg->subscription_count = 1;
    strcpy(cfg->subscriptions[0].topic, "test/in");
    cfg->subscriptions[0].qos = 1;
    emqtt_runtime_t *r = NULL;
    assert(emqtt_create(cfg, &r) == ESP_OK);
    free(cfg);
    assert(esp_ptr_in_iram(r));
    printf("RUNTIME_PROBE start runtime_in_iram=%d address=%p\n",
           esp_ptr_in_iram(r), (void *)r);
    fflush(stdout);
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration < 4; iteration++) {
        ready(r);
        for (unsigned i = 0; i < 4; i++) {
            char bytes[2] = {(char)i, 42};
            emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){
                .msg_id = (int)i + 10, .topic = "test/in", .topic_len = 7,
                .data = bytes, .data_len = 2, .total_data_len = 2, .qos = 1});
        }
        const unsigned before = stopped;
        const bool got = emqtt_poll(r, &out);
        const bool ok = got && out.kind == EMQTT_EVENT_ERROR &&
            out.error == EMQTT_ERROR_QUEUE && emqtt_state(r) == EMQTT_FAILED &&
            stopped == before + 1;
        if (!ok) failures++;
        printf("RUNTIME_PROBE fourth_complete iteration=%u got=%d kind=%d "
               "error=%d state=%d stop_delta=%u pass=%d\n", iteration, got,
               (int)out.kind, (int)out.error, (int)emqtt_state(r), stopped - before, ok);
        fflush(stdout);
        assert(emqtt_stop(r) == ESP_OK);
    }
    assert(emqtt_destroy(r) == ESP_OK);
    printf("RUNTIME_PROBE done failures=%u\n", failures);
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
