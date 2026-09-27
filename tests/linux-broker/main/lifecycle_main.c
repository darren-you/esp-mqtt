// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

/* The fixed SDK's Linux esp_timer component has headers but no implementation. */
int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static emqtt_config_t config;
static emqtt_event_t event;

static void fail(emqtt_runtime_t *runtime, int cycle, const char *reason)
{
    printf("TEST FAIL cycle=%d reason=%s state=%d\n", cycle, reason,
           runtime ? emqtt_state(runtime) : -1);
    if (runtime) (void)emqtt_destroy(runtime);
    exit(1);
}

static void resource_sample(int cycle)
{
    size_t rss_bytes = 0, malloc_in_use_bytes = 0;
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        rss_bytes = (size_t)info.resident_size;
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats);
    malloc_in_use_bytes = stats.size_in_use;
#endif
    int fd_0_255 = 0;
    for (int fd = 0; fd < 256; ++fd)
        if (fcntl(fd, F_GETFD) != -1) ++fd_0_255;
    printf("TEST CYCLE cycle=%d rss_bytes=%zu malloc_in_use_bytes=%zu fd_0_255=%d\n",
           cycle, rss_bytes, malloc_in_use_bytes, fd_0_255);
}

static void run_cycle(int cycle)
{
    memset(&config, 0, sizeof(config));
    strcpy(config.hostname, "127.0.0.1");
    const char *port_text = getenv("EMQTT_TEST_PORT");
    const long port = port_text ? strtol(port_text, NULL, 10) : 0;
    if (port < 1 || port > 65535) fail(NULL, cycle, "port");
    config.port = (uint16_t)port;
    strcpy(config.client_id, "linux-lifecycle-real-core");
    strcpy(config.will_topic, "test/lifecycle/will");
    strcpy(config.subscriptions[0].topic, "test/lifecycle/base");
    config.subscriptions[0].qos = 1;
    config.subscription_count = 1;

    emqtt_runtime_t *runtime = NULL;
    if (emqtt_create(&config, &runtime) != ESP_OK || emqtt_start(runtime, true, true) != ESP_OK)
        fail(runtime, cycle, "start");

    enum { INITIAL_READY, DYNAMIC_READY, WAIT_PUBACK, WAIT_DISCONNECT,
           RECONNECT_READY, WAIT_UNSUBACK } phase = INITIAL_READY;
    int publish_id = -1;
    const int64_t deadline = esp_timer_get_time() + 20000000;
    while (esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            if (event.kind == EMQTT_EVENT_ERROR) {
                if (event.error == EMQTT_ERROR_TRANSPORT &&
                    (phase == WAIT_DISCONNECT || phase == RECONNECT_READY)) continue;
                fail(runtime, cycle, "event_error");
            }
            if (phase == INITIAL_READY && event.kind == EMQTT_EVENT_READY) {
                if (emqtt_subscribe(runtime, "test/lifecycle/dynamic", 1) != ESP_OK)
                    fail(runtime, cycle, "dynamic_subscribe");
                phase = DYNAMIC_READY;
            } else if (phase == DYNAMIC_READY && event.kind == EMQTT_EVENT_READY) {
                char payload[32];
                const int length = snprintf(payload, sizeof(payload), "cycle=%d", cycle);
                if (length < 0 || length >= (int)sizeof(payload) ||
                    emqtt_enqueue(runtime, "test/lifecycle/out", payload, (size_t)length,
                                  1, false, &publish_id) != ESP_OK || publish_id <= 0)
                    fail(runtime, cycle, "qos1_enqueue");
                phase = WAIT_PUBACK;
            } else if (phase == WAIT_PUBACK && event.kind == EMQTT_EVENT_PUBACK) {
                if (event.message_id != publish_id) fail(runtime, cycle, "puback_id");
                phase = WAIT_DISCONNECT;
            } else if (phase == WAIT_DISCONNECT && event.kind == EMQTT_EVENT_DISCONNECTED) {
                phase = RECONNECT_READY;
            } else if (phase == RECONNECT_READY && event.kind == EMQTT_EVENT_READY) {
                if (emqtt_unsubscribe(runtime, "test/lifecycle/dynamic") != ESP_OK)
                    fail(runtime, cycle, "dynamic_unsubscribe");
                phase = WAIT_UNSUBACK;
            } else if (phase == WAIT_UNSUBACK && event.kind == EMQTT_EVENT_UNSUBSCRIBED) {
                if (emqtt_outbox_size(runtime) != 0) fail(runtime, cycle, "outbox_not_empty");
                if (emqtt_stop(runtime) != ESP_OK) fail(runtime, cycle, "stop");
                if (emqtt_destroy(runtime) != ESP_OK) fail(NULL, cycle, "destroy");
                resource_sample(cycle);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    fail(runtime, cycle, "timeout");
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *count_text = getenv("EMQTT_TEST_COUNT");
    const long count = count_text ? strtol(count_text, NULL, 10) : 0;
    if (count < 1 || count > 100) fail(NULL, 0, "count");
    for (int cycle = 1; cycle <= count; ++cycle) run_cycle(cycle);
    printf("TEST PASS cycles=%ld\n", count);
    exit(0);
}
