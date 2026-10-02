// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "emqtt_contract.h"
#include <string.h>

/* Private metadata shared by fixed public messages and variable-size owners.
 * The runtime's length is also its allocation capacity: each owner is created
 * from the first fragment's total length, never resized or reused. */
typedef struct {
    char topic[EMQTT_TOPIC_MAX + 1];
    size_t length;
    int message_id;
    uint8_t qos;
    bool retain, duplicate;
} emqtt_message_info_t;

typedef struct {
    emqtt_message_info_t info;
    uint8_t payload[];
} emqtt_owned_message_t;

typedef struct {
    size_t received;
    bool active;
} emqtt_receive_state_t;

/* This is the sole fragment validation/copy core. The public contract keeps
 * its fixed message layout; runtime owners supply only the declared payload. */
static inline emqtt_rx_result_t emqtt_receive_into(emqtt_receive_state_t *r,
    emqtt_message_info_t *message, uint8_t *payload, size_t capacity,
    const emqtt_fragment_t *f)
{
    if (!f || f->topic_length < 0 || f->data_length < 0 || f->total_length < 0 || f->offset < 0 ||
        f->total_length > (int)EMQTT_PAYLOAD_MAX || (size_t)f->total_length > capacity ||
        f->offset > f->total_length || f->data_length > f->total_length - f->offset ||
        (f->data_length && !f->data) || f->qos < 0 || f->qos > 1 ||
        f->message_id < 0 || f->message_id > 65535 ||
        (f->qos == 1 && f->message_id == 0) || (f->qos == 0 && f->message_id != 0)) goto reject;
    if (!r->active) {
        if (f->offset || !emqtt_topic_valid(f->topic, (size_t)f->topic_length, false)) goto reject;
        memcpy(message->topic, f->topic, (size_t)f->topic_length);
        message->topic[f->topic_length] = '\0';
        message->length = (size_t)f->total_length;
        message->message_id = f->message_id;
        message->qos = (uint8_t)f->qos;
        message->retain = f->retain;
        message->duplicate = f->duplicate;
        r->received = 0;
        r->active = true;
    } else if (f->topic_length && (!f->topic || (size_t)f->topic_length != strlen(message->topic) ||
               memcmp(f->topic, message->topic, (size_t)f->topic_length))) goto reject;
    if ((size_t)f->offset != r->received || (size_t)f->total_length != message->length ||
        f->message_id != message->message_id || f->qos != message->qos ||
        f->retain != message->retain || f->duplicate != message->duplicate ||
        (f->data_length == 0 && f->offset != 0)) goto reject;
    if (f->data_length) memcpy(payload + r->received, f->data, (size_t)f->data_length);
    r->received += (size_t)f->data_length;
    if (r->received == message->length) {
        r->active = false;
        return EMQTT_RX_COMPLETE;
    }
    return EMQTT_RX_MORE;
reject:
    r->active = false;
    r->received = 0;
    return EMQTT_RX_REJECTED;
}
