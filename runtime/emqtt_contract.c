// SPDX-License-Identifier: Apache-2.0
#include "emqtt_contract.h"
#include "emqtt_receive_internal.h"
#include <string.h>

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && text[length]) ++length;
    return length;
}

static bool utf8(const char *text, size_t length)
{
    for (size_t at = 0; at < length;) {
        const uint8_t first = (uint8_t)text[at++];
        uint32_t code = first, minimum = 0;
        size_t extra = 0;
        if (first >= 0xc2 && first <= 0xdf) { code &= 0x1f; extra = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { code &= 0x0f; extra = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { code &= 7; extra = 3; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        if (extra > length - at) return false;
        while (extra--) {
            const uint8_t byte = (uint8_t)text[at++];
            if ((byte & 0xc0) != 0x80) return false;
            code = (code << 6) | (byte & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
            code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            (code >= 0xfdd0 && code <= 0xfdef) || (code & 0xffff) >= 0xfffe) return false;
    }
    return true;
}

bool emqtt_topic_valid(const char *topic, size_t length, bool filter)
{
    if (!topic || length == 0 || length > EMQTT_TOPIC_MAX || !utf8(topic, length)) return false;
    for (size_t i = 0; i < length; ++i) {
        if (topic[i] == '#' && (!filter || i + 1 != length || (i > 0 && topic[i - 1] != '/'))) return false;
        if (topic[i] == '+' && (!filter || (i > 0 && topic[i - 1] != '/') ||
                               (i + 1 < length && topic[i + 1] != '/'))) return false;
    }
    return true;
}

bool emqtt_config_valid(const emqtt_config_t *c, bool allow_plaintext_lab)
{
    if (!c || c->port == 0 || (!c->tls && !allow_plaintext_lab) ||
        c->subscription_count > EMQTT_SUBSCRIPTIONS_MAX || c->will_qos > 1 ||
        c->will_length > sizeof(c->will_payload)) return false;
    const size_t host_len = bounded_length(c->hostname, sizeof(c->hostname));
    if (!host_len || host_len == sizeof(c->hostname)) return false;
    for (size_t i = 0, label = 0; i < host_len; ++i) {
        const char ch = c->hostname[i];
        if (ch == '.') {
            if (!label || c->hostname[i - 1] == '-' || i + 1 == host_len) return false;
            label = 0;
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || (ch == '-' && label > 0))) return false;
            if (++label > 63 || (i + 1 == host_len && ch == '-')) return false;
        }
    }
    const size_t client_id_len = bounded_length(c->client_id, sizeof(c->client_id));
    if (!client_id_len || client_id_len == sizeof(c->client_id) ||
        !utf8(c->client_id, client_id_len)) return false;
    const size_t user_len = bounded_length(c->username, sizeof(c->username));
    const size_t password_len = bounded_length(c->password, sizeof(c->password));
    if (user_len == sizeof(c->username) || password_len == sizeof(c->password) ||
        !utf8(c->username, user_len) || !utf8(c->password, password_len) || (!user_len && password_len)) return false;
    const size_t cert_len = bounded_length(c->ca_pem, sizeof(c->ca_pem));
    if (cert_len == sizeof(c->ca_pem) || (c->tls && (!cert_len ||
        !strstr(c->ca_pem, "-----BEGIN CERTIFICATE-----") || !strstr(c->ca_pem, "-----END CERTIFICATE-----"))) ||
        (!c->tls && cert_len)) return false;
    const size_t will_len = bounded_length(c->will_topic, sizeof(c->will_topic));
    if (!emqtt_topic_valid(c->will_topic, will_len, false)) return false;
    for (size_t i = 0; i < c->subscription_count; ++i) {
        const emqtt_subscription_t *sub = &c->subscriptions[i];
        if (sub->qos > 1 || !emqtt_topic_valid(sub->topic, bounded_length(sub->topic, sizeof(sub->topic)), true)) return false;
        for (size_t j = 0; j < i; ++j) if (!strcmp(sub->topic, c->subscriptions[j].topic)) return false;
    }
    return true;
}

void emqtt_receive_reset(emqtt_receiver_t *receiver)
{
    if (receiver) { receiver->active = false; receiver->received = 0; }
}

emqtt_rx_result_t emqtt_receive(emqtt_receiver_t *r, const emqtt_fragment_t *f)
{
    if (!r) return EMQTT_RX_REJECTED;
    if (!r->message) { emqtt_receive_reset(r); return EMQTT_RX_REJECTED; }
    emqtt_message_t *message = r->message;
    emqtt_message_info_t info = {0};
    if (r->active) {
        memcpy(info.topic, message->topic, sizeof info.topic);
        info.length = message->length; info.message_id = message->message_id;
        info.qos = message->qos; info.retain = message->retain; info.duplicate = message->duplicate;
    }
    emqtt_receive_state_t state = {.received = r->received, .active = r->active};
    const emqtt_rx_result_t result = emqtt_receive_into(&state, &info,
        message->payload, sizeof message->payload, f);
    r->received = state.received; r->active = state.active;
    if (result != EMQTT_RX_REJECTED) {
        memcpy(message->topic, info.topic, sizeof message->topic);
        message->length = info.length; message->message_id = info.message_id;
        message->qos = info.qos; message->retain = info.retain; message->duplicate = info.duplicate;
    }
    return result;
}

bool emqtt_suback_valid(const uint8_t *codes, size_t count,
                            const emqtt_subscription_t *subscriptions, size_t expected)
{
    if (!codes || !subscriptions || !expected || expected > EMQTT_SUBSCRIPTIONS_MAX || count != expected) return false;
    for (size_t i = 0; i < count; ++i) if (codes[i] > 1 || codes[i] > subscriptions[i].qos) return false;
    return true;
}
