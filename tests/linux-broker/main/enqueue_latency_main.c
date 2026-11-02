#include "emqtt.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

enum { BLOCKS = 12, BLOCK_BYTES = 1024, TRACE_LIMIT = 512 };
typedef struct { const char *kind; int64_t us; int a; int b; } trace_t;
static trace_t traces[TRACE_LIMIT];
static atomic_uint trace_count;
static atomic_int poll_sequence, active_poll;
static atomic_llong poll_started;
static emqtt_event_t event;

/* Same monotonic clock as the existing Linux harness; no transport emulation. */
int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void trace(const char *kind, int a, int b)
{
    unsigned i = atomic_fetch_add(&trace_count, 1);
    if (i >= TRACE_LIMIT) abort();
    traces[i] = (trace_t){kind, esp_timer_get_time(), a, b};
}

/* Only mqtt_client.c's call is redirected. Wait and return are the real SDK's. */
int emqtt_test_transport_poll_read(esp_transport_handle_t transport, int timeout_ms)
{
    int seq = atomic_fetch_add(&poll_sequence, 1) + 1;
    trace("poll_begin", seq, timeout_ms);
    atomic_store(&poll_started, esp_timer_get_time());
    atomic_store(&active_poll, seq);
    int result = esp_transport_poll_read(transport, timeout_ms);
    atomic_store(&active_poll, 0);
    trace("poll_end", seq, result);
    return result;
}

static int64_t cpu_us(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) abort();
    return ((int64_t)usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 +
           usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}

static void finish(emqtt_runtime_t *runtime, const char *failure)
{
    if (runtime && emqtt_destroy(runtime) != ESP_OK) failure = "destroy";
    unsigned count = atomic_load(&trace_count);
    for (unsigned i = 0; i < count; ++i)
        printf("TRACE %s %lld %d %d\n", traces[i].kind,
               (long long)traces[i].us, traces[i].a, traces[i].b);
    printf("TEST %s %s\n", failure ? "FAIL" : "PASS", failure ? failure : "enqueue_latency");
    exit(failure ? 1 : 0);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *port_text = getenv("EMQTT_TEST_PORT");
    const long port = port_text ? strtol(port_text, NULL, 10) : 0;
    if (port < 1 || port > 65535) finish(NULL, "port");
    emqtt_config_t config = {.hostname = "127.0.0.1", .port = (uint16_t)port,
        .tls = false, .client_id = "linux-enqueue-latency", .will_topic = "test/will",
        .subscriptions = {{.topic = "test/data", .qos = 1}}, .subscription_count = 1};
    emqtt_runtime_t *runtime = NULL;
    if (emqtt_create(&config, &runtime) != ESP_OK ||
        emqtt_start(runtime, true, true) != ESP_OK) finish(runtime, "start");
    bool ready = false, acknowledged = true, received = true;
    int block = -1, message_id = -1;
    int64_t idle_until = 0, idle_cpu = 0;
    const int64_t deadline = esp_timer_get_time() + 35000000;
    while (esp_timer_get_time() < deadline) {
        while (emqtt_poll(runtime, &event)) {
            if (event.kind == EMQTT_EVENT_READY) {
                if (ready) finish(runtime, "duplicate_ready");
                ready = true;
            } else if (event.kind == EMQTT_EVENT_PUBACK) {
                if (block < 0 || acknowledged || event.message_id != message_id)
                    finish(runtime, "puback");
                trace("puback", block, message_id);
                acknowledged = true;
            } else if (event.kind == EMQTT_EVENT_MESSAGE) {
                const emqtt_message_t *m = &event.message;
                if (block < 0 || received || strcmp(m->topic, "test/data") ||
                    m->length != BLOCK_BYTES || m->qos != 1 || m->retain || m->duplicate ||
                    m->message_id != 0x4000 + block) finish(runtime, "data_metadata");
                for (unsigned i = 0; i < m->length; ++i)
                    if ((unsigned char)m->payload[i] != (unsigned char)(block + i))
                        finish(runtime, "data_bytes");
                trace("data", block, BLOCK_BYTES);
                received = true;
            } else if (event.kind == EMQTT_EVENT_ERROR || event.kind == EMQTT_EVENT_DISCONNECTED) {
                finish(runtime, "connection");
            }
        }
        if (ready && acknowledged && received) {
            if (block + 1 == BLOCKS) {
                if (!idle_until) {
                    trace("idle_begin", atomic_load(&poll_sequence), 0);
                    idle_cpu = cpu_us();
                    idle_until = esp_timer_get_time() + 3000000;
                    vTaskDelay(pdMS_TO_TICKS(3000));
                } else if (esp_timer_get_time() >= idle_until) {
                    trace("idle_end", atomic_load(&poll_sequence), (int)(cpu_us() - idle_cpu));
                    if (emqtt_outbox_size(runtime)) finish(runtime, "outbox");
                    finish(runtime, NULL);
                }
            } else {
                int active = atomic_load(&active_poll);
                // Observe a poll already in progress, without delaying the MQTT task.
                // The owner yields on its regular 10 ms FreeRTOS tick below.
                if (active && esp_timer_get_time() - atomic_load(&poll_started) >= 1000) {
                    char request[24];
                    int length = snprintf(request, sizeof request, "%d", ++block);
                    trace("enqueue_begin", block, active);
                    if (emqtt_enqueue(runtime, "test/request", request, (size_t)length,
                                      1, false, &message_id) != ESP_OK || message_id <= 0)
                        finish(runtime, "enqueue");
                    trace("enqueue_end", block, message_id);
                    if (atomic_load(&active_poll) != active) finish(runtime, "poll_phase_changed");
                    acknowledged = received = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    finish(runtime, "timeout");
}
