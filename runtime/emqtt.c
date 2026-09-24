// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

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
    emqtt_config_t config;
    emqtt_receiver_t receiver;
    int receiver_slot;
    emqtt_message_t messages[MESSAGE_SLOTS];
    atomic_bool overflow;
    bool started;
    bool faulted;
    emqtt_state_t state;
    int pending_subscribe, pending_unsubscribe;
    size_t pending_subscribe_count, pending_unsubscribe_index;
    emqtt_subscription_t pending_subscription;
    bool pending_dynamic_subscribe;
    int64_t subscription_deadline_us;
};
static emqtt_runtime_t *s_instance;

static bool owned(const emqtt_runtime_t *r)
{
    return r && r == s_instance && r->owner == xTaskGetCurrentTaskHandle();
}

static void post(emqtt_runtime_t *r, const notice_t *notice)
{
    if (xQueueSend(r->notices, notice, 0) != pdTRUE) {
        if (notice->slot >= 0) { const int slot = notice->slot; (void)xQueueSend(r->free_slots, &slot, 0); }
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
    const int slot = r->receiver_slot;
    r->receiver_slot = -1;
    emqtt_receive_reset(&r->receiver);
    r->receiver.message = NULL;
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
        if (r->receiver_slot < 0) {
            if (xQueueReceive(r->free_slots, &r->receiver_slot, 0) != pdTRUE) {
                atomic_store(&r->overflow, true);
                return;
            }
            r->receiver.message = &r->messages[r->receiver_slot];
        }
        const emqtt_fragment_t fragment = {.topic = event->topic, .topic_length = event->topic_len,
            .data = event->data, .data_length = event->data_len, .total_length = event->total_data_len,
            .offset = event->current_data_offset, .message_id = event->msg_id, .qos = event->qos,
            .retain = event->retain, .duplicate = event->dup};
        const emqtt_rx_result_t result = emqtt_receive(&r->receiver, &fragment);
        if (result == EMQTT_RX_MORE) return;
        if (result == EMQTT_RX_REJECTED) {
            release_receiver_slot(r);
            n.kind = EMQTT_EVENT_ERROR; n.error = EMQTT_ERROR_FRAGMENT;
        }
        else {
            n.slot = r->receiver_slot;
            r->receiver_slot = -1;
            r->receiver.message = NULL;
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
    r->receiver_slot = -1;
    emqtt_receive_reset(&r->receiver);
    r->receiver.message = NULL;
    xQueueReset(r->notices);
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
    emqtt_runtime_t *r = calloc(1, sizeof(*r));
    if (!r) return ESP_ERR_NO_MEM;
    r->owner = xTaskGetCurrentTaskHandle();
    r->config = *config;
    esp_err_t failure = ESP_ERR_NO_MEM;
    atomic_init(&r->overflow, false);
    r->notices = xQueueCreate(NOTICE_CAPACITY, sizeof(notice_t));
    r->free_slots = xQueueCreate(MESSAGE_SLOTS, sizeof(int));
    if (!r->notices || !r->free_slots) goto fail;
    drain(r);
    const esp_mqtt_client_config_t official = {
        .broker.address = {.hostname = r->config.hostname, .port = config->port,
            .transport = config->tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP},
        .broker.verification = {.certificate = config->tls ? r->config.ca_pem : NULL,
            .skip_cert_common_name_check = false},
        .credentials = {.client_id = r->config.client_id,
            .username = config->username[0] ? r->config.username : NULL,
            .authentication.password = config->username[0] ? r->config.password : NULL},
        .session = {.protocol_ver = MQTT_PROTOCOL_V_3_1_1, .disable_clean_session = false,
            .keepalive = 30, .message_retransmit_timeout = 5000,
            .last_will = {.topic = r->config.will_topic,
                .msg = config->will_length ? (const char *)r->config.will_payload : "",
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
    s_instance = r;
    *out = r;
    return ESP_OK;
fail:
    if (r->client) esp_mqtt_client_destroy(r->client);
    if (r->notices) vQueueDelete(r->notices);
    if (r->free_slots) vQueueDelete(r->free_slots);
    volatile unsigned char *bytes = (volatile unsigned char *)r;
    for (size_t i = 0; i < sizeof(*r); ++i) bytes[i] = 0;
    free(r);
    return failure;
}

esp_err_t emqtt_start(emqtt_runtime_t *r, bool network_ready, bool trusted_time_ready)
{
    if (!owned(r) || r->started || !network_ready) return ESP_ERR_INVALID_STATE;
    if (r->config.tls && !trusted_time_ready) return ESP_ERR_EMQTT_TIME_REQUIRED;
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
        if (submit_subscriptions(r, r->config.subscriptions, r->config.subscription_count, false) != ESP_OK) {
            fail_closed(r); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        }
        else if (!r->config.subscription_count) out->kind = EMQTT_EVENT_READY;
        break;
    case NOTICE_SUBACK:
        if (r->pending_subscribe <= 0 || n.message_id != r->pending_subscribe || r->state != EMQTT_SUBSCRIBING ||
            n.error != EMQTT_ERROR_NONE ||
            !emqtt_suback_valid(n.suback, n.suback_count,
                r->pending_dynamic_subscribe ? &r->pending_subscription : r->config.subscriptions,
                r->pending_subscribe_count)) {
            fail_closed(r); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        } else {
            if (r->pending_dynamic_subscribe)
                r->config.subscriptions[r->config.subscription_count++] = r->pending_subscription;
            r->state = EMQTT_READY; out->kind = EMQTT_EVENT_READY;
        }
        clear_pending(r);
        break;
    case EMQTT_EVENT_UNSUBSCRIBED:
        if (r->pending_unsubscribe <= 0 || n.message_id != r->pending_unsubscribe || r->state != EMQTT_SUBSCRIBING) {
            fail_closed(r); out->kind = EMQTT_EVENT_ERROR; out->error = EMQTT_ERROR_SUBSCRIPTION;
        } else {
            memmove(r->config.subscriptions + r->pending_unsubscribe_index,
                    r->config.subscriptions + r->pending_unsubscribe_index + 1,
                    (r->config.subscription_count - r->pending_unsubscribe_index - 1) * sizeof(r->config.subscriptions[0]));
            --r->config.subscription_count;
            r->state = EMQTT_READY;
        }
        clear_pending(r);
        break;
    case EMQTT_EVENT_DISCONNECTED:
        clear_pending(r);
        r->state = EMQTT_DISCONNECTED;
        break;
    case EMQTT_EVENT_MESSAGE:
        out->message = r->messages[n.slot];
        (void)xQueueSend(r->free_slots, &n.slot, 0);
        break;
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
        length > EMQTT_PAYLOAD_MAX || (!payload && length) || qos > 1) return ESP_ERR_INVALID_ARG;
    /* 零长度传 NULL：官方 API 对非 NULL/len=0 会隐式 strlen。 */
    const int id = esp_mqtt_client_enqueue(r->client, topic, length ? payload : NULL, (int)length, qos, retain, true);
    const esp_err_t error = api_result(id);
    if (error == ESP_OK) *message_id = id;
    return error;
}

esp_err_t emqtt_subscribe(emqtt_runtime_t *r, const char *filter, uint8_t qos)
{
    if (!owned(r) || r->state != EMQTT_READY) return ESP_ERR_INVALID_STATE;
    if (!filter || qos > 1 || !emqtt_topic_valid(filter, strnlen(filter, EMQTT_TOPIC_MAX + 1), true) ||
        r->config.subscription_count == EMQTT_SUBSCRIPTIONS_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < r->config.subscription_count; ++i)
        if (!strcmp(filter, r->config.subscriptions[i].topic)) return ESP_ERR_INVALID_ARG;
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
    while (index < r->config.subscription_count && strcmp(filter, r->config.subscriptions[index].topic)) ++index;
    if (index == r->config.subscription_count) return ESP_ERR_INVALID_ARG;
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
