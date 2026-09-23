#!/usr/bin/env bash
set -euo pipefail
umask 077

for program in mosquitto mosquitto_pub mosquitto_sub openssl python3; do
  if ! command -v "$program" >/dev/null 2>&1; then
    printf 'ESP MQTT Broker 回环\n  结果  失败\n  原因  缺少 %s\n' "$program" >&2
    exit 1
  fi
done

lab_dir="$(mktemp -d "${TMPDIR:-/tmp}/emqtt-broker-loopback.XXXXXXXX")"
broker_pid=""
cleanup() {
  if [[ -n "$broker_pid" ]]; then
    kill "$broker_pid" 2>/dev/null || true
    wait "$broker_pid" 2>/dev/null || true
  fi
  rm -rf -- "$lab_dir"
}
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
  -keyout "$lab_dir/ca.key" -out "$lab_dir/ca.crt" \
  -subj '/CN=ESP MQTT Loopback CA' >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes \
  -keyout "$lab_dir/server.key" -out "$lab_dir/server.csr" \
  -subj '/CN=127.0.0.1' >/dev/null 2>&1
printf 'subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n' > "$lab_dir/server.ext"
openssl x509 -req -in "$lab_dir/server.csr" -CA "$lab_dir/ca.crt" \
  -CAkey "$lab_dir/ca.key" -CAcreateserial -out "$lab_dir/server.crt" \
  -days 1 -sha256 -extfile "$lab_dir/server.ext" >/dev/null 2>&1

port="$(python3 - <<'PY'
import socket
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    print(listener.getsockname()[1])
PY
)"
cat > "$lab_dir/mosquitto.conf" <<EOF
listener $port 127.0.0.1
protocol mqtt
allow_anonymous true
persistence false
certfile $lab_dir/server.crt
keyfile $lab_dir/server.key
log_dest file $lab_dir/mosquitto.log
EOF
mosquitto -c "$lab_dir/mosquitto.conf" > "$lab_dir/broker.stdout" 2>&1 &
broker_pid=$!

broker_args=(-h 127.0.0.1 -p "$port" --cafile "$lab_dir/ca.crt")
ready=false
for ((attempt = 0; attempt < 50; attempt++)); do
  if mosquitto_pub "${broker_args[@]}" -t lab/esp-mqtt/ready -m ready >/dev/null 2>&1; then
    ready=true
    break
  fi
  sleep 0.1
done
if [[ "$ready" != true ]]; then
  printf 'ESP MQTT Broker 回环\n  结果  失败\n  原因  本地 TLS Broker 未启动\n' >&2
  cat "$lab_dir/broker.stdout" "$lab_dir/mosquitto.log" 2>/dev/null >&2 || true
  exit 1
fi

mosquitto_sub "${broker_args[@]}" -t lab/esp-mqtt/qos -q 1 -C 2 -W 5 \
  > "$lab_dir/qos.received" &
subscriber_pid=$!
sleep 0.2
mosquitto_pub "${broker_args[@]}" -t lab/esp-mqtt/qos -q 0 -m qos0
mosquitto_pub "${broker_args[@]}" -t lab/esp-mqtt/qos -q 1 -m qos1
wait "$subscriber_pid"
printf 'qos0\nqos1\n' > "$lab_dir/qos.expected"
cmp "$lab_dir/qos.expected" "$lab_dir/qos.received"

mosquitto_pub "${broker_args[@]}" -t lab/esp-mqtt/retain -q 1 -r -m retained
mosquitto_sub "${broker_args[@]}" -t lab/esp-mqtt/retain -C 1 -W 5 \
  > "$lab_dir/retain.received"
printf 'retained\n' > "$lab_dir/retain.expected"
cmp "$lab_dir/retain.expected" "$lab_dir/retain.received"

for size in 4096 4097; do
  python3 - "$lab_dir/payload" "$size" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b"A" * int(sys.argv[2]))
PY
  mosquitto_sub "${broker_args[@]}" -t lab/esp-mqtt/size -C 1 -W 5 -N \
    > "$lab_dir/size.received" &
  subscriber_pid=$!
  sleep 0.2
  mosquitto_pub "${broker_args[@]}" -t lab/esp-mqtt/size -q 1 -f "$lab_dir/payload"
  wait "$subscriber_pid"
  cmp "$lab_dir/payload" "$lab_dir/size.received"
done

printf 'ESP MQTT Broker 回环\n  结果  通过\n  范围  仅主机 Mosquitto TLS、QoS0/1、retained、4096/4097 字节\n  监听  127.0.0.1:%s（已清理）\n' "$port"
