// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "emqtt_receive_internal.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_FREERTOS_UNICORE) && \
    defined(CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY)
#include "esp_heap_caps.h"
#define EMQTT_STORAGE_IRAM_8BIT 1
#endif
#endif

#if !CONFIG_MQTT_REPORT_DELETED_MESSAGES
#error "MQTT outbox expiry must report deleted messages"
#endif
#if !CONFIG_MBEDTLS_HAVE_TIME_DATE
#error "MQTT TLS requires certificate validity-period checks"
#endif
#if defined(CONFIG_MQTT_EVENT_QUEUE_SIZE) && CONFIG_MQTT_EVENT_QUEUE_SIZE != 1
#error "emqtt event copies require synchronous ESP-MQTT dispatch (event queue size 1)"
#endif
#if CONFIG_MQTT_DISABLE_API_LOCKS
#error "emqtt requires ESP-MQTT API locks for SDK worker and owner operations"
#endif

#define NOTICE_CAPACITY 16
#define MESSAGE_SLOTS 3
#define NOTICE_SUBACK 100

typedef struct {
    int kind, message_id, slot, broker_code, tls_flags;
    int64_t arrived_us;
    emqtt_error_t error;
    uint8_t suback[EMQTT_SUBSCRIPTIONS_MAX];
    size_t suback_count;
} notice_t;

struct emqtt_runtime {
    esp_mqtt_client_handle_t client;
    TaskHandle_t owner;
    QueueHandle_t notices, free_slots;
    /* The SDK owns copies of endpoint, credentials and will. Its CA pointer
     * remains borrowed across reconnect, so retain only that owned PEM plus
     * the desired subscription list and trusted-time policy. */
    bool tls;
    char *ca_pem;
    size_t ca_size_bytes;
    emqtt_subscription_t subscriptions[EMQTT_SUBSCRIPTIONS_MAX];
    size_t subscription_count;
    emqtt_receive_state_t receiver;
    emqtt_owned_message_t *receiver_message;
    int receiver_slot;
    /* Queue slots bound the number of complete messages; payload storage only
     * exists from the first DATA fragment until poll, stop, or rejection. */
    emqtt_owned_message_t *messages[MESSAGE_SLOTS];
    atomic_bool overflow;
    bool started;
    bool faulted;
    emqtt_state_t state;
    int pending_subscribe, pending_unsubscribe;
    size_t pending_subscribe_count, pending_unsubscribe_index;
    emqtt_subscription_t pending_subscription;
    bool pending_dynamic_subscribe;
    int64_t subscription_deadline_us;
    portMUX_TYPE capacity_lock;
    emqtt_capacity_stats_t capacity;
};
static emqtt_runtime_t *s_instance;
static uint32_t s_runtime_instance;

static void capacity_increment(emqtt_capacity_stats_t *stats, uint32_t *counter)
{
    if (*counter == UINT32_MAX) stats->counters_valid = false;
    else ++*counter;
}

static void capacity_note_partial(emqtt_runtime_t *r)
{
    /* DATA is synchronous and the SDK worker holds its recursive API lock.
     * Take the existing real outbox count before our short resource critical
     * section. No owner enqueue or worker expiry can change it in this callback. */
    const int outbox_bytes = esp_mqtt_client_get_outbox_size(r->client);
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    portENTER_CRITICAL(&r->capacity_lock);
    uint32_t complete = 0, payload = 0;
    for (unsigned i = 0; i < MESSAGE_SLOTS; ++i) {
        if (!r->messages[i]) continue;
        ++complete;
        payload += (uint32_t)r->messages[i]->info.length;
    }
    const uint32_t declared = (uint32_t)r->receiver_message->info.length;
    const uint32_t received = (uint32_t)r->receiver.received;
    const uint32_t bytes = payload + declared + (complete + 1U) * (uint32_t)sizeof(emqtt_owned_message_t);
    portEXIT_CRITICAL(&r->capacity_lock);
    /* Bound the actual locked tuple without calling even the clock inside the
     * resource critical section. The SDK API lock still owns the outbox. */
    const uint64_t observed_until_ms = (uint64_t)esp_timer_get_time() / 1000U;
    portENTER_CRITICAL(&r->capacity_lock);
    if (outbox_bytes < 0) r->capacity.counters_valid = false;
    else if (!r->capacity.rx_peak_valid || bytes > r->capacity.owned_request_bytes ||
             (bytes == r->capacity.owned_request_bytes &&
              (uint32_t)outbox_bytes > r->capacity.outbox_wire_bytes_at_rx_peak)) {
        r->capacity.rx_peak_uptime_ms = now_ms;
        r->capacity.rx_peak_observed_until_uptime_ms = observed_until_ms;
        r->capacity.complete_owner_count = complete;
        r->capacity.complete_payload_bytes = payload;
        r->capacity.partial_declared_bytes = declared;
        r->capacity.partial_received_bytes = received;
        r->capacity.owned_request_bytes = bytes;
        r->capacity.outbox_wire_bytes_at_rx_peak = (uint32_t)outbox_bytes;
        r->capacity.rx_peak_valid = true;
    }
    portEXIT_CRITICAL(&r->capacity_lock);
}

static void *emqtt_storage_calloc(size_t size)
{
#ifdef EMQTT_STORAGE_IRAM_8BIT
    /* The single-core ESP32 keeps bounded MQTT state in byte-accessible IRAM;
     * free() also accepts pointers returned by heap_caps_calloc(). */
    return heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT);
#else
    return calloc(1, size);
#endif
}

static void release_ca(emqtt_runtime_t *r)
{
    if (r->ca_pem) {
        volatile unsigned char *bytes = (volatile unsigned char *)r->ca_pem;
        for (size_t i = 0; i < r->ca_size_bytes; ++i) bytes[i] = 0;
    }
    free(r->ca_pem);
    r->ca_pem = NULL;
    r->ca_size_bytes = 0U;
}

static bool owned(const emqtt_runtime_t *r)
{
    return r && r == s_instance && r->owner == xTaskGetCurrentTaskHandle();
}

static void release_dynamic_message(emqtt_owned_message_t *message)
{
    if (!message) return;
    volatile unsigned char *bytes = (volatile unsigned char *)message;
    const size_t size = sizeof(*message) + message->info.length;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
    free(message);
}

static emqtt_owned_message_t *detach_message_slot(emqtt_runtime_t *r, int slot)
{
    portENTER_CRITICAL(&r->capacity_lock);
    emqtt_owned_message_t *message = r->messages[slot];
    r->messages[slot] = NULL;
    portEXIT_CRITICAL(&r->capacity_lock);
    return message;
}

static void release_message_slot(emqtt_runtime_t *r, int slot)
{
    release_dynamic_message(detach_message_slot(r, slot));
    if (xQueueSend(r->free_slots, &slot, 0) != pdTRUE)
        atomic_store(&r->overflow, true);
}

static void post(emqtt_runtime_t *r, const notice_t *notice)
{
    const bool posted = xQueueSend(r->notices, notice, 0) == pdTRUE;
    /* This is an actual sampled queue size, possibly below a missed transient
     * peak. Never infer occupancy from the number of sent events. */
    const UBaseType_t count = uxQueueMessagesWaiting(r->notices);
    portENTER_CRITICAL(&r->capacity_lock);
    if (count > r->capacity.notice_count_high_water) r->capacity.notice_count_high_water = count;
    if (!posted) capacity_increment(&r->capacity, &r->capacity.notice_full_count);
    portEXIT_CRITICAL(&r->capacity_lock);
    if (!posted) {
        if (notice->slot >= 0) release_message_slot(r, notice->slot);
        atomic_store(&r->overflow, true);
    }
}

static void clear_pending(emqtt_runtime_t *r)
{
    r->pending_subscribe = r->pending_unsubscribe = -1;
    r->pending_subscribe_count = r->pending_unsubscribe_index = 0;
    r->pending_dynamic_subscribe = false;
    memset(&r->pending_subscription, 0, sizeof(r->pending_subscription));
    r->subscription_deadline_us = 0;
}

static void release_receiver_slot(emqtt_runtime_t *r)
{
    portENTER_CRITICAL(&r->capacity_lock);
    const int slot = r->receiver_slot;
    emqtt_owned_message_t *message = r->receiver_message;
    r->receiver_slot = -1;
    r->receiver.active = false; r->receiver.received = 0;
    r->receiver_message = NULL;
    portEXIT_CRITICAL(&r->capacity_lock);
    release_dynamic_message(message);
    if (slot >= 0 && xQueueSend(r->free_slots, &slot, 0) != pdTRUE)
        atomic_store(&r->overflow, true);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    emqtt_runtime_t *r = arg;
    const esp_mqtt_event_t *event = data;
    notice_t n = {.slot = -1, .message_id = event->msg_id,
                  .arrived_us = esp_timer_get_time()};
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        release_receiver_slot(r);
        n.kind = EMQTT_EVENT_CONNECTED;
        break;
    case MQTT_EVENT_DISCONNECTED:
        release_receiver_slot(r);
        n.kind = EMQTT_EVENT_DISCONNECTED;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        n.kind = NOTICE_SUBACK;
        if (event->data_len < 1 || event->data_len > (int)EMQTT_SUBSCRIPTIONS_MAX || !event->data) {
            n.error = EMQTT_ERROR_SUBSCRIPTION;
        } else {
            n.suback_count = (size_t)event->data_len;
            memcpy(n.suback, event->data, n.suback_count);
        }
        break;
    case MQTT_EVENT_UNSUBSCRIBED: n.kind = EMQTT_EVENT_UNSUBSCRIBED; break;
    case MQTT_EVENT_PUBLISHED: n.kind = EMQTT_EVENT_PUBACK; break;
    case MQTT_EVENT_DELETED: n.kind = EMQTT_EVENT_DELETED; n.error = EMQTT_ERROR_EXPIRED; break;
    case MQTT_EVENT_ERROR:
        n.kind = EMQTT_EVENT_ERROR;
        n.error = EMQTT_ERROR_SDK;
        if (event->error_handle) {
            const esp_mqtt_error_codes_t *error = event->error_handle;
            n.broker_code = error->connect_return_code;
            n.tls_flags = error->esp_tls_cert_verify_flags;
            if (error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                n.error = error->esp_tls_cert_verify_flags || error->esp_tls_stack_err ?
                    EMQTT_ERROR_TLS : EMQTT_ERROR_TRANSPORT;
            } else if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                n.error = error->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                          error->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ?
                    EMQTT_ERROR_AUTH : EMQTT_ERROR_BROKER;
            } else if (error->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) n.error = EMQTT_ERROR_SUBSCRIPTION;
        }
        break;
    case MQTT_EVENT_DATA: {
        if (!r->receiver_message) {
            /* Reject a malformed declared size before it can drive allocation.
             * An existing receiver is rejected by the same fragment core. */
            if (event->total_data_len < 0 || event->total_data_len > (int)EMQTT_PAYLOAD_MAX) {
                n.kind = EMQTT_EVENT_ERROR; n.error = EMQTT_ERROR_FRAGMENT;
                break;
            }
            if (xQueueReceive(r->free_slots, &r->receiver_slot, 0) != pdTRUE) {
                r->receiver_slot = -1;
            }
            const size_t length = (size_t)event->total_data_len;
            emqtt_owned_message_t *message = emqtt_storage_calloc(sizeof(*message) + length);
            if (!message) {
                if (r->receiver_slot >= 0)
                    (void)xQueueSend(r->free_slots, &r->receiver_slot, 0);
                r->receiver_slot = -1;
                atomic_store(&r->overflow, true);
                return;
            }
            message->info.length = length;
            portENTER_CRITICAL(&r->capacity_lock);
            r->receiver_message = message;
            portEXIT_CRITICAL(&r->capacity_lock);
        }
        const emqtt_fragment_t fragment = {.topic = event->topic, .topic_length = event->topic_len,
            .data = event->data, .data_length = event->data_len, .total_length = event->total_data_len,
            .offset = event->current_data_offset, .message_id = event->msg_id, .qos = event->qos,
            .retain = event->retain, .duplicate = event->dup};
        const emqtt_rx_result_t result = emqtt_receive_into(&r->receiver,
            &r->receiver_message->info, r->receiver_message->payload,
            r->receiver_message->info.length, &fragment);
        if (result == EMQTT_RX_MORE) { capacity_note_partial(r); return; }
        if (result == EMQTT_RX_REJECTED) {
            release_receiver_slot(r);
            n.kind = EMQTT_EVENT_ERROR; n.error = EMQTT_ERROR_FRAGMENT;
        }
        else {
            if (r->receiver_slot < 0) {
                if (xQueueReceive(r->free_slots, &n.slot, 0) != pdTRUE) {
                    release_receiver_slot(r);
                    atomic_store(&r->overflow, true);
                    return;
                }
            } else {
                n.slot = r->receiver_slot;
            }
            /* Transfer the completed buffer to the bounded owner queue. */
            portENTER_CRITICAL(&r->capacity_lock);
            r->messages[n.slot] = r->receiver_message;
            r->receiver_slot = -1;
            r->receiver_message = NULL;
            portEXIT_CRITICAL(&r->capacity_lock);
            n.kind = EMQTT_EVENT_MESSAGE;
        }
        break;
    }
    default: return;
    }
    post(r, &n);
}

static void drain(emqtt_runtime_t *r)
{
    /* 只在官方 stop 已等待回调退出，或尚未 start 时调用。 */
    release_receiver_slot(r);
    xQueueReset(r->notices);
    for (int i = 0; i < MESSAGE_SLOTS; ++i) {
        release_dynamic_message(detach_message_slot(r, i));
    }
    xQueueReset(r->free_slots);
    for (int i = 0; i < MESSAGE_SLOTS; ++i) (void)xQueueSend(r->free_slots, &i, 0);
    atomic_store(&r->overflow, false);
    clear_pending(r);
    r->faulted = false;
}

static esp_err_t api_result(int id)
{
    return id == -2 ? ESP_ERR_EMQTT_OUTBOX_FULL : id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t emqtt_create(const emqtt_config_t *config, emqtt_runtime_t **out)
{
    bool lab = false;
#if CONFIG_EMQTT_PLAINTEXT_LAB
    lab = true;
#endif
    if (!out || !emqtt_config_valid(config, lab)) return ESP_ERR_INVALID_ARG;
    if (s_instance) return ESP_ERR_INVALID_STATE;
    emqtt_runtime_t *r = emqtt_storage_calloc(sizeof(*r));
    if (!r) return ESP_ERR_NO_MEM;
    portMUX_INITIALIZE(&r->capacity_lock);
    r->owner = xTaskGetCurrentTaskHandle();
    esp_err_t failure = ESP_ERR_NO_MEM;
    r->tls = config->tls;
    r->subscription_count = config->subscription_count;
    memcpy(r->subscriptions, config->subscriptions,
           config->subscription_count * sizeof r->subscriptions[0]);
    if (config->tls) {
        r->ca_size_bytes = strlen(config->ca_pem) + 1U;
        r->ca_pem = emqtt_storage_calloc(r->ca_size_bytes);
        if (!r->ca_pem) goto fail;
        memcpy(r->ca_pem, config->ca_pem, r->ca_size_bytes);
    }
    atomic_init(&r->overflow, false);
    r->receiver_slot = -1;
    r->notices = xQueueCreate(NOTICE_CAPACITY, sizeof(notice_t));
    r->free_slots = xQueueCreate(MESSAGE_SLOTS, sizeof(int));
    if (!r->notices || !r->free_slots) goto fail;
    drain(r);
    const esp_mqtt_client_config_t official = {
        .broker.address = {.hostname = config->hostname, .port = config->port,
            .transport = config->tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP},
        .broker.verification = {.certificate = config->tls ? r->ca_pem : NULL,
            .skip_cert_common_name_check = false},
        .credentials = {.client_id = config->client_id,
            .username = config->username[0] ? config->username : NULL,
            .authentication.password = config->username[0] ? config->password : NULL},
        .session = {.protocol_ver = MQTT_PROTOCOL_V_3_1_1, .disable_clean_session = false,
            .keepalive = 30, .message_retransmit_timeout = 5000,
            .last_will = {.topic = config->will_topic,
                .msg = config->will_length ? (const char *)config->will_payload : "",
                .msg_len = (int)config->will_length, .qos = config->will_qos, .retain = config->will_retain}},
        .network = {.reconnect_timeout_ms = 2000, .timeout_ms = 3000, .disable_auto_reconnect = false},
        .task = {.priority = 5, .stack_size = 6144},
        /* 八个最大长度 filter 的单次 SUBSCRIBE 需要超过 2 KiB。 */
        .buffer = {.size = 1024, .out_size = 2304},
        .outbox.limit = EMQTT_OUTBOX_LIMIT
    };
    r->client = esp_mqtt_client_init(&official);
    if (!r->client) goto fail;
    failure = esp_mqtt_client_register_event(r->client, MQTT_EVENT_ANY, on_event, r);
    if (failure != ESP_OK) goto fail;
    r->capacity.counters_valid = s_runtime_instance != UINT32_MAX;
    if (r->capacity.counters_valid) ++s_runtime_instance;
    r->capacity.runtime_instance = s_runtime_instance;
    r->capacity.owned_message_metadata_bytes = sizeof(emqtt_owned_message_t);
    r->capacity.observation_storage_bytes = sizeof(r->capacity) + sizeof(r->capacity_lock) + sizeof(s_runtime_instance);
    s_instance = r;
    *out = r;
    return ESP_OK;
fail:
    if (r->client) esp_mqtt_client_destroy(r->client);
    if (r->notices) vQueueDelete(r->notices);
    if (r->free_slots) vQueueDelete(r->free_slots);
    release_ca(r);
    volatile unsigned char *bytes = (volatile unsigned char *)r;
    for (size_t i = 0; i < sizeof(*r); ++i) bytes[i] = 0;
    free(r);
    return failure;
}

esp_err_t emqtt_start(emqtt_runtime_t *r, bool network_ready, bool trusted_time_ready)
{
    if (!owned(r) || r->started || !network_ready) return ESP_ERR_INVALID_STATE;
    if (r->tls && !trusted_time_ready) return ESP_ERR_EMQTT_TIME_REQUIRED;
    drain(r);
    const esp_err_t error = esp_mqtt_client_start(r->client);
    if (error != ESP_OK) { r->state = EMQTT_FAILED; return error; }
    r->started = true;
    r->state = EMQTT_CONNECTING;
    return ESP_OK;
}

esp_err_t emqtt_stop(emqtt_runtime_t *r)
{
    if (!owned(r)) return ESP_ERR_INVALID_STATE;
    if (r->started) {
        const esp_err_t error = esp_mqtt_client_stop(r->client);
        if (error != ESP_OK) { r->state = EMQTT_FAILED; return error; }
        r->started = false;
    }
    drain(r);
    r->state = EMQTT_STOPPED;
    return ESP_OK;
}

esp_err_t emqtt_destroy(emqtt_runtime_t *r)
{
    if (!owned(r)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = emqtt_stop(r);
    if (error != ESP_OK) return error;
    error = esp_mqtt_client_destroy(r->client);
    if (error != ESP_OK) return error;
    vQueueDelete(r->notices);
    vQueueDelete(r->free_slots);
    release_ca(r);
    /* volatile 防止释放前凭据清除被优化掉。 */
    volatile unsigned char *bytes = (volatile unsigned char *)r;
    for (size_t i = 0; i < sizeof(*r); ++i) bytes[i] = 0;
    s_instance = NULL;
    free(r);
    return ESP_OK;
}

static esp_err_t submit_subscriptions(emqtt_runtime_t *r, const emqtt_subscription_t *subscriptions,
                                      size_t count, bool dynamic)
{
    if (!count) { r->state = EMQTT_READY; return ESP_OK; }
    esp_mqtt_topic_t topics[EMQTT_SUBSCRIPTIONS_MAX];
    for (size_t i = 0; i < count; ++i) {
        topics[i].filter = subscriptions[i].topic;
        topics[i].qos = subscriptions[i].qos;
    }
    const int id = esp_mqtt_client_subscribe_multiple(r->client, topics, (int)count);
    if (id <= 0) { r->state = EMQTT_FAILED; return id == 0 ? ESP_FAIL : api_result(id); }
    r->pending_subscribe = id;
    r->pending_subscribe_count = count;
    r->pending_dynamic_subscribe = dynamic;
    r->subscription_deadline_us = esp_timer_get_time() + 10000000;
    r->state = EMQTT_SUBSCRIBING;
    return ESP_OK;
}

static void fail_closed(emqtt_runtime_t *r)
{
    (void)emqtt_stop(r);
    r->faulted = true;
    r->state = EMQTT_FAILED;
}

bool emqtt_poll(emqtt_runtime_t *r, emqtt_event_t *out)
{
    if (!owned(r) || !out || r->faulted) return false;
    if (atomic_exchange(&r->overflow, false)) {
        /* 事件丢失后不能继续声称通道可用；由 owner 结束 SDK 回调再释放队列。 */
        fail_closed(r);
        memset(out, 0, sizeof(*out)); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_QUEUE;
        return true;
    }
    notice_t n;
    if (xQueueReceive(r->notices, &n, 0) != pdTRUE) {
        if (r->state != EMQTT_SUBSCRIBING || esp_timer_get_time() < r->subscription_deadline_us)
            return false;
        fail_closed(r);
        memset(out, 0, sizeof(*out)); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        return true;
    }
    /* 已入队的回执以到达时刻判定；逾期到达的任何事件不能延长等待。 */
    if (r->state == EMQTT_SUBSCRIBING && n.arrived_us >= r->subscription_deadline_us) {
        fail_closed(r);
        memset(out, 0, sizeof(*out)); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        return true;
    }
    memset(out, 0, sizeof(*out));
    out->kind = (emqtt_event_kind_t)n.kind;
    out->message_id = n.message_id; out->error = n.error; out->broker_code = n.broker_code; out->tls_flags = n.tls_flags;
    switch (n.kind) {
    case EMQTT_EVENT_CONNECTED:
        clear_pending(r);
        if (submit_subscriptions(r, r->subscriptions, r->subscription_count, false) != ESP_OK) {
            fail_closed(r); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        }
        else if (!r->subscription_count) out->kind = EMQTT_EVENT_READY;
        break;
    case NOTICE_SUBACK:
        if (r->pending_subscribe <= 0 || n.message_id != r->pending_subscribe || r->state != EMQTT_SUBSCRIBING ||
            n.error != EMQTT_ERROR_NONE ||
            !emqtt_suback_valid(n.suback, n.suback_count,
                r->pending_dynamic_subscribe ? &r->pending_subscription : r->subscriptions,
                r->pending_subscribe_count)) {
            fail_closed(r); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        } else {
            if (r->pending_dynamic_subscribe)
                r->subscriptions[r->subscription_count++] = r->pending_subscription;
            r->state = EMQTT_READY; out->kind = EMQTT_EVENT_READY;
        }
        clear_pending(r);
        break;
    case EMQTT_EVENT_UNSUBSCRIBED:
        if (r->pending_unsubscribe <= 0 || n.message_id != r->pending_unsubscribe || r->state != EMQTT_SUBSCRIBING) {
            fail_closed(r); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        } else {
            memmove(r->subscriptions + r->pending_unsubscribe_index,
                    r->subscriptions + r->pending_unsubscribe_index + 1,
                    (r->subscription_count - r->pending_unsubscribe_index - 1) * sizeof(r->subscriptions[0]));
            --r->subscription_count;
            r->state = EMQTT_READY;
        }
        clear_pending(r);
        break;
    case EMQTT_EVENT_DISCONNECTED:
        clear_pending(r);
        r->state = EMQTT_DISCONNECTED;
        break;
    case EMQTT_EVENT_MESSAGE: {
        /* out was fully cleared above; unused public payload bytes stay zero. */
        const emqtt_owned_message_t *message = r->messages[n.slot];
        memcpy(out->message.topic, message->info.topic, sizeof out->message.topic);
        memcpy(out->message.payload, message->payload, message->info.length);
        out->message.length = message->info.length; out->message.message_id = message->info.message_id;
        out->message.qos = message->info.qos; out->message.retain = message->info.retain;
        out->message.duplicate = message->info.duplicate;
        release_message_slot(r, n.slot);
        break;
    }
    case EMQTT_EVENT_ERROR:
        if (n.error == EMQTT_ERROR_SUBSCRIPTION) fail_closed(r);
        else if (n.error != EMQTT_ERROR_FRAGMENT) r->state = EMQTT_FAILED;
        break;
    default: break;
    }
    return true;
}

emqtt_state_t emqtt_state(const emqtt_runtime_t *r)
{
    return owned(r) ? r->state : EMQTT_FAILED;
}

esp_err_t emqtt_enqueue(emqtt_runtime_t *r, const char *topic,
                                const void *payload, size_t length, uint8_t qos, bool retain, int *message_id)
{
    if (!owned(r) || !r->started || r->state == EMQTT_FAILED) return ESP_ERR_INVALID_STATE;
    if (!message_id || !topic || !emqtt_topic_valid(topic, strnlen(topic, EMQTT_TOPIC_MAX + 1), false) ||
        length > EMQTT_PUBLISH_PAYLOAD_MAX_BYTES || (!payload && length) || qos > 1) return ESP_ERR_INVALID_ARG;
    /* 零长度传 NULL：官方 API 对非 NULL/len=0 会隐式 strlen。 */
    const int id = esp_mqtt_client_enqueue(r->client, topic, length ? payload : NULL, (int)length, qos, retain, true);
    const esp_err_t error = api_result(id);
    if (error == ESP_ERR_EMQTT_OUTBOX_FULL) {
        const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
        portENTER_CRITICAL(&r->capacity_lock);
        capacity_increment(&r->capacity, &r->capacity.outbox_full_count);
        r->capacity.last_outbox_full_payload_bytes = (uint32_t)length;
        r->capacity.last_outbox_full_uptime_ms = now_ms;
        portEXIT_CRITICAL(&r->capacity_lock);
    }
    if (error == ESP_OK) *message_id = id;
    return error;
}

esp_err_t emqtt_subscribe(emqtt_runtime_t *r, const char *filter, uint8_t qos)
{
    if (!owned(r) || r->state != EMQTT_READY) return ESP_ERR_INVALID_STATE;
    if (!filter || qos > 1 || !emqtt_topic_valid(filter, strnlen(filter, EMQTT_TOPIC_MAX + 1), true) ||
        r->subscription_count == EMQTT_SUBSCRIPTIONS_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < r->subscription_count; ++i)
        if (!strcmp(filter, r->subscriptions[i].topic)) return ESP_ERR_INVALID_ARG;
    emqtt_subscription_t *sub = &r->pending_subscription;
    strcpy(sub->topic, filter); sub->qos = qos;
    /* 只订阅新增项，避免重新订阅旧项触发无关 retained 消息再次交付。 */
    const esp_err_t error = submit_subscriptions(r, sub, 1, true);
    if (error != ESP_OK) fail_closed(r);
    return error;
}

esp_err_t emqtt_unsubscribe(emqtt_runtime_t *r, const char *filter)
{
    if (!owned(r) || r->state != EMQTT_READY) return ESP_ERR_INVALID_STATE;
    if (!filter || !emqtt_topic_valid(filter, strnlen(filter, EMQTT_TOPIC_MAX + 1), true)) return ESP_ERR_INVALID_ARG;
    size_t index = 0;
    while (index < r->subscription_count && strcmp(filter, r->subscriptions[index].topic)) ++index;
    if (index == r->subscription_count) return ESP_ERR_INVALID_ARG;
    const int id = esp_mqtt_client_unsubscribe(r->client, filter);
    if (id <= 0) { fail_closed(r); return id == 0 ? ESP_FAIL : api_result(id); }
    r->pending_unsubscribe = id;
    r->pending_unsubscribe_index = index;
    r->subscription_deadline_us = esp_timer_get_time() + 10000000;
    r->state = EMQTT_SUBSCRIBING;
    return ESP_OK;
}

int emqtt_outbox_size(const emqtt_runtime_t *r)
{
    return owned(r) ? esp_mqtt_client_get_outbox_size(r->client) : -1;
}

bool emqtt_get_capacity_snapshot(const emqtt_runtime_t *r, emqtt_capacity_stats_t *out)
{
    if (!owned(r) || !out) return false;
    portMUX_TYPE *lock = (portMUX_TYPE *)&r->capacity_lock;
    portENTER_CRITICAL(lock);
    *out = r->capacity;
    portEXIT_CRITICAL(lock);
    return true;
}
