# ESP MQTT P3-04 本机回归记录

记录日期：2026-09-23。范围为本仓首个第一方提交 `35de71ac9e978b65c2503dbdecb153a272274513` 包含的源码候选；此记录不代表 Broker 联网或实板验收。

## 固定输入

| 输入 | 精确值 |
| --- | --- |
| 官方 MQTT 历史基线 | `espressif/esp-mqtt` v1.1.0，`1a1e5788a5cf57a0f44a3c6c061407f6c9be1026` |
| ESP-IDF | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| esp-lwip | `2758df4cd3666b3b2a5b53830148379326425c0d` |
| 当前源码候选摘要 | SHA-256 `c4c19bb6005409a70c57cd984fb5da8627c70cc2796d5f60e5ae82730b06f1e3`，46 个文件 |

候选摘要按路径字典序遍历：固定文件 `mqtt_client.c`、`mqtt5_client.c`、`CMakeLists.txt`、`Kconfig`、`idf_component.yml`、`sdk-lock.json`，以及 `lib/`、`include/`、`runtime/`、`tests/host/`、`examples/broker-client/` 下扩展名为 `.c`、`.h`、`.txt`、`.json`、`.lock`、`.sh`、`.yml`、`.defaults` 的文件。对每个文件依次向 SHA-256 输入相对路径 UTF-8、NUL、原始文件字节、NUL。报告文件不参与摘要。

`git merge-base --is-ancestor 1a1e5788a5cf57a0f44a3c6c061407f6c9be1026 HEAD` 与 `git diff --quiet 1a1e5788a5cf57a0f44a3c6c061407f6c9be1026 HEAD -- mqtt_client.c mqtt5_client.c lib include` 均返回 0：官方核心源码和头文件相对固定上游提交未改动。本次没有需要以 SDK fake 冒充的官方协议核心源码补丁。

## 运行结果

| 门禁 | 结果与实际范围 |
| --- | --- |
| `bash tests/host/run.sh` | 通过；C11 + ASan/UBSan 执行 `emqtt_contract.c` 与 `emqtt.c` 的合同和运行层测试；fake 注入 SDK API、事件和队列 |
| `python3 -m unittest discover -s tools/tests -p 'test_*.py'` | 8/8 通过；SDK 锁定与准备工具测试 |
| `python3 tools/sdk.py check --path <固定 SDK checkout>` | 通过；回读上述 IDF 和 lwIP 完整 SHA |
| 独立 C3 Broker 样例 | 固定 SDK 下以非敏感合成输入完整编译并链接；`compile_commands.json` 回读确认真实本仓 `mqtt_client.c` 参与构建；镜像 953,504 字节（`0xe8ca0`），SHA-256 `6101f36852ad7057a9949278a6ddf394e108472b16c9187e78eec527a4cd85c0` |
| MQTT 多项事件队列负例 | `CONFIG_MQTT_EVENT_QUEUE_SIZE=2` 在 `runtime/emqtt.c` 编译期按预期拒绝，避免把借用的 DATA 指针跨异步回调使用 |

host fake 没有实现 MQTT wire 编解码、Broker、TLS、FreeRTOS 并发或网络。C3 镜像未写板，合成输入不连接真实 Broker。P3-07 要以同一已提交候选 SHA 做独立 Broker/C3 矩阵、百次资源回收与实板验证；本记录不替代这些结果。
