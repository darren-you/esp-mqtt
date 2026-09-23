#include "emqtt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fixed SDK's Linux esp_timer component has headers but no implementation. */
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
    const char *port_text = getenv("EMQTT_TEST_PORT");
    const long port = port_text ? strtol(port_text, NULL, 10) : 0;
    if (port < 1 || port > 65535) fail(NULL, "missing_port");

    emqtt_config_t config = {.hostname = "127.0.0.1", .port = (uint16_t)port,
        .tls = false, .client_id = "linux-qos1-ack-test", .will_topic = "test/will",
        .subscriptions = {{.topic = "test/base", .qos = 1}}, .subscription_count = 1};
    emqtt_runtime_t *runtime = NULL;
    if (emqtt_create(&config, &runtime) != ESP_OK ||
        emqtt_start(runtime, true, true) != ESP_OK) fail(runtime, "start");

    int delayed_id = -1, reconnect_id = -1;
    int ready_count = 0, disconnected_count = 0;
    int delayed_ack_count = 0, reconnect_ack_count = 0, incoming_count = 0;
    int64_t quiet_until = 0;
    const int64_t deadline = esp_timer_get_time() + 50000000;

    while (esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            printf("TEST EVENT kind=%d error=%d id=%d dup=%d state=%d\n",
                   event.kind, event.error, event.message_id,
                   event.kind == EMQTT_EVENT_MESSAGE ? event.message.duplicate : -1,
                   emqtt_state(runtime));
            if (event.kind == EMQTT_EVENT_ERROR) {
                if (event.error != EMQTT_ERROR_TRANSPORT || reconnect_id <= 0 || disconnected_count)
                    fail(runtime, "unexpected_error");
                continue;
            }
            if (event.kind == EMQTT_EVENT_READY) {
                if (++ready_count == 1) {
                    if (emqtt_enqueue(runtime, "test/publish", "delayed-ack", 11,
                                      1, false, &delayed_id) != ESP_OK || delayed_id <= 0)
                        fail(runtime, "enqueue_delayed");
                    printf("TEST DELAYED id=%d\n", delayed_id);
                } else if (ready_count != 2 || disconnected_count != 1 || reconnect_id <= 0) {
                    fail(runtime, "unexpected_ready");
                }
            } else if (event.kind == EMQTT_EVENT_PUBACK) {
                if (event.message_id == delayed_id) {
                    if (++delayed_ack_count != 1 || reconnect_id > 0)
                        fail(runtime, "duplicate_delayed_puback");
                } else if (event.message_id == reconnect_id) {
                    if (++reconnect_ack_count != 1 || disconnected_count != 1)
                        fail(runtime, "duplicate_reconnect_puback");
                    quiet_until = esp_timer_get_time() + 750000;
                } else {
                    fail(runtime, "unknown_puback");
                }
            } else if (event.kind == EMQTT_EVENT_MESSAGE) {
                const emqtt_message_t *message = &event.message;
                if (delayed_ack_count != 1 || disconnected_count || incoming_count >= 2 ||
                    strcmp(message->topic, "test/base") || message->length != 8 ||
                    memcmp(message->payload, "incoming", 8) || message->message_id != 0x4455 ||
                    message->qos != 1 || message->retain ||
                    message->duplicate != (bool)incoming_count)
                    fail(runtime, "incoming_duplicate_semantics");
                if (++incoming_count == 2) {
                    if (emqtt_outbox_size(runtime) != 0 ||
                        emqtt_enqueue(runtime, "test/publish", "reconnect", 9,
                                      1, false, &reconnect_id) != ESP_OK ||
                        reconnect_id <= 0 || reconnect_id == delayed_id)
                        fail(runtime, "enqueue_reconnect");
                    printf("TEST RECONNECT id=%d\n", reconnect_id);
                }
            } else if (event.kind == EMQTT_EVENT_DISCONNECTED) {
                if (++disconnected_count != 1 || incoming_count != 2 || reconnect_id <= 0)
                    fail(runtime, "unexpected_disconnect");
            }
        }
        if (quiet_until && esp_timer_get_time() >= quiet_until) {
            if (ready_count != 2 || disconnected_count != 1 || delayed_ack_count != 1 ||
                reconnect_ack_count != 1 || incoming_count != 2 || emqtt_outbox_size(runtime) != 0)
                fail(runtime, "final_counts_or_outbox");
            printf("TEST PASS delayed_id=%d reconnect_id=%d incoming=%d pubacks=%d\n",
                   delayed_id, reconnect_id, incoming_count,
                   delayed_ack_count + reconnect_ack_count);
            if (emqtt_destroy(runtime) != ESP_OK) exit(2);
            exit(0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fail(runtime, "timeout");
}
