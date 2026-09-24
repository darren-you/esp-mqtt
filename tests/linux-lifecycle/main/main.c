#include "emqtt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool fail_init, fail_add;
static atomic_uint injected, released;
static esp_transport_handle_t unowned_transport;
static atomic_int secondary_stop_result;

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* 只替换 MQTT 对 SDK 的四个调用点；任务、事件、核心与队列均为真实实现。 */
esp_transport_handle_t emqtt_test_tcp_init(void)
{
    if (fail_init) {
        atomic_fetch_add(&injected, 1);
        return NULL;
    }
    return esp_transport_tcp_init();
}

esp_transport_handle_t emqtt_test_ssl_init(void)
{
    if (fail_init) {
        atomic_fetch_add(&injected, 1);
        return NULL;
    }
    return esp_transport_ssl_init();
}

esp_err_t emqtt_test_list_add(esp_transport_list_handle_t list,
                            esp_transport_handle_t transport, const char *scheme)
{
    if (fail_add) {
        unowned_transport = transport;
        atomic_fetch_add(&injected, 1);
        return ESP_ERR_NO_MEM;
    }
    return esp_transport_list_add(list, transport, scheme);
}

esp_err_t emqtt_test_transport_destroy(esp_transport_handle_t transport)
{
    if (transport && transport == unowned_transport) {
        unowned_transport = NULL;
        atomic_fetch_add(&released, 1);
    }
    return esp_transport_destroy(transport);
}

static void require(bool valid, const char *reason)
{
    if (!valid) {
        printf("TEST FAIL %s injected=%u released=%u\n", reason,
               atomic_load(&injected), atomic_load(&released));
        exit(1);
    }
}

static void secondary_stop(void *arg)
{
    atomic_store(&secondary_stop_result, esp_mqtt_client_stop(arg));
    vTaskDelete(NULL);
}

static void concurrent_stop(void)
{
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        const esp_mqtt_client_config_t config = {
            .broker.address = {.hostname = "127.0.0.1", .port = 1, .transport = MQTT_TRANSPORT_OVER_TCP},
            .credentials.client_id = "linux-concurrent-stop",
            .task = {.priority = 5, .stack_size = 6144},
        };
        esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
        require(client != NULL, "core_create");
        require(esp_mqtt_client_start(client) == ESP_OK, "core_start");
        atomic_store(&secondary_stop_result, -999);
        require(xTaskCreate(secondary_stop, "second_stop", 4096, client, 9, NULL) == pdPASS,
                "second_stop_task");
        /* owner 等待退出时，次级 stopper 先于 MQTT worker 调度。 */
        require(esp_mqtt_client_stop(client) == ESP_OK, "core_stop");
        const int64_t deadline = esp_timer_get_time() + 1000000;
        while (atomic_load(&secondary_stop_result) == -999 && esp_timer_get_time() < deadline)
            vTaskDelay(pdMS_TO_TICKS(10));
        require(atomic_load(&secondary_stop_result) == ESP_FAIL, "parallel_stop_not_rejected");
        require(esp_mqtt_client_destroy(client) == ESP_OK, "core_destroy");
    }
    printf("TEST PASS scenario=concurrent-stop cycles=100\n");
    exit(0);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
    const char *scenario = getenv("EMQTT_LIFECYCLE_SCENARIO");
    require(scenario != NULL, "missing_scenario");
    /* 高于 MQTT 的 5，使 start 返回时 worker 尚未获调度。 */
    vTaskPrioritySet(NULL, 10);
    if (!strcmp(scenario, "concurrent-stop")) concurrent_stop();
    const bool tls = !strncmp(scenario, "tls-", 4);
    fail_init = !strcmp(scenario, "init-oom") || !strcmp(scenario, "tls-init-oom");
    fail_add = !strcmp(scenario, "register-oom") || !strcmp(scenario, "tls-register-oom");
    require(fail_init || fail_add || !strcmp(scenario, "immediate-stop"), "invalid_scenario");
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        emqtt_config_t config = {.hostname = "127.0.0.1", .port = 1,
            .client_id = "linux-lifecycle", .will_topic = "test/will"};
        if (tls) {
            config.tls = true;
            strcpy(config.ca_pem, "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----");
        }
        emqtt_runtime_t *runtime = NULL;
        require(emqtt_create(&config, &runtime) == ESP_OK, "create");
        require(emqtt_start(runtime, true, true) == ESP_OK, "start");
        if (fail_init || fail_add) {
            const unsigned expected = cycle + 1;
            const int64_t deadline = esp_timer_get_time() + 1000000;
            while (atomic_load(&injected) < expected && esp_timer_get_time() < deadline)
                vTaskDelay(pdMS_TO_TICKS(10));
            require(atomic_load(&injected) == expected, "fault_not_reached");
            /* 允许 worker 完成退出，随后 owner 必须仍能回收单实例。 */
            vTaskDelay(pdMS_TO_TICKS(10));
            emqtt_event_t event;
            require(emqtt_poll(runtime, &event) && event.kind == EMQTT_EVENT_ERROR &&
                    event.error == EMQTT_ERROR_TRANSPORT, "missing_start_error");
            require(emqtt_state(runtime) == EMQTT_FAILED, "failed_state");
        }
        require(emqtt_stop(runtime) == ESP_OK, "stop");
        require(!fail_add || atomic_load(&released) == cycle + 1, "transport_leaked");
        /* 复用同一实例时，旧 STOPPED 位不能让新任务尚未退出便释放内存。 */
        require(emqtt_start(runtime, true, true) == ESP_OK, "restart");
        require(emqtt_stop(runtime) == ESP_OK, "stop_after_restart");
        require(emqtt_destroy(runtime) == ESP_OK, "destroy");
    }
    printf("TEST PASS scenario=%s cycles=100 injected=%u released=%u\n", scenario,
           atomic_load(&injected), atomic_load(&released));
    exit(0);
}
