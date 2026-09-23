// SPDX-License-Identifier: Apache-2.0
#include "emqtt.h"
#include "esp_log.h"

static emqtt_config_t config;

void app_main(void)
{
    ESP_LOGI("emqtt_smoke", "runtime API linked; empty config accepted=%d",
             emqtt_config_valid(&config, false));
}
