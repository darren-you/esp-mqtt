#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 || -z "${IDF_PATH:-}" ]] || ! command -v idf.py >/dev/null 2>&1; then
  printf '用法：先导出固定 SDK，再运行 %s <源码目录> <仓外输出目录>\n' "$0" >&2
  exit 2
fi
source_repo="$(cd -- "$1" && pwd -P)"
mkdir -p -- "$2"
output_dir="$(cd -- "$2" && pwd -P)"
if [[ -e "$output_dir/mqtt" || -L "$output_dir/mqtt" ]]; then
  [[ "$(cd -- "$output_dir/mqtt" && pwd -P)" == "$source_repo" ]] || exit 2
else
  ln -s -- "$source_repo" "$output_dir/mqtt"
fi
harness_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
idf.py -C "$harness_dir" -B "$output_dir/build" \
  -D SDKCONFIG="$output_dir/sdkconfig" \
  -D ESP_MQTT_COMPONENT_DIR="$output_dir/mqtt" build
python3 - "$output_dir/build/esp_mqtt_linux_lifecycle_probe.elf" <<'PY'
import os
import subprocess
import sys

failed = False
for scenario in ("immediate-stop", "init-oom", "register-oom", "tls-init-oom", "tls-register-oom",
                 "concurrent-stop"):
    result = subprocess.run([sys.argv[1]], env={**os.environ, "EMQTT_LIFECYCLE_SCENARIO": scenario},
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    print(result.stdout, end="")
    failed |= result.returncode != 0
raise SystemExit(1 if failed else 0)
PY
