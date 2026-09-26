// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 固定 SDK 的 Linux esp_timer 只有头文件，没有实现。 */
int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static emqtt_config_t config;
static emqtt_event_t event;

static void fail(emqtt_runtime_t *runtime, const char *reason)
{
    printf("TEST FAIL %s\n", reason);
    if (runtime) (void)emqtt_destroy(runtime);
    exit(1);
}

static void load_ca(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) fail(NULL, "ca_open");
    const size_t length = fread(config.ca_pem, 1, sizeof(config.ca_pem), file);
    if (ferror(file) || !feof(file) || length == sizeof(config.ca_pem)) {
        fclose(file);
        fail(NULL, "ca_length");
    }
    fclose(file);
    config.ca_pem[length] = '\0';
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *port_text = getenv("EMQTT_TEST_PORT");
    const char *host = getenv("EMQTT_TEST_HOST");
    const char *ca_path = getenv("EMQTT_TEST_CA_FILE");
    const char *mode = getenv("EMQTT_TEST_MODE");
    const long port = port_text ? strtol(port_text, NULL, 10) : 0;
    if (!host || !ca_path || !mode || port < 1 || port > 65535 ||
        (strcmp(mode, "valid") && strcmp(mode, "reject"))) fail(NULL, "inputs");
    if (strlen(host) >= sizeof(config.hostname)) fail(NULL, "hostname_length");
    strcpy(config.hostname, host);
    config.port = (uint16_t)port;
    config.tls = true;
    strcpy(config.client_id, "linux-tls-real-core");
    strcpy(config.will_topic, "test/tls/will");
    strcpy(config.subscriptions[0].topic, "test/tls/in");
    config.subscriptions[0].qos = 1;
    config.subscription_count = 1;
    load_ca(ca_path);

    emqtt_runtime_t *runtime = NULL;
    if (emqtt_create(&config, &runtime) != ESP_OK) fail(runtime, "create");
    if (emqtt_start(runtime, true, false) != ESP_ERR_EMQTT_TIME_REQUIRED)
        fail(runtime, "trusted_time_gate");
    if (emqtt_start(runtime, true, true) != ESP_OK) fail(runtime, "start");
    printf("TEST START mode=%s host=%s port=%ld\n", mode, host, port);

    bool sent = false;
    int publish_id = -1;
    const int64_t deadline = esp_timer_get_time() + 15000000;
    while (esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            printf("TEST EVENT kind=%d error=%d id=%d flags=%d state=%d\n",
                   event.kind, event.error, event.message_id, event.tls_flags, emqtt_state(runtime));
            if (!strcmp(mode, "reject")) {
                if (event.kind == EMQTT_EVENT_READY) fail(runtime, "unexpected_ready");
                if (event.kind == EMQTT_EVENT_ERROR && event.error == EMQTT_ERROR_TLS) {
                    if (emqtt_destroy(runtime) != ESP_OK) exit(2);
                    puts("TEST PASS rejected_tls_identity");
                    exit(0);
                }
            } else if (event.kind == EMQTT_EVENT_READY && !sent) {
                if (emqtt_enqueue(runtime, "test/tls/out", "tls-proof", 9, 1, false, &publish_id) != ESP_OK)
                    fail(runtime, "enqueue");
                sent = true;
                printf("TEST PUBLISH id=%d\n", publish_id);
            } else if (event.kind == EMQTT_EVENT_PUBACK && sent && event.message_id == publish_id) {
                if (emqtt_destroy(runtime) != ESP_OK) exit(2);
                puts("TEST PASS trusted_time_tls_suback_puback");
                exit(0);
            } else if (event.kind == EMQTT_EVENT_ERROR) {
                fail(runtime, "unexpected_error");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fail(runtime, "timeout");
}
