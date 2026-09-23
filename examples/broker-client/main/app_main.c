// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sample_inputs.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE
#error "The sample must not persist PHY calibration into an existing board's NVS"
#endif
#if CONFIG_EMQTT_PLAINTEXT_LAB
#error "The broker sample requires strict TLS"
#endif

static emqtt_config_t config;
static emqtt_event_t event_output;
static emqtt_runtime_t *runtime;
static atomic_bool wifi_started, wifi_ready, clock_synced;
static atomic_uint ip_generation, sync_seconds;
static bool wifi_wanted = true;
static bool manual_hold;
static bool first_start_attempted;
static unsigned cycle;

static uint32_t payload_crc32(const uint8_t *payload, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= payload[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (length >= capacity) return false;
    memcpy(destination, source, length + 1);
    return true;
}

static bool prepare_config(void)
{
    const size_t ssid_length = strlen(sample_wifi_ssid);
    const size_t password_length = strlen(sample_wifi_password);
    if (!ssid_length || ssid_length > 32 || password_length < 8 || password_length > 64 ||
        !sample_ntp_server[0] || !sample_dynamic_topic[0] || !sample_publish_topic[0]) return false;
    memset(&config, 0, sizeof(config));
    config.port = sample_broker_port;
    config.tls = true;
    config.will_qos = 1;
    config.will_retain = true;
    config.will_length = sizeof("offline") - 1;
    memcpy(config.will_payload, "offline", config.will_length);
    config.subscription_count = 1;
    config.subscriptions[0].qos = 1;
    if (!copy_text(config.hostname, sizeof(config.hostname), sample_broker_hostname) ||
        !copy_text(config.client_id, sizeof(config.client_id), sample_client_id) ||
        !copy_text(config.username, sizeof(config.username), sample_username) ||
        !copy_text(config.password, sizeof(config.password), sample_password) ||
        !copy_text(config.ca_pem, sizeof(config.ca_pem), sample_broker_ca_pem) ||
        !copy_text(config.will_topic, sizeof(config.will_topic), sample_will_topic) ||
        !copy_text(config.subscriptions[0].topic, sizeof(config.subscriptions[0].topic), sample_subscribe_topic)) return false;
    return emqtt_config_valid(&config, false) &&
        strcmp(sample_dynamic_topic, sample_subscribe_topic) != 0 &&
        emqtt_topic_valid(sample_dynamic_topic, strlen(sample_dynamic_topic), true) &&
        emqtt_topic_valid(sample_publish_topic, strlen(sample_publish_topic), false);
}

static bool trusted_time(void)
{
    unsigned now = (unsigned)(esp_timer_get_time() / 1000000);
    return atomic_load(&clock_synced) && now - atomic_load(&sync_seconds) < 7200u;
}

static void time_synced(struct timeval *time_value)
{
    if (!time_value || time_value->tv_sec < 1704067200) return;
    atomic_store(&sync_seconds, (unsigned)(esp_timer_get_time() / 1000000));
    atomic_store(&clock_synced, true);
}

static void network_event(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) atomic_store(&wifi_started, true);
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP))
        atomic_store(&wifi_ready, false);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) atomic_store(&wifi_started, false);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_fetch_add(&ip_generation, 1);
        atomic_store(&wifi_ready, true);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) atomic_store(&wifi_ready, false);
}

static esp_err_t start_network(void)
{
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK) return error;
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false;
    error = esp_wifi_init(&init);
    if (error != ESP_OK) return error;
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return error;
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, sample_wifi_ssid, strlen(sample_wifi_ssid));
    memcpy(wifi.sta.password, sample_wifi_password, strlen(sample_wifi_password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.sta.pmf_cfg.capable = true;
    if ((error = esp_wifi_set_config(WIFI_IF_STA, &wifi)) != ESP_OK ||
        (error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL)) != ESP_OK ||
        (error = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, NULL)) != ESP_OK) return error;
    return esp_wifi_start();
}

static void report(const char *phase)
{
    printf("EMQTT_SAMPLE cycle=%u phase=%s state=%d wifi=%u trusted=%u outbox=%d"
           " heap_free=%zu heap_min=%zu heap_largest=%zu time_ms=%" PRIu64 "\n",
           cycle, phase, runtime ? emqtt_state(runtime) : EMQTT_STOPPED,
           (unsigned)atomic_load(&wifi_ready), (unsigned)trusted_time(), runtime ? emqtt_outbox_size(runtime) : 0,
           heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (uint64_t)esp_timer_get_time() / 1000u);
}

static esp_err_t start_runtime(void)
{
    if (!atomic_load(&wifi_ready) || !trusted_time()) return ESP_ERR_INVALID_STATE;
    if (!runtime) {
        esp_err_t error = emqtt_create(&config, &runtime);
        if (error != ESP_OK) return error;
    }
    esp_err_t error = emqtt_stop(runtime);
    return error == ESP_OK ? emqtt_start(runtime, true, true) : error;
}

static void command(const char *name)
{
    esp_err_t error = ESP_OK;
    int message_id = -1;
    if (!strcmp(name, "stats")) { report("requested"); return; }
    if (!strcmp(name, "stop")) {
        manual_hold = true;
        if (runtime) error = emqtt_stop(runtime);
    } else if (!strcmp(name, "start")) {
        manual_hold = false;
        first_start_attempted = true;
        error = start_runtime();
    } else if (!strcmp(name, "cycle")) {
        if (runtime) error = emqtt_destroy(runtime);
        if (error == ESP_OK) {
            runtime = NULL;
            ++cycle;
            manual_hold = false;
            first_start_attempted = true;
            error = start_runtime();
        }
    } else if (!strcmp(name, "subscribe")) {
        error = runtime ? emqtt_subscribe(runtime, sample_dynamic_topic, 1) : ESP_ERR_INVALID_STATE;
    } else if (!strcmp(name, "unsubscribe")) {
        error = runtime ? emqtt_unsubscribe(runtime, sample_dynamic_topic) : ESP_ERR_INVALID_STATE;
    } else if (!strcmp(name, "publish") || !strcmp(name, "publish0") || !strcmp(name, "publish4k")) {
        static char large_payload[EMQTT_PAYLOAD_MAX];
        const bool large = !strcmp(name, "publish4k");
        if (large) memset(large_payload, 'A', sizeof(large_payload));
        error = runtime && emqtt_state(runtime) == EMQTT_READY ?
            emqtt_enqueue(runtime, sample_publish_topic, large ? large_payload : "ping",
                          large ? sizeof(large_payload) : 4u,
                          !strcmp(name, "publish0") ? 0 : 1, false, &message_id)
                        : ESP_ERR_INVALID_STATE;
    } else if (!strcmp(name, "fill")) {
        static char large_payload[EMQTT_PAYLOAD_MAX];
        unsigned accepted = 0;
        error = ESP_ERR_INVALID_STATE;
        if (runtime && emqtt_state(runtime) == EMQTT_DISCONNECTED) {
            memset(large_payload, 'A', sizeof(large_payload));
            for (unsigned i = 0; i < 8; ++i) {
                error = emqtt_enqueue(runtime, sample_publish_topic, large_payload,
                                      sizeof(large_payload), 1, false, &message_id);
                if (error != ESP_OK) { message_id = -1; break; }
                ++accepted;
            }
        }
        printf("EMQTT_SAMPLE fill_accepted=%u fill_error=%d outbox=%d\n",
               accepted, error, runtime ? emqtt_outbox_size(runtime) : -1);
    } else if (!strcmp(name, "wifi_down")) {
        wifi_wanted = false;
        error = esp_wifi_stop();
    } else if (!strcmp(name, "wifi_up")) {
        wifi_wanted = true;
        error = esp_wifi_start();
    } else {
        puts("EMQTT_SAMPLE command_error=unknown");
        return;
    }
    printf("EMQTT_SAMPLE command=%s error=%d message_id=%d cycle=%u\n", name, error, message_id, cycle);
    if (!strcmp(name, "cycle")) report("cycle");
}

static void serial_input(void)
{
    static char line[32];
    static size_t used;
    static bool overflow;
    char bytes[32];
    int count = read(STDIN_FILENO, bytes, sizeof(bytes));
    for (int i = 0; i < count; ++i) {
        if (bytes[i] == '\n') {
            if (overflow) puts("EMQTT_SAMPLE command_error=too_long");
            else { line[used] = 0; command(line); }
            used = 0;
            overflow = false;
        } else if (bytes[i] != '\r') {
            if (used == sizeof(line) - 1 || bytes[i] < 32 || bytes[i] > 126) overflow = true;
            else if (!overflow) line[used++] = bytes[i];
        }
    }
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("ESP_MQTT_LAB_ONLY broker_client sdk=6.1 target=esp32c3");
    if (!prepare_config()) { puts("EMQTT_SAMPLE valid_private_inputs_required; no_network_started"); return; }
    usb_serial_jtag_vfs_use_nonblocking();
    esp_err_t error = start_network();
    if (error != ESP_OK) { printf("EMQTT_SAMPLE wifi_init_error=%d\n", error); return; }
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(sample_ntp_server);
    ntp.sync_cb = time_synced;
    ntp.start = false;
    error = esp_netif_sntp_init(&ntp);
    if (error != ESP_OK) { printf("EMQTT_SAMPLE sntp_init_error=%d\n", error); return; }
    unsigned last_ip = 0;
    uint64_t next_wifi_ms = 0, next_report_ms = 0;
    for (;;) {
        uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000u;
        if (wifi_wanted && atomic_load(&wifi_started) && !atomic_load(&wifi_ready) && now_ms >= next_wifi_ms) {
            printf("EMQTT_SAMPLE wifi_connect_error=%d\n", esp_wifi_connect());
            next_wifi_ms = now_ms + 5000u;
        }
        unsigned generation = atomic_load(&ip_generation);
        if (atomic_load(&wifi_ready) && last_ip != generation) {
            error = esp_netif_sntp_start();
            printf("EMQTT_SAMPLE sntp_start_error=%d\n", error);
            if (error != ESP_OK) return;
            last_ip = generation;
        }
        if (!first_start_attempted && !manual_hold && atomic_load(&wifi_ready) && trusted_time()) {
            first_start_attempted = true;
            error = start_runtime();
            printf("EMQTT_SAMPLE start_error=%d\n", error);
        }
        while (runtime && emqtt_poll(runtime, &event_output)) {
            const bool message = event_output.kind == EMQTT_EVENT_MESSAGE;
            printf("EMQTT_SAMPLE_EVENT kind=%d error=%d message_id=%d broker_code=%d tls_flags=%d"
                   " topic=%s length=%zu qos=%u retain=%u duplicate=%u crc32=%08" PRIx32 "\n",
                   event_output.kind, event_output.error, event_output.message_id,
                   event_output.broker_code, event_output.tls_flags,
                   message ? event_output.message.topic : "",
                   message ? event_output.message.length : 0u,
                   message ? (unsigned)event_output.message.qos : 0u,
                   message ? (unsigned)event_output.message.retain : 0u,
                   message ? (unsigned)event_output.message.duplicate : 0u,
                   message ? payload_crc32(event_output.message.payload, event_output.message.length) : 0u);
            if (event_output.kind == EMQTT_EVENT_READY) {
                int message_id = -1;
                error = emqtt_enqueue(runtime, config.will_topic, "online", 6, 1, true, &message_id);
                printf("EMQTT_SAMPLE online_error=%d message_id=%d\n", error, message_id);
            }
        }
        serial_input();
        if (now_ms >= next_report_ms) { report("periodic"); next_report_ms = now_ms + 5000u; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
