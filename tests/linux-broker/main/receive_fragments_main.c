// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static emqtt_event_t event;

static void fail(emqtt_runtime_t *runtime, const char *reason)
{
    printf("TEST FAIL %s\n", reason);
    if (runtime) (void)emqtt_destroy(runtime);
    exit(1);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *value = getenv("EMQTT_TEST_PORT");
    const long port = value ? strtol(value, NULL, 10) : 0;
    if (port < 1 || port > 65535) fail(NULL, "port");
    emqtt_config_t config = {.hostname = "127.0.0.1", .port = (uint16_t)port,
        .tls = false, .client_id = "linux-receive-fragments", .will_topic = "test/will",
        .subscriptions = {{.topic = "test/base", .qos = 1}}, .subscription_count = 1};
    emqtt_runtime_t *runtime = NULL;
    if (emqtt_create(&config, &runtime) != ESP_OK ||
        emqtt_start(runtime, true, true) != ESP_OK) fail(runtime, "start");
    int ready = 0, disconnected = 0, received = 0, enqueued = 0;
    int64_t next_enqueue = 0, maximum_enqueue_us = 0, completed_at = 0;
    const int64_t deadline = esp_timer_get_time() + 25000000;
    while (esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            if (event.kind == EMQTT_EVENT_READY) {
                if (++ready > 4 || (ready > 1 && (received != ready + 1 || disconnected != ready - 1)))
                    fail(runtime, "unexpected_ready");
            } else if (event.kind == EMQTT_EVENT_DISCONNECTED) {
                if (++disconnected > 3 || received != disconnected + 2)
                    fail(runtime, "early_partial_disconnect");
            } else if (event.kind == EMQTT_EVENT_ERROR) {
                if (event.error != EMQTT_ERROR_TRANSPORT || (received < 3 || received > 5))
                    fail(runtime, "unexpected_error");
            } else if (event.kind == EMQTT_EVENT_MESSAGE) {
                const emqtt_message_t *message = &event.message;
                const int ids[] = {201, 202, 203, 205, 207, 209};
                const size_t lengths[] = {4096, 128, 4096, 128, 128, 128};
                if (received >= 6 || message->message_id != ids[received] ||
                    message->length != lengths[received] || message->qos != 1 ||
                    message->retain || message->duplicate || strcmp(message->topic, "test/base"))
                    fail(runtime, "message_identity");
                for (size_t i = 0; i < message->length; ++i)
                    if (message->payload[i] != (uint8_t)(received + 1))
                        fail(runtime, "message_bytes");
                printf("TEST RECEIVED id=%d bytes=%zu\n", ids[received], lengths[received]);
                if (++received == 6) completed_at = esp_timer_get_time();
            }
        }
        const int64_t now = esp_timer_get_time();
        if (emqtt_state(runtime) == EMQTT_READY && now >= next_enqueue && !completed_at) {
            int id = -1;
            const int64_t begin = esp_timer_get_time();
            if (emqtt_enqueue(runtime, "test/heartbeat", "live", 4, 0, false, &id) != ESP_OK)
                fail(runtime, "enqueue");
            const int64_t elapsed = esp_timer_get_time() - begin;
            if (elapsed > maximum_enqueue_us) maximum_enqueue_us = elapsed;
            if (elapsed > 250000) fail(runtime, "partial_read_blocked_enqueue");
            ++enqueued;
            next_enqueue = now + 100000;
        }
        if (completed_at && now - completed_at > 300000) {
            if (ready != 4 || disconnected != 3 || enqueued < 10 || emqtt_outbox_size(runtime))
                fail(runtime, "final_state");
            if (emqtt_destroy(runtime) != ESP_OK) fail(NULL, "destroy");
            printf("TEST PASS receive-fragments ready=%d disconnected=%d messages=%d enqueued=%d max_enqueue_us=%lld\n",
                   ready, disconnected, received, enqueued, (long long)maximum_enqueue_us);
            exit(0);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    fail(runtime, "test_deadline");
}
