#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  printf '用法：%s <esp-mqtt 源目录> <仓外输出目录> [full|unsub-only|qos1-ack]\n' "$0" >&2
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
if [[ "$scenario" != full && "$scenario" != unsub-only && "$scenario" != qos1-ack ]]; then
  printf 'scenario 只能是 full、unsub-only 或 qos1-ack\n' >&2
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
cp -- "$harness_dir/CMakeLists.txt" "$harness_dir/sdkconfig.defaults" "$project_dir/"
cp -- "$harness_dir/main/CMakeLists.txt" "$project_dir/main/"
if [[ "$scenario" == qos1-ack ]]; then
  cp -- "$harness_dir/main/qos1_main.c" "$project_dir/main/main.c"
else
  cp -- "$harness_dir/main/main.c" "$project_dir/main/"
fi
idf.py -C "$project_dir" -B "$output_dir/build" \
  -D SDKCONFIG="$output_dir/sdkconfig" \
  -D ESP_MQTT_COMPONENT_DIR="$output_dir/mqtt" build
if [[ "$scenario" == qos1-ack ]]; then
  python3 "$harness_dir/run_qos1_test.py" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
else
  python3 "$harness_dir/run_broker_test.py" --scenario "$scenario" \
    --app "$output_dir/build/esp_mqtt_linux_broker_probe.elf"
fi
