// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EMQTT_PAYLOAD_MAX 4096u
#define EMQTT_TOPIC_MAX 256u
#define EMQTT_CLIENT_ID_MAX 128u
#define EMQTT_SUBSCRIPTIONS_MAX 8u
#define EMQTT_CA_MAX 4096u
#define EMQTT_OUTBOX_LIMIT 16384u

typedef struct {
    char topic[EMQTT_TOPIC_MAX + 1];
    uint8_t qos;
} emqtt_subscription_t;

/* 按值保存输入；证书不借用调用者的临时缓冲区。没有 URI、跳过校验或备用地址。 */
typedef struct {
    char hostname[254];
    uint16_t port;
    bool tls;
    char client_id[EMQTT_CLIENT_ID_MAX + 1];
    char username[129];
    char password[257];
    char ca_pem[EMQTT_CA_MAX + 1];
    char will_topic[EMQTT_TOPIC_MAX + 1];
    uint8_t will_payload[512];
    size_t will_length;
    uint8_t will_qos;
    bool will_retain;
    emqtt_subscription_t subscriptions[EMQTT_SUBSCRIPTIONS_MAX];
    size_t subscription_count;
} emqtt_config_t;

typedef struct {
    char topic[EMQTT_TOPIC_MAX + 1];
    uint8_t payload[EMQTT_PAYLOAD_MAX];
    size_t length;
    int message_id;
    uint8_t qos;
    bool retain;
    bool duplicate;
} emqtt_message_t;

/* 官方 MQTT_EVENT_DATA 的借用字段；此类型不是 MQTT 报文 parser。 */
typedef struct {
    const char *topic;
    int topic_length;
    const void *data;
    int data_length;
    int total_length;
    int offset;
    int message_id;
    int qos;
    bool retain;
    bool duplicate;
} emqtt_fragment_t;

typedef struct {
    /* 调用方拥有消息存储；重组期间不得复用或释放。 */
    emqtt_message_t *message;
    size_t received;
    bool active;
} emqtt_receiver_t;

typedef enum {
    EMQTT_RX_MORE,
    EMQTT_RX_COMPLETE,
    EMQTT_RX_REJECTED
} emqtt_rx_result_t;

bool emqtt_topic_valid(const char *topic, size_t length, bool filter);
bool emqtt_config_valid(const emqtt_config_t *config, bool allow_plaintext_lab);
/* 完整数据归调用方提供的 message 所有，只在 COMPLETE 时可消费。 */
emqtt_rx_result_t emqtt_receive(emqtt_receiver_t *receiver, const emqtt_fragment_t *fragment);
void emqtt_receive_reset(emqtt_receiver_t *receiver);
/* 每个返回码必须与本次订阅逐项对应；任何拒绝、截断或额外项均失败。 */
bool emqtt_suback_valid(const uint8_t *codes, size_t count,
                            const emqtt_subscription_t *subscriptions, size_t expected_count);
