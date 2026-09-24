// SPDX-License-Identifier: Apache-2.0
/* 测试本仓装配与回调生命周期；MQTT 协议行为仍须官方组件 + 真 Broker 实测。 */
#include "emqtt.h"
#include "mqtt_client.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_queue { unsigned length, item_size, head, count; unsigned char data[]; };
static unsigned live_queues, queue_attempt, fail_queue_attempt;
static void *dynamic_allocation;
static unsigned dynamic_attempts, dynamic_allocations, dynamic_frees;
static bool fail_next_dynamic;
void *emqtt_test_calloc(size_t count, size_t size)
{
    if (count == 1 && size == sizeof(emqtt_message_t)) {
        ++dynamic_attempts;
        if (fail_next_dynamic) { fail_next_dynamic = false; return NULL; }
        assert(!dynamic_allocation);
        dynamic_allocation = calloc(count, size);
        if (dynamic_allocation) ++dynamic_allocations;
        return dynamic_allocation;
    }
    return calloc(count, size);
}
void emqtt_test_free(void *pointer)
{
    if (pointer == dynamic_allocation && pointer) {
        const unsigned char *bytes = pointer;
        for (size_t i = 0; i < sizeof(emqtt_message_t); ++i) assert(bytes[i] == 0);
        dynamic_allocation = NULL;
        ++dynamic_frees;
    }
    free(pointer);
}
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    if (++queue_attempt == fail_queue_attempt) return NULL;
    QueueHandle_t q = calloc(1, sizeof(*q) + (size_t)length * item_size);
    assert(q); q->length = length; q->item_size = item_size; ++live_queues; return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    assert(ticks == 0);
    if (q->count == q->length) return pdFALSE;
    memcpy(q->data + ((q->head + q->count) % q->length) * q->item_size, item, q->item_size);
    ++q->count; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    assert(ticks == 0);
    if (!q->count) return pdFALSE;
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length; --q->count; return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t q) { q->head = q->count = 0; return pdTRUE; }
void vQueueDelete(QueueHandle_t q) { assert(live_queues); --live_queues; free(q); }
static uintptr_t current_task = 1;
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)current_task; }
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }

struct esp_mqtt_client { bool started; };
static struct esp_mqtt_client sdk;
static esp_mqtt_client_config_t configured;
static esp_event_handler_t callback;
static void *callback_arg;
static bool callback_active, init_fail, live_client;
static int register_error, start_error, stop_error, enqueue_result = 41, subscribe_result = 31, unsubscribe_result = 32;
static unsigned stop_calls, subscriptions, enqueues;
static int submitted_topic_count;
static emqtt_subscription_t submitted_topics[8];
static const char *sent_data;
static int sent_length, sent_qos;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    assert(!live_client); if (init_fail) return NULL;
    live_client = true; configured = *config; return &sdk;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t id,
                                        esp_event_handler_t handler, void *arg)
{
    assert(client == &sdk && id == MQTT_EVENT_ANY); callback = handler; callback_arg = arg; return register_error;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    assert(client == &sdk && !callback_active && !sdk.started);
    if (!start_error) sdk.started = true;
    return start_error;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    assert(client == &sdk && !callback_active && sdk.started); ++stop_calls;
    if (!stop_error) sdk.started = false;
    return stop_error;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    assert(client == &sdk && !sdk.started && !callback_active && live_client); live_client = false; return ESP_OK;
}
int esp_mqtt_client_subscribe_multiple(esp_mqtt_client_handle_t client, const esp_mqtt_topic_t *topics, int count)
{
    assert(client == &sdk && !callback_active && topics && count > 0 && count <= 8);
    submitted_topic_count = count;
    for (int i = 0; i < count; ++i) {
        strcpy(submitted_topics[i].topic, topics[i].filter);
        submitted_topics[i].qos = (uint8_t)topics[i].qos;
    }
    ++subscriptions; return subscribe_result;
}
int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t client, const char *topic)
{
    assert(client == &sdk && !callback_active && topic); return unsubscribe_result;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic, const char *data,
                            int length, int qos, int retain, bool store)
{
    assert(client == &sdk && !callback_active && topic && store); (void)retain;
    ++enqueues; sent_data = data; sent_length = length; sent_qos = qos; return enqueue_result;
}
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client) { assert(client == &sdk); return 123; }

static void emit(esp_mqtt_event_id_t kind, esp_mqtt_event_t event)
{
    assert(live_client && sdk.started);
    callback_active = true; current_task = 2;
    callback(callback_arg, "mqtt", kind, &event);
    current_task = 1; callback_active = false;
}
static emqtt_config_t config(void)
{
    return (emqtt_config_t){.hostname = "broker.example.invalid", .port = 8883, .tls = true,
        .client_id = "mqtt-standalone-test", .username = "test", .password = "test-only",
        .ca_pem = "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
        .will_topic = "unit/status", .will_length = 0, .will_qos = 1, .will_retain = true,
        .subscriptions = {{.topic = "unit/in", .qos = 1}}, .subscription_count = 1};
}
static emqtt_event_t output;
static void connect_ready(emqtt_runtime_t *r)
{
    emit(MQTT_EVENT_CONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_CONNECTED);
    assert(emqtt_state(r) == EMQTT_SUBSCRIBING);
    char grant = 1;
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = subscribe_result, .data = &grant, .data_len = 1});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_READY);
    assert(emqtt_state(r) == EMQTT_READY);
}

int main(void)
{
    emqtt_config_t c = config();
    emqtt_runtime_t *r = NULL;
    for (unsigned fail = 1; fail <= 2; ++fail) {
        queue_attempt = 0; fail_queue_attempt = fail;
        assert(emqtt_create(&c, &r) == ESP_ERR_NO_MEM && !r && !live_queues && !live_client);
    }
    fail_queue_attempt = 0; init_fail = true;
    assert(emqtt_create(&c, &r) == ESP_ERR_NO_MEM && !live_queues); init_fail = false;
    register_error = ESP_FAIL;
    assert(emqtt_create(&c, &r) == ESP_FAIL && !live_queues && !live_client); register_error = 0;
    assert(emqtt_create(&c, &r) == ESP_OK);
    assert(emqtt_create(&c, &r) == ESP_ERR_INVALID_STATE);
    assert(configured.session.protocol_ver == MQTT_PROTOCOL_V_3_1_1 && !configured.session.disable_clean_session);
    assert(configured.broker.address.transport == MQTT_TRANSPORT_OVER_SSL && !configured.broker.verification.skip_cert_common_name_check);
    assert(configured.outbox.limit == 16384 && configured.session.keepalive == 30);
    assert(configured.buffer.out_size >= 5 + 8 * (2 + 256 + 1));
    assert(configured.network.reconnect_timeout_ms && !configured.network.disable_auto_reconnect);
    assert(!strcmp(configured.credentials.client_id, c.client_id));
    c.ca_pem[0] = 'X'; c.password[0] = 'X';
    assert(configured.broker.verification.certificate[0] == '-' && configured.credentials.authentication.password[0] == 't');
    assert(emqtt_start(r, false, true) == ESP_ERR_INVALID_STATE);
    assert(emqtt_start(r, true, false) == ESP_ERR_EMQTT_TIME_REQUIRED);
    start_error = ESP_FAIL; assert(emqtt_start(r, true, true) == ESP_FAIL); start_error = 0;
    assert(emqtt_start(r, true, true) == ESP_OK);
    current_task = 2; assert(emqtt_stop(r) == ESP_ERR_INVALID_STATE); current_task = 1;
    connect_ready(r);
    /* 回执按实际到达时刻判定；owner 晚些 poll 不应误报超时。 */
    assert(emqtt_subscribe(r, "unit/timely", 0) == ESP_OK);
    char timely_grant = 0;
    emit(MQTT_EVENT_PUBLISHED, (esp_mqtt_event_t){.msg_id = 77});
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = &timely_grant, .data_len = 1});
    now_us += 10000001;
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_PUBACK);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_READY);
    assert(emqtt_unsubscribe(r, "unit/timely") == ESP_OK);
    emit(MQTT_EVENT_UNSUBSCRIBED, (esp_mqtt_event_t){.msg_id = 32});
    now_us += 10000001;
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_UNSUBSCRIBED);
    /* 超时后才到达的回执即使在 poll 前入队，也不能被接受。 */
    assert(emqtt_subscribe(r, "unit/late", 0) == ESP_OK);
    now_us += 10000001;
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = &timely_grant, .data_len = 1});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    /* 未获确认的动态新增不能成为下一轮的期望订阅。 */
    assert(emqtt_start(r, true, true) == ESP_OK);
    connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    assert(emqtt_destroy(r) == ESP_OK);
    c = config();
    assert(emqtt_create(&c, &r) == ESP_OK);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    int id = -100;
    enqueue_result = -2;
    assert(emqtt_enqueue(r, "unit/out", "x", 1, 1, false, &id) == ESP_ERR_EMQTT_OUTBOX_FULL && id == -100);
    enqueue_result = -1; assert(emqtt_enqueue(r, "unit/out", "x", 1, 1, false, &id) == ESP_FAIL);
    enqueue_result = 0;
    assert(emqtt_enqueue(r, "unit/out", "not-a-payload", 0, 0, false, &id) == ESP_OK && id == 0);
    assert(sent_data == NULL && sent_length == 0 && sent_qos == 0);
    assert(emqtt_enqueue(r, "unit/+", "x", 1, 1, false, &id) == ESP_ERR_INVALID_ARG);
    assert(emqtt_enqueue(r, "unit/out", "x", 4097, 1, false, &id) == ESP_ERR_INVALID_ARG);
    assert(emqtt_outbox_size(r) == 123);
    char bytes[] = "abcdef";
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 10, .topic = "unit/in", .topic_len = 7,
        .data = bytes, .data_len = 3, .total_data_len = 6, .qos = 1});
    assert(!emqtt_poll(r, &output));
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 10, .data = bytes + 3,
        .data_len = 3, .total_data_len = 6, .current_data_offset = 3, .qos = 1});
    memset(bytes, 'X', sizeof(bytes));
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    assert(output.message.length == 6 && !memcmp(output.message.payload, "abcdef", 6));
    /* 首片预留的槽在断线时归还；同一运行实例可再次接收三个待处理消息。 */
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 11, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_DISCONNECTED);
    connect_ready(r);
    char queued[] = "abc";
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = i + 12, .topic = "unit/in", .topic_len = 7,
            .data = &queued[i], .data_len = 1, .total_data_len = 1, .qos = 1});
    /* 三槽排队后第 4 条开始分片；owner 先消费一槽，余下片仍须交付。 */
    const unsigned allocations_before = dynamic_attempts;
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 15, .topic = "unit/in", .topic_len = 7,
        .data = "d", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(dynamic_attempts == allocations_before + 1 && dynamic_allocation);
    memset(queued, 'X', sizeof queued);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE &&
           output.message.length == 1 && output.message.payload[0] == 'a');
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 15, .data = "e", .data_len = 1,
        .total_data_len = 2, .current_data_offset = 1, .qos = 1});
    for (int i = 0; i < 2; ++i)
        assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE &&
               output.message.length == 1 && output.message.payload[0] == 'b' + i);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE &&
           output.message.length == 2 && !memcmp(output.message.payload, "de", 2));
    assert(!dynamic_allocation);
    /* 畸形后续片归还正在重组的槽，后续完整消息仍可到达。 */
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 15, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 15, .data = "b", .data_len = 1,
        .total_data_len = 2, .current_data_offset = 2, .qos = 1});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_ERROR &&
           output.error == EMQTT_ERROR_FRAGMENT);
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 16, .topic = "unit/in", .topic_len = 7,
        .data = "z", .data_len = 1, .total_data_len = 1, .qos = 1});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE &&
           output.message.payload[0] == 'z');
    /* 主动停止也必须回收未完成分片占用的槽。 */
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 17, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(emqtt_stop(r) == ESP_OK);
    assert(emqtt_start(r, true, true) == ESP_OK);
    connect_ready(r);
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = i + 18, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    for (int i = 0; i < 3; ++i)
        assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE &&
               output.message.payload[0] == 'q');
    emit(MQTT_EVENT_DELETED, (esp_mqtt_event_t){.msg_id = 41});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_DELETED && output.error == EMQTT_ERROR_EXPIRED);
    assert(emqtt_state(r) == EMQTT_READY);
    emit(MQTT_EVENT_PUBLISHED, (esp_mqtt_event_t){.msg_id = 41});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_PUBACK);
    assert(emqtt_subscribe(r, "unit/extra", 0) == ESP_OK);
    assert(emqtt_subscribe(r, "unit/extra", 0) == ESP_ERR_INVALID_STATE);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/extra") && submitted_topics[0].qos == 0);
    char grant_extra = 0;
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = &grant_extra, .data_len = 1});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_READY);
    assert(emqtt_subscribe(r, "unit/extra", 0) == ESP_ERR_INVALID_ARG);
    /* 单项动态 SUBACK 与重新连接时的完整 SUBACK 是不同的证明。 */
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0}); assert(emqtt_poll(r, &output));
    emit(MQTT_EVENT_CONNECTED, (esp_mqtt_event_t){0}); assert(emqtt_poll(r, &output));
    assert(submitted_topic_count == 2 && !strcmp(submitted_topics[0].topic, "unit/in") && !strcmp(submitted_topics[1].topic, "unit/extra"));
    char grants[] = {1, 0};
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = grants, .data_len = 2});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_READY);
    assert(emqtt_unsubscribe(r, "unit/extra") == ESP_OK);
    assert(emqtt_unsubscribe(r, "unit/extra") == ESP_ERR_INVALID_STATE);
    emit(MQTT_EVENT_UNSUBSCRIBED, (esp_mqtt_event_t){.msg_id = 32});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_UNSUBSCRIBED);
    assert(emqtt_unsubscribe(r, "unit/extra") == ESP_ERR_INVALID_ARG);
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &output) && emqtt_state(r) == EMQTT_DISCONNECTED);
    unsigned before = subscriptions; connect_ready(r); assert(subscriptions == before + 1);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    esp_mqtt_error_codes_t fault = {.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        .connect_return_code = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED};
    emit(MQTT_EVENT_ERROR, (esp_mqtt_event_t){.error_handle = &fault});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_AUTH);
    assert(emqtt_state(r) == EMQTT_FAILED && live_client);
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0}); assert(emqtt_poll(r, &output)); connect_ready(r);
    fault = (esp_mqtt_error_codes_t){.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT, .esp_tls_cert_verify_flags = 8};
    emit(MQTT_EVENT_ERROR, (esp_mqtt_event_t){.error_handle = &fault});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_TLS && output.tls_flags == 8);
    assert(configured.broker.address.transport == MQTT_TRANSPORT_OVER_SSL && !configured.broker.verification.skip_cert_common_name_check);
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0}); assert(emqtt_poll(r, &output)); connect_ready(r);
    assert(emqtt_stop(r) == ESP_OK);
    assert(emqtt_start(r, true, true) == ESP_OK);
    emit(MQTT_EVENT_CONNECTED, (esp_mqtt_event_t){0}); assert(emqtt_poll(r, &output));
    char denied = (char)0x80;
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = &denied, .data_len = 1});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION);
    assert(!sdk.started && emqtt_state(r) == EMQTT_FAILED);
    assert(!emqtt_poll(r, &output));
    assert(emqtt_start(r, true, true) == ESP_OK);
    emit(MQTT_EVENT_CONNECTED, (esp_mqtt_event_t){0}); assert(emqtt_poll(r, &output));
    now_us += 10000001;
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    /* 动态缓冲上的畸形续片必须释放；既有三条消息仍按顺序交付。 */
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 50 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 53, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(dynamic_allocation);
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 53, .data = "b", .data_len = 1,
        .total_data_len = 2, .current_data_offset = 2, .qos = 1});
    assert(!dynamic_allocation);
    for (int i = 0; i < 3; ++i)
        assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_ERROR && output.error == EMQTT_ERROR_FRAGMENT);
    /* 断线、停止和已入通知队列后停止都不能遗留临时缓冲。 */
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 54 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 57, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(dynamic_allocation);
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0});
    assert(!dynamic_allocation);
    for (int i = 0; i < 3; ++i)
        assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_DISCONNECTED);
    connect_ready(r);
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 58 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 61, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(dynamic_allocation && emqtt_stop(r) == ESP_OK && !dynamic_allocation);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 62 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 65, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(dynamic_allocation);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 65, .data = "b", .data_len = 1,
        .total_data_len = 2, .current_data_offset = 1, .qos = 1});
    assert(!dynamic_allocation && emqtt_stop(r) == ESP_OK);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    /* 第 4 条完成前 owner 已释放槽，但通知队列已满：归还槽并 fail closed。 */
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 66 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 69, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    for (int i = 0; i < 14; ++i)
        emit(MQTT_EVENT_PUBLISHED, (esp_mqtt_event_t){.msg_id = 70 + i});
    assert(dynamic_allocation);
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 69, .data = "b", .data_len = 1,
        .total_data_len = 2, .current_data_offset = 1, .qos = 1});
    assert(!dynamic_allocation);
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_QUEUE && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    /* 临时申请 OOM 时本地 fail closed；官方核心可能已先发 PUBACK。 */
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 83 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    fail_next_dynamic = true;
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 86, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(!fail_next_dynamic && !dynamic_allocation);
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_QUEUE && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    /* 第 4 条完成时无槽可转存，保持原有 fail-closed 上限。 */
    for (int i = 0; i < 4; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.topic = "unit/in", .topic_len = 7});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_QUEUE && !sdk.started);
    assert(!dynamic_allocation);
    assert(!emqtt_poll(r, &output));
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    /* 第四条在订阅期限后完成时，仍使用原有到达时刻判定。 */
    assert(emqtt_subscribe(r, "unit/late-dynamic", 0) == ESP_OK);
    for (int i = 0; i < 3; ++i)
        emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 87 + i, .topic = "unit/in", .topic_len = 7,
            .data = "q", .data_len = 1, .total_data_len = 1, .qos = 1});
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 90, .topic = "unit/in", .topic_len = 7,
        .data = "a", .data_len = 1, .total_data_len = 2, .qos = 1});
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    now_us += 10000001;
    emit(MQTT_EVENT_DATA, (esp_mqtt_event_t){.msg_id = 90, .data = "b", .data_len = 1,
        .total_data_len = 2, .current_data_offset = 1, .qos = 1});
    assert(!dynamic_allocation);
    for (int i = 0; i < 2; ++i)
        assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_MESSAGE);
    assert(emqtt_poll(r, &output) && output.kind == EMQTT_EVENT_ERROR &&
           output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started && !dynamic_allocation);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    for (int i = 0; i < 17; ++i) emit(MQTT_EVENT_PUBLISHED, (esp_mqtt_event_t){.msg_id = i + 1});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_QUEUE && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK);
    stop_error = ESP_FAIL;
    assert(emqtt_destroy(r) == ESP_FAIL && live_client && live_queues == 2);
    stop_error = 0; assert(emqtt_destroy(r) == ESP_OK && !live_client && !live_queues);
    c = config();
    assert(emqtt_create(&c, &r) == ESP_OK);
    assert(emqtt_start(r, true, true) == ESP_OK);
    subscribe_result = -2;
    emit(MQTT_EVENT_CONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    subscribe_result = 31;
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    subscribe_result = -1;
    assert(emqtt_subscribe(r, "unit/failed", 1) == ESP_FAIL && !sdk.started);
    subscribe_result = 31;
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    unsubscribe_result = -1;
    assert(emqtt_unsubscribe(r, "unit/in") == ESP_FAIL && !sdk.started);
    unsubscribe_result = 32;
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    assert(submitted_topic_count == 1);
    /* 动态单项请求不接受错误条数、未匹配 UNSUBACK 或无限等待。 */
    assert(emqtt_subscribe(r, "unit/extra", 0) == ESP_OK);
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = grants, .data_len = 2});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    assert(emqtt_destroy(r) == ESP_OK);
    assert(emqtt_create(&c, &r) == ESP_OK);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    emit(MQTT_EVENT_UNSUBSCRIBED, (esp_mqtt_event_t){.msg_id = -1});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    /* 断线前未确认的新增不得进入重连时的期望列表。 */
    assert(emqtt_subscribe(r, "unit/pending", 0) == ESP_OK);
    assert(emqtt_subscribe(r, "unit/pending", 0) == ESP_ERR_INVALID_STATE);
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &output) && emqtt_state(r) == EMQTT_DISCONNECTED);
    connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    /* Broker 明确拒绝的新增也不能在显式重启后偷偷成为 READY 的订阅。 */
    assert(emqtt_subscribe(r, "unit/denied", 0) == ESP_OK);
    char dynamic_denied = (char)0x80;
    emit(MQTT_EVENT_SUBSCRIBED, (esp_mqtt_event_t){.msg_id = 31, .data = &dynamic_denied, .data_len = 1});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    /* 断线前未确认的退订不能删除原有的已确认订阅。 */
    assert(emqtt_unsubscribe(r, "unit/in") == ESP_OK);
    assert(emqtt_unsubscribe(r, "unit/in") == ESP_ERR_INVALID_STATE);
    emit(MQTT_EVENT_DISCONNECTED, (esp_mqtt_event_t){0});
    assert(emqtt_poll(r, &output) && emqtt_state(r) == EMQTT_DISCONNECTED);
    connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    /* 错误 UNSUBACK ID 后停止会话，重启仍恢复原订阅。 */
    assert(emqtt_unsubscribe(r, "unit/in") == ESP_OK);
    emit(MQTT_EVENT_UNSUBSCRIBED, (esp_mqtt_event_t){.msg_id = 33});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    assert(emqtt_unsubscribe(r, "unit/in") == ESP_OK);
    now_us += 10000001;
    emit(MQTT_EVENT_UNSUBSCRIBED, (esp_mqtt_event_t){.msg_id = 32});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    /* 未获确认的动态退订不能让下一轮 READY 丢失原订阅。 */
    assert(emqtt_start(r, true, true) == ESP_OK);
    connect_ready(r);
    assert(submitted_topic_count == 1 && !strcmp(submitted_topics[0].topic, "unit/in"));
    fault = (esp_mqtt_error_codes_t){.error_type = MQTT_ERROR_TYPE_SUBSCRIBE_FAILED};
    emit(MQTT_EVENT_ERROR, (esp_mqtt_event_t){.error_handle = &fault});
    assert(emqtt_poll(r, &output) && output.error == EMQTT_ERROR_SUBSCRIPTION && !sdk.started);
    assert(emqtt_destroy(r) == ESP_OK);
    for (int i = 0; i < 100; ++i) {
        assert(emqtt_create(&c, &r) == ESP_OK);
        assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
        assert(emqtt_stop(r) == ESP_OK);
        assert(emqtt_start(r, true, true) == ESP_OK); connect_ready(r);
        assert(emqtt_destroy(r) == ESP_OK && !live_client && !live_queues);
    }
    assert(stop_calls > 200 && enqueues == 3);
    assert(!dynamic_allocation && dynamic_allocations == dynamic_frees);
    puts("  mqtt_runtime   passed (SDK event injection; not Broker/hardware acceptance)");
    return 0;
}
