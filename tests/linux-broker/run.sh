#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  printf '用法：%s <esp-mqtt 源目录> <仓外输出目录> [full|unsub-only|qos1-ack|tls|lifecycle|enqueue-latency|receive-fragments]\n' "$0" >&2
  exit 2
fi
if [[ -z "${IDF_PATH:-}" ]] || ! command -v idf.py >/dev/null 2>&1; then
  printf '请先导出固定 ESP-IDF 环境，且让 idf.py 可用\n' >&2
  exit 2
fi
source_repo="$(cd -- "$1" && pwd -P)"
mkdir -p -- "$2"
output_dir="$(cd -- "$2" && pwd -P)"
scenario="${3:-full}"
if [[ "$scenario" != full && "$scenario" != unsub-only && "$scenario" != qos1-ack && "$scenario" != tls && "$scenario" != lifecycle && "$scenario" != enqueue-latency && "$scenario" != receive-fragments ]]; then
  printf '未知 scenario\n' >&2
  exit 2
fi
if [[ -e "$output_dir/mqtt" || -L "$output_dir/mqtt" ]]; then
  if [[ "$(cd -- "$output_dir/mqtt" && pwd -P)" != "$source_repo" ]]; then
    printf '输出目录 mqtt 入口指向其他源码：%s\n' "$output_dir/mqtt" >&2
    exit 2
  fi
else
  ln -s -- "$source_repo" "$output_dir/mqtt"
fi
harness_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$output_dir/project"
mkdir -p -- "$project_dir/main"
cp -- "$harness_dir/CMakeLists.txt" "$project_dir/"
if [[ "$scenario" == tls ]]; then
  cp -- "$harness_dir/sdkconfig.tls.defaults" "$project_dir/sdkconfig.defaults"
else
  cp -- "$harness_dir/sdkconfig.defaults" "$project_dir/sdkconfig.defaults"
fi
latency_test=OFF
if [[ "$scenario" == enqueue-latency ]]; then
  latency_test=ON
  poll_ms="${EMQTT_TEST_POLL_MS:-1000}"
  case "$poll_ms" in 1000|100|50) ;; *) printf 'EMQTT_TEST_POLL_MS 仅支持 1000、100、50\n' >&2; exit 2 ;; esac
  # Each comparison must start from its own generated sdkconfig.
  if [[ -e "$output_dir/sdkconfig" ]]; then
    printf 'enqueue-latency 要求尚未生成 sdkconfig 的独立输出目录\n' >&2
    exit 2
  fi
  printf '\nCONFIG_FREERTOS_HZ=100\n' >> "$project_dir/sdkconfig.defaults"
  if [[ "$poll_ms" != 1000 ]]; then
    printf 'CONFIG_MQTT_USE_CUSTOM_CONFIG=y\nCONFIG_MQTT_POLL_READ_TIMEOUT_MS=%s\nCONFIG_MQTT_EVENT_QUEUE_SIZE=1\n' "$poll_ms" >> "$project_dir/sdkconfig.defaults"
  fi
fi
if [[ "$scenario" == receive-fragments ]]; then
  printf '\nCONFIG_FREERTOS_HZ=100\nCONFIG_MQTT_USE_CUSTOM_CONFIG=y\nCONFIG_MQTT_POLL_READ_TIMEOUT_MS=50\nCONFIG_MQTT_EVENT_QUEUE_SIZE=1\n' >> "$project_dir/sdkconfig.defaults"
fi
cp -- "$harness_dir/main/CMakeLists.txt" "$project_dir/main/"
if [[ "$scenario" == receive-fragments ]]; then
  cp -- "$harness_dir/main/receive_fragments_main.c" "$project_dir/main/main.c"
elif [[ "$scenario" == enqueue-latency ]]; then
  cp -- "$harness_dir/main/enqueue_latency_main.c" "$project_dir/main/main.c"
elif [[ "$scenario" == tls ]]; then
  cp -- "$harness_dir/main/tls_main.c" "$project_dir/main/main.c"
elif [[ "$scenario" == lifecycle ]]; then
  cp -- "$harness_dir/main/lifecycle_main.c" "$project_dir/main/main.c"
elif [[ "$scenario" == qos1-ack ]]; then
  cp -- "$harness_dir/main/qos1_main.c" "$project_dir/main/main.c"
else
  cp -- "$harness_dir/main/main.c" "$project_dir/main/"
fi
idf.py -C "$project_dir" -B "$output_dir/build" \
  -D SDKCONFIG="$output_dir/sdkconfig" \
  -D EMQTT_ENQUEUE_LATENCY_TEST="$latency_test" \
  -D ESP_MQTT_COMPONENT_DIR="$output_dir/mqtt" build
if [[ "$scenario" == receive-fragments ]]; then
  grep -qx 'CONFIG_MQTT_POLL_READ_TIMEOUT_MS=50' "$output_dir/sdkconfig"
  python3 "$harness_dir/run_receive_fragments_test.py" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
elif [[ "$scenario" == enqueue-latency ]]; then
  if [[ "$poll_ms" == 1000 ]]; then
    grep -qx '# CONFIG_MQTT_USE_CUSTOM_CONFIG is not set' "$output_dir/sdkconfig"
  else
    grep -qx "CONFIG_MQTT_POLL_READ_TIMEOUT_MS=$poll_ms" "$output_dir/sdkconfig"
    grep -qx 'CONFIG_MQTT_EVENT_QUEUE_SIZE=1' "$output_dir/sdkconfig"
  fi
  python3 "$harness_dir/run_enqueue_latency_test.py" --poll-ms "$poll_ms" \
    --output "$output_dir/latency.json" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
elif [[ "$scenario" == tls ]]; then
  if ! grep -qx 'CONFIG_MQTT_TRANSPORT_SSL=y' "$output_dir/sdkconfig" ||
     ! grep -qx '# CONFIG_EMQTT_PLAINTEXT_LAB is not set' "$output_dir/sdkconfig"; then
    printf 'TLS 场景要求启用 SSL 并关闭明文实验开关；请使用独立输出目录\n' >&2
    exit 2
  fi
  python3 "$harness_dir/run_tls_test.py" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
elif [[ "$scenario" == qos1-ack ]]; then
  python3 "$harness_dir/run_qos1_test.py" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
elif [[ "$scenario" == lifecycle ]]; then
  python3 "$harness_dir/run_lifecycle_test.py" \
    --count "${EMQTT_LIFECYCLE_COUNT:-100}" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
else
  python3 "$harness_dir/run_broker_test.py" --scenario "$scenario" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
fi
