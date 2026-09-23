// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "emqtt_contract.h"
#include "esp_err.h"

typedef struct emqtt_runtime emqtt_runtime_t;
typedef enum { EMQTT_STOPPED, EMQTT_CONNECTING, EMQTT_SUBSCRIBING,
               EMQTT_READY, EMQTT_DISCONNECTED, EMQTT_FAILED } emqtt_state_t;
typedef enum { EMQTT_ERROR_NONE, EMQTT_ERROR_TRANSPORT, EMQTT_ERROR_TLS,
               EMQTT_ERROR_AUTH, EMQTT_ERROR_BROKER, EMQTT_ERROR_SUBSCRIPTION,
               EMQTT_ERROR_FRAGMENT, EMQTT_ERROR_QUEUE, EMQTT_ERROR_EXPIRED,
               EMQTT_ERROR_SDK } emqtt_error_t;
typedef enum { EMQTT_EVENT_CONNECTED, EMQTT_EVENT_READY, EMQTT_EVENT_DISCONNECTED,
               EMQTT_EVENT_MESSAGE, EMQTT_EVENT_PUBACK, EMQTT_EVENT_DELETED,
               EMQTT_EVENT_UNSUBSCRIBED, EMQTT_EVENT_ERROR } emqtt_event_kind_t;
typedef struct {
    emqtt_event_kind_t kind;
    emqtt_error_t error;
    int message_id;
    int broker_code;
    int tls_flags;
    emqtt_message_t message;
} emqtt_event_t;

#define ESP_ERR_EMQTT_OUTBOX_FULL 0x7501
#define ESP_ERR_EMQTT_TIME_REQUIRED 0x7502

/* 仅一个实例，由创建它的控制任务调用所有 API。MQTT 回调仅复制有界事件。
 * start/subscribe/stop 可能等待官方 API 锁或网络期限，不得从 SDK 回调调用。
 * poll 的输出由调用者持有，建议静态存储，避免在小任务栈分配 4 KiB。 */
esp_err_t emqtt_create(const emqtt_config_t *config, emqtt_runtime_t **out);
esp_err_t emqtt_start(emqtt_runtime_t *runtime, bool network_ready, bool trusted_time_ready);
esp_err_t emqtt_stop(emqtt_runtime_t *runtime);
esp_err_t emqtt_destroy(emqtt_runtime_t *runtime);
bool emqtt_poll(emqtt_runtime_t *runtime, emqtt_event_t *out);
emqtt_state_t emqtt_state(const emqtt_runtime_t *runtime);
esp_err_t emqtt_enqueue(emqtt_runtime_t *runtime, const char *topic,
                                const void *payload, size_t length, uint8_t qos, bool retain, int *message_id);
/* 动态新增只发送该 filter；仅在匹配的 SUBACK/UNSUBACK 获准后修改期望订阅。
 * 回执失败、超时或断线保留原期望列表，重连重新提交该列表。
 * 请求失败停止会话，start 是显式恢复入口；API 成功不代表回执已确认。 */
esp_err_t emqtt_subscribe(emqtt_runtime_t *runtime, const char *filter, uint8_t qos);
esp_err_t emqtt_unsubscribe(emqtt_runtime_t *runtime, const char *filter);
/* 官方 outbox 的协议字节计数，不是完整 heap 占用或远端处理证明。 */
int emqtt_outbox_size(const emqtt_runtime_t *runtime);
