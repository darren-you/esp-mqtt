#!/usr/bin/env bash
set -euo pipefail
umask 077
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ -z "${IDF_PATH:-}" ]]; then
  printf 'ESP MQTT Broker 样例\n  结果  失败\n  原因  先导出已锁定的 ESP-IDF 环境\n' >&2
  exit 1
fi
build_root="${1:-$(mktemp -d)}"
if [[ -L "$build_root" ]]; then
  printf 'ESP MQTT Broker 样例\n  结果  失败\n  原因  输出目录不能是符号链接\n' >&2
  exit 1
fi
if [[ -e "$build_root/mqtt" || -L "$build_root/mqtt" ]]; then
  printf 'ESP MQTT Broker 样例\n  结果  失败\n  原因  构建目录的 mqtt 入口已存在\n' >&2
  exit 1
fi
mkdir -p "$build_root"
if ! python3 - "$build_root" <<'PY'
import os
import stat
import sys

path = sys.argv[1]
mode = os.stat(path).st_mode
sys.exit(0 if stat.S_ISDIR(mode) and not mode & 0o077 and os.stat(path).st_uid == os.getuid() else 1)
PY
then
  printf 'ESP MQTT Broker 样例\n  结果  失败\n  原因  输出目录必须由当前用户持有且不允许其他用户访问\n' >&2
  exit 1
fi
ln -s "$repo_root" "$build_root/mqtt"
printf 'ESP MQTT Broker 样例\n  源码  %s\n  输出  %s\n' "$repo_root" "$build_root"
idf.py -C "$repo_root/examples/broker-client" -B "$build_root/build" \
  -D SDKCONFIG="$build_root/sdkconfig" \
  -D ESP_MQTT_COMPONENT_DIR="$build_root/mqtt" \
  -D EMQTT_SAMPLE_INPUTS="${EMQTT_SAMPLE_INPUTS:-$repo_root/examples/broker-client/inputs.example.h}" build
