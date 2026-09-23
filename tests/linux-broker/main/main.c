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

static void fail(emqtt_runtime_t *runtime, const char *reason)
{
    printf("TEST FAIL %s\n", reason);
    if (runtime) (void)emqtt_destroy(runtime);
    exit(1);
}

static void run_unsubscribe_only(emqtt_runtime_t *runtime)
{
    enum { INITIAL, CONFIRM_DYNAMIC, HELD_UNSUBACK, RECONNECTED, WAIT_PUBACK } phase = INITIAL;
    int message_id = -1;
    bool published = false;
    const int64_t deadline = esp_timer_get_time() + 35000000;
    while (esp_timer_get_time() < deadline) {
        emqtt_event_t event;
        while (emqtt_poll(runtime, &event)) {
            printf("TEST EVENT kind=%d error=%d id=%d phase=%d state=%d\n",
                   event.kind, event.error, event.message_id, phase, emqtt_state(runtime));
            if (event.kind == EMQTT_EVENT_ERROR &&
                !(phase == HELD_UNSUBACK && event.error == EMQTT_ERROR_TRANSPORT))
                fail(runtime, "runtime_error");
            if (event.kind == EMQTT_EVENT_PUBACK && event.message_id == message_id)
                published = true;
            if (phase == INITIAL && event.kind == EMQTT_EVENT_READY) {
                if (emqtt_subscribe(runtime, "test/dynamic", 1) != ESP_OK)
                    fail(runtime, "dynamic_subscribe");
                phase = CONFIRM_DYNAMIC;
            } else if (phase == CONFIRM_DYNAMIC && event.kind == EMQTT_EVENT_READY) {
                if (emqtt_unsubscribe(runtime, "test/dynamic") != ESP_OK ||
                    emqtt_enqueue(runtime, "test/publish", "second", 6, 1, false, &message_id) != ESP_OK)
                    fail(runtime, "dynamic_unsubscribe");
                printf("TEST SECOND id=%d\n", message_id);
                phase = HELD_UNSUBACK;
            } else if (phase == HELD_UNSUBACK && event.kind == EMQTT_EVENT_DISCONNECTED) {
                phase = RECONNECTED;
            } else if (phase == RECONNECTED && event.kind == EMQTT_EVENT_READY) {
                phase = WAIT_PUBACK;
            }
        }
        if (phase == WAIT_PUBACK && published) {
            printf("TEST PASS second_id=%d\n", message_id);
            if (emqtt_destroy(runtime) != ESP_OK) exit(2);
            exit(0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fail(runtime, "timeout");
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *port_text = getenv("EMQTT_TEST_PORT");
    const long port = port_text ? strtol(port_text, NULL, 10) : 0;
    if (port < 1 || port > 65535) fail(NULL, "missing_port");

    emqtt_config_t config = {.hostname = "127.0.0.1", .port = (uint16_t)port,
        .tls = false, .client_id = "linux-real-core-test", .will_topic = "test/will",
        .subscriptions = {{.topic = "test/base", .qos = 1}}, .subscription_count = 1};
    emqtt_runtime_t *runtime = NULL;
    if (emqtt_create(&config, &runtime) != ESP_OK ||
        emqtt_start(runtime, true, true) != ESP_OK) fail(runtime, "start");
    printf("TEST START real_core=1 port=%ld\n", port);
    const char *scenario = getenv("EMQTT_SCENARIO");
    if (scenario && !strcmp(scenario, "unsub-only")) run_unsubscribe_only(runtime);

    enum { INITIAL_READY, HELD_SUBACK, WAIT_FIRST_READY, WAIT_FIRST_PUBACK,
           WAIT_DYNAMIC_READY, HELD_UNSUBACK, WAIT_SECOND_READY, WAIT_SECOND_PUBACK } phase = INITIAL_READY;
    int first_id = -1, second_id = -1;
    bool first_acked = false, second_acked = false;
    const int64_t deadline = esp_timer_get_time() + 55000000;

    while (esp_timer_get_time() < deadline) {
        emqtt_event_t event;
        while (emqtt_poll(runtime, &event)) {
            printf("TEST EVENT kind=%d error=%d id=%d phase=%d state=%d\n",
                   event.kind, event.error, event.message_id, phase, emqtt_state(runtime));
            if (event.kind == EMQTT_EVENT_ERROR &&
                !(event.error == EMQTT_ERROR_TRANSPORT &&
                  (phase == HELD_SUBACK || phase == HELD_UNSUBACK)))
                fail(runtime, "runtime_error");
            if (event.kind == EMQTT_EVENT_PUBACK) {
                if (event.message_id == first_id) first_acked = true;
                if (event.message_id == second_id) second_acked = true;
            }
            switch (phase) {
            case INITIAL_READY:
                if (event.kind != EMQTT_EVENT_READY) break;
                if (emqtt_subscribe(runtime, "test/dynamic", 1) != ESP_OK ||
                    emqtt_enqueue(runtime, "test/publish", "first", 5, 1, false, &first_id) != ESP_OK)
                    fail(runtime, "first_operation");
                printf("TEST FIRST id=%d\n", first_id);
                phase = HELD_SUBACK;
                break;
            case HELD_SUBACK:
                if (event.kind == EMQTT_EVENT_DISCONNECTED) phase = WAIT_FIRST_READY;
                break;
            case WAIT_FIRST_READY:
                if (event.kind == EMQTT_EVENT_READY) phase = WAIT_FIRST_PUBACK;
                break;
            case WAIT_FIRST_PUBACK:
                break;
            case WAIT_DYNAMIC_READY:
                if (event.kind != EMQTT_EVENT_READY) break;
                if (emqtt_unsubscribe(runtime, "test/dynamic") != ESP_OK ||
                    emqtt_enqueue(runtime, "test/publish", "second", 6, 1, false, &second_id) != ESP_OK)
                    fail(runtime, "second_operation");
                printf("TEST SECOND id=%d\n", second_id);
                phase = HELD_UNSUBACK;
                break;
            case HELD_UNSUBACK:
                if (event.kind == EMQTT_EVENT_DISCONNECTED) phase = WAIT_SECOND_READY;
                break;
            case WAIT_SECOND_READY:
                if (event.kind == EMQTT_EVENT_READY) phase = WAIT_SECOND_PUBACK;
                break;
            case WAIT_SECOND_PUBACK:
                break;
            }
        }
        if (phase == WAIT_FIRST_PUBACK && first_acked) {
            if (emqtt_subscribe(runtime, "test/dynamic", 1) != ESP_OK)
                fail(runtime, "dynamic_confirmed_subscription");
            phase = WAIT_DYNAMIC_READY;
        }
        if (phase == WAIT_SECOND_PUBACK && second_acked) {
            printf("TEST PASS first_id=%d second_id=%d\n", first_id, second_id);
            if (emqtt_destroy(runtime) != ESP_OK) exit(2);
            exit(0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fail(runtime, "timeout");
}
