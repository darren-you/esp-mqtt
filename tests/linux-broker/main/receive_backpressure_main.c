// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static emqtt_event_t event;
static atomic_bool gate_open = true, armed;
static atomic_int read_bytes, close_after, raw_complete;
static atomic_llong gate_closed_us;
static size_t raw_offset;

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void require(bool valid, const char *reason)
{
    if (!valid) { printf("TEST FAIL %s\n", reason); exit(1); }
}

/* Only mqtt_client.c is redirected; bytes and waits use the real transport.
 * Closing the test gate after the actual read makes the partial-packet case
 * independent of host scheduling between Broker send and SDK receive. */
int emqtt_test_transport_read(esp_transport_handle_t transport, char *buffer, int len, int timeout)
{
    const int result = esp_transport_read(transport, buffer, len, timeout);
    if (result > 0 && atomic_load(&armed)) {
        int total = atomic_fetch_add(&read_bytes, result) + result;
        if (total >= atomic_load(&close_after)) {
            atomic_store(&armed, false);
            atomic_store(&gate_closed_us, esp_timer_get_time());
            atomic_store(&gate_open, false);
        }
    }
    return result;
}

static void delay_until(int64_t deadline)
{
    while (esp_timer_get_time() < deadline) vTaskDelay(1);
}

static void runtime_case(int port)
{
    emqtt_config_t config = {.hostname = "127.0.0.1", .port = port,
        .tls = false, .client_id = "linux-receive-backpressure", .will_topic = "test/will",
        .subscriptions = {{.topic = "test/base", .qos = 1}}, .subscription_count = 1};
    emqtt_runtime_t *runtime = NULL;
    require(emqtt_create(&config, &runtime) == ESP_OK, "runtime_create");
    require(emqtt_start(runtime, true, true) == ESP_OK, "runtime_start");
    int64_t deadline = esp_timer_get_time() + 5000000;
    bool ready = false;
    while (!ready && esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            require(event.kind != EMQTT_EVENT_ERROR && event.kind != EMQTT_EVENT_MESSAGE, "ready_event");
            if (event.kind == EMQTT_EVENT_READY) ready = true;
        }
        vTaskDelay(1);
    }
    require(ready, "runtime_ready");
    int id = -1;
    require(emqtt_enqueue(runtime, "test/phase", "burst", 5, 0, false, &id) == ESP_OK, "burst_signal");
    /* Do not poll the owner during the hold. Three complete messages must
     * remain queued while the fourth stays in the transport. */
    const int64_t held_at = esp_timer_get_time();
    delay_until(held_at + 500000);
    int held_id = -1;
    const int64_t enqueue_at = esp_timer_get_time();
    require(emqtt_enqueue(runtime, "test/phase", "held", 4, 1, false, &held_id) == ESP_OK, "paused_enqueue");
    require(esp_timer_get_time() - enqueue_at < 250000, "paused_enqueue_blocked");
    delay_until(held_at + 1500000);
    printf("TEST DRAIN at_us=%lld\n", (long long)esp_timer_get_time());
    int received = 0; bool acknowledged = false;
    deadline = esp_timer_get_time() + 5000000;
    while ((received < 4 || !acknowledged) && esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            if (event.kind == EMQTT_EVENT_PUBACK) {
                require(event.message_id == held_id && !acknowledged, "paused_puback");
                acknowledged = true;
            } else if (event.kind == EMQTT_EVENT_MESSAGE) {
                const emqtt_message_t *m = &event.message;
                require(received < 4 && m->message_id == 201 + received && m->length == 4096 &&
                    m->qos == 1 && !m->retain && !m->duplicate && !strcmp(m->topic, "test/base"), "burst_identity");
                for (size_t i = 0; i < m->length; ++i)
                    require(m->payload[i] == (unsigned char)(received + 1), "burst_bytes");
                ++received;
            } else require(false, "unexpected_runtime_event");
        }
        vTaskDelay(1);
    }
    require(received == 4 && acknowledged && emqtt_outbox_size(runtime) == 0, "burst_completion");
    require(emqtt_enqueue(runtime, "test/phase", "stop", 4, 0, false, &id) == ESP_OK, "stop_signal");
    delay_until(esp_timer_get_time() + 1000000);
    const int64_t stop_at = esp_timer_get_time();
    require(emqtt_stop(runtime) == ESP_OK, "paused_stop");
    const int64_t stop_us = esp_timer_get_time() - stop_at;
    require(stop_us < 1000000, "paused_stop_blocked");
    require(emqtt_destroy(runtime) == ESP_OK, "runtime_destroy");
    printf("TEST PASS receive-backpressure runtime messages=%d stop_us=%lld\n", received, (long long)stop_us);
}

static bool receive_ready(void *context)
{
    require(context == &gate_open, "gate_context");
    return atomic_load(&gate_open);
}

static void raw_event(void *context, esp_event_base_t base, int32_t type, void *data)
{
    (void)base;
    esp_mqtt_client_handle_t client = context;
    esp_mqtt_event_t *e = data;
    if (type == MQTT_EVENT_CONNECTED) {
        require(esp_mqtt_client_subscribe(client, "test/base", 1) > 0, "raw_subscribe");
    } else if (type == MQTT_EVENT_SUBSCRIBED) {
        if (atomic_load(&raw_complete) == 0) atomic_store(&armed, true);
    } else if (type == MQTT_EVENT_DATA) {
        const int complete = atomic_load(&raw_complete);
        require(complete < 3 && e->msg_id == 301 + complete && e->total_data_len == 4096 &&
            e->current_data_offset == (int)raw_offset && e->qos == 1 && !e->retain && !e->dup, "raw_identity");
        for (int i = 0; i < e->data_len; ++i)
            require((unsigned char)e->data[i] == (unsigned char)(complete + 1), "raw_bytes");
        raw_offset += e->data_len;
        if (raw_offset == 4096) { raw_offset = 0; atomic_store(&raw_complete, complete + 1); }
    } else if (type == MQTT_EVENT_ERROR || type == MQTT_EVENT_DISCONNECTED) {
        require(false, "raw_connection_error");
    }
}

static void raw_case(int port, int cutoff)
{
    atomic_store(&close_after, cutoff);
    esp_mqtt_client_config_t config = {.broker.address.hostname = "127.0.0.1",
        .broker.address.port = port, .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = "linux-receive-gate-partial", .buffer.size = 512,
        .session.keepalive = 4, .network.timeout_ms = 3000, .network.disable_auto_reconnect = true};
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    require(client != NULL, "raw_init");
    require(emqtt_client_set_receive_ready_callback(client, receive_ready, &gate_open) == ESP_OK, "gate_install");
    require(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, raw_event, client) == ESP_OK, "raw_register");
    require(esp_mqtt_client_start(client) == ESP_OK, "raw_start");
    require(emqtt_client_set_receive_ready_callback(client, NULL, NULL) == ESP_ERR_INVALID_STATE, "running_setter");
    int64_t deadline = esp_timer_get_time() + 5000000;
    while (atomic_load(&raw_complete) < 1 && esp_timer_get_time() < deadline) vTaskDelay(1);
    require(atomic_load(&raw_complete) == 1 && !atomic_load(&gate_open) &&
        atomic_load(&gate_closed_us) > 0, "partial_not_completed");
    require(esp_mqtt_client_enqueue(client, "test/phase", "paused", 6, 0, false, true) >= 0, "raw_paused_enqueue");
    delay_until(esp_timer_get_time() + 2500000);
    require(atomic_load(&raw_complete) == 1, "gate_read_next_packet");
    printf("TEST RAW_RELEASE at_us=%lld cutoff=%d actual_read=%d\n", (long long)esp_timer_get_time(), cutoff, atomic_load(&read_bytes));
    atomic_store(&gate_open, true);
    deadline = esp_timer_get_time() + 5000000;
    while (atomic_load(&raw_complete) < 2 && esp_timer_get_time() < deadline) vTaskDelay(1);
    require(atomic_load(&raw_complete) == 2, "raw_resume");
    require(esp_mqtt_client_stop(client) == ESP_OK, "raw_stop");
    require(emqtt_client_set_receive_ready_callback(client, NULL, NULL) == ESP_OK, "gate_remove");
    atomic_store(&gate_open, false);
    require(esp_mqtt_client_start(client) == ESP_OK, "raw_restart");
    deadline = esp_timer_get_time() + 5000000;
    while (atomic_load(&raw_complete) < 3 && esp_timer_get_time() < deadline) vTaskDelay(1);
    require(atomic_load(&raw_complete) == 3, "unset_callback_behavior");
    require(esp_mqtt_client_stop(client) == ESP_OK && esp_mqtt_client_destroy(client) == ESP_OK, "raw_destroy");
    printf("TEST PASS receive-backpressure partial cutoff=%d messages=3\n", cutoff);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *text = getenv("EMQTT_TEST_PORT");
    const int port = text ? atoi(text) : 0;
    require(port > 0 && port <= 65535, "port");
    const char *mode = getenv("EMQTT_GATE_MODE");
    require(mode != NULL, "mode");
    if (!strcmp(mode, "runtime")) runtime_case(port);
    else raw_case(port, atoi(mode));
    exit(0);
}
