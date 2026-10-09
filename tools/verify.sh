#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
bash tests/host/run.sh
python3 -m unittest discover -s tools/tests -p 'test_*.py'
printf 'ESP MQTT 验证\n  host 与工具合同  通过\n  实板与发布      未执行\n'
