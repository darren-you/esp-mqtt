#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ -z "${IDF_PATH:-}" ]]; then
  printf 'ESP MQTT C3 构建\n  结果  失败\n  原因  先导出已锁定的 ESP-IDF 环境\n' >&2
  exit 1
fi
build_root="${1:-$(mktemp -d)}"
if [[ -e "$build_root/mqtt" || -L "$build_root/mqtt" ]]; then
  printf 'ESP MQTT C3 构建\n  结果  失败\n  原因  构建目录的 mqtt 入口已存在\n' >&2
  exit 1
fi
mkdir -p "$build_root"
ln -s "$repo_root" "$build_root/mqtt"
printf 'ESP MQTT C3 构建\n  源码  %s\n  输出  %s\n' "$repo_root" "$build_root"
idf.py -C "$repo_root/tests/c3-smoke" -B "$build_root/build" \
  -D SDKCONFIG="$build_root/sdkconfig" \
  -D ESP_MQTT_COMPONENT_DIR="$build_root/mqtt" build
