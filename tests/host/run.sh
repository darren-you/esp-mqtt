#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT
compiler="${CC:-cc}"
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined)
"$compiler" "${flags[@]}" -I "$repo_root/runtime/include" \
  "$repo_root/runtime/emqtt_contract.c" "$repo_root/tests/host/emqtt_contract_test.c" \
  -o "$build_dir/emqtt_contract_test"
"$build_dir/emqtt_contract_test"
"$compiler" "${flags[@]}" -c \
  -DCONFIG_MQTT_REPORT_DELETED_MESSAGES=1 -DCONFIG_EMQTT_PLAINTEXT_LAB=1 -DCONFIG_MBEDTLS_HAVE_TIME_DATE=1 \
  -Dcalloc=emqtt_test_calloc -Dfree=emqtt_test_free \
  -include "$repo_root/tests/host/fakes/emqtt_test_alloc.h" \
  -I "$repo_root/tests/host/fakes" -I "$repo_root/runtime/include" -I "$repo_root/include" \
  "$repo_root/runtime/emqtt.c" -o "$build_dir/emqtt.o"
"$compiler" "${flags[@]}" -DCONFIG_EMQTT_PLAINTEXT_LAB=1 \
  -I "$repo_root/tests/host/fakes" -I "$repo_root/runtime/include" -I "$repo_root/include" \
  "$repo_root/runtime/emqtt_contract.c" "$repo_root/tests/host/emqtt_runtime_test.c" \
  "$build_dir/emqtt.o" -o "$build_dir/emqtt_runtime_test"
"$build_dir/emqtt_runtime_test"
printf 'ESP MQTT host\n  合同与运行层  通过\n  sanitizer     ASan/UBSan\n  设备与 Broker  未使用\n'
