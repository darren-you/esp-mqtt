# ESP MQTT 来源归属与 P3a 差异盘点

## 冻结输入

| 事实 | 本轮读取值 | 职责 |
| --- | --- | --- |
| 官方 ESP-MQTT | `espressif/esp-mqtt` tag `v1.1.0`，`1a1e5788a5cf57a0f44a3c6c061407f6c9be1026` | `mqtt_client.c`、`include/`、`lib/`、`Kconfig`、官方示例和测试的上游历史；Apache-2.0 |
| ESP Base 源 | `49df899e9f9f8fe6bc5eade1ea2814c02a498bfa` 工作树读取；本轮所读 MQTT 文件无未提交修改 | 设备内现有第一方适配、测试和实验应用的迁入来源 |
| Base 当时锁定组件 | `firmware/dependencies.lock` 中 `espressif/mqtt` 1.1.0，component hash `fb18bc3b65aa8c94693a9811ffc322cca8a65d92d5ec84983d3e385080e3969c` | P3a 盘点时 Base 仍消费 Registry 包；后续切换见下文 |
| ESP-IDF | `fff9895c82d744c7237be8847347bdd1b07c6643`，本轮核对的原 SDK 工作树干净 | 官方 SDK 基线 |
| esp-lwip | `2758df4cd3666b3b2a5b53830148379326425c0d` | 独立拥有零窗口 ACK 修正；本仓不复制 lwIP 源 |

`tooling/esp-mqtt` 从固定官方提交完整克隆，保留截至该提交的 1076 个上游提交、`v1.1.0` 标签与原 `LICENSE`。第一方提交 `35de71ac9e978b65c2503dbdecb153a272274513` 已推送至公开 `darren-you/esp-mqtt` 的 `master`，官方仓库保留为 `upstream` 远端。上游 Git 测试子模块 `test/tools/paho.mqtt.testing` 尚未初始化，未进入本轮 host 构建。

## 实际源码差异与归属

| 读取范围 | 实测差异 | 裁决 |
| --- | --- | --- |
| Base `managed_components/espressif__mqtt` 与固定官方 v1.1.0 | `mqtt_client.c`、`mqtt5_client.c`、`include/*.h`、`lib` 的 C/头文件和组件 CMake/Kconfig 逐文件相同。差异只在 Registry 重新排序并补 `repository_info` 的 `idf_component.yml`、`.component_hash`/`CHECKSUMS.json` 等打包元数据、上游 Git/CI 元数据与测试子模块展开状态。 | 未发现当前实际编译的官方 MQTT 源码修正；不能把 Registry 打包差异称作协议补丁。新仓从官方源码直接维护，禁止编辑 Base 的生成目录。 |
| Base `components/mqtt_runtime/esp_base_mqtt.c`，最晚来源提交 `88e68bc0cad140295ee068adecc9aa116bde175c` | 持有官方 client、深拷贝配置、单 owner、通知/消息队列、生命周期、事件分类、订阅/退订回执与 READY。 | 复制到 `runtime/emqtt.c`，只在新仓保留通用 `emqtt_` 接口；Base 原件已从 `a0eabdf` 删除。 |
| Base `components/mqtt_runtime/mqtt_contract.c`，最晚来源提交 `c3d22c5be9c6f5943081fc1973d8475597135af8`；配套 `include/*.h` | Topic/过滤器、UTF-8、容量、分片重组、SUBACK 校验属通用合同；ClientID 必须 UUID v4 属 Base 身份策略。 | 复制到 `runtime/emqtt_contract.c` 和 `runtime/include/`；去掉 UUID 形状要求，改为调用方提供的非空、有效 UTF-8、最长 128 字节 ClientID。Base 后续继续装配持久 UUID。 |
| Base `firmware/tests/mqtt_{contract,runtime}_test.c`，运行测试最晚来源提交 `88e68bc0cad140295ee068adecc9aa116bde175c` | SDK 回调/队列 fake 与第一方适配回归。 | 复制、改名并增强 ClientID 边界测试，放入 `tests/host/`；直接包含本仓官方 `mqtt_client.h`，不读 Base 或 `managed_components`。 |
| Base `apps/mqtt_integration` | 仍依赖 Base identity、remote_config、已提交 Wi-Fi/NVS 与私有 Broker 输入。 | 尚未迁入；独立样例必须改用 RAM Wi-Fi、SNTP 和显式实验 ClientID/Topic/CA，不能复制 Base 的持久身份或 NVS 行为。 |
| `esp-frp` SDK 准备工具，来源提交 `4bf6e43017d4d55963409328413f2d48b641dc05` | 公开 IDF/lwIP 精确提交准备、gitlink 检查及真实 Git fixture。 | 复制到本仓 `tools/sdk.py`、`tools/tests/test_sdk.py`，仅调整输出归属为 ESP MQTT；`sdk-lock.json` 与 FRP 版本对齐，独立 checkout 可执行。 |

复制文件的精确原路径还包括 Base `firmware/components/mqtt_runtime/include/esp_base_mqtt.h`（`88e68bc0cad140295ee068adecc9aa116bde175c`）、`include/esp_base_mqtt_contract.h`（`c3d22c5be9c6f5943081fc1973d8475597135af8`）、`firmware/tests/mqtt_contract_test.c`（`88e68bc0cad140295ee068adecc9aa116bde175c`）、`firmware/tests/fakes/{esp_err.h,esp_event.h,esp_timer.h,esp_transport.h,freertos/FreeRTOS.h,freertos/queue.h,freertos/task.h}`（均为 `c3d22c5be9c6f5943081fc1973d8475597135af8`），以及 FRP `tools/sdk.py` 和 `tools/tests/test_sdk.py`（`4bf6e43017d4d55963409328413f2d48b641dc05`）。这些文件分别落到本仓 `runtime/include/`、`tests/host/`、`tools/`。官方文件归 Espressif 原作者与贡献者，Base/FRP 第一方文件归 `darren-you`；均保留 Apache-2.0 来源和代码中的 SPDX 标识。

官方原文件、版权和 Apache-2.0 标识继续由仓内上游历史与 `LICENSE` 保留。本地修改包括路径和前缀硬切、ClientID 责任剥离、组件 CMake/Kconfig/manifest，以及 host 测试入口。官方 MQTT5、QoS2、WS/WSS 源码作为原上游历史保留，首版 `emqtt_` 合同不声称覆盖这些能力。

后续第一方修正直接落在官方来源文件 `mqtt_client.c`、`lib/mqtt_outbox.c` 与 `lib/include/mqtt_outbox.h`：clean session 断线时，在客户端 API 锁内且派发 `DISCONNECTED` 前删除 RAM outbox 中仍未确认的 SUBSCRIBE/UNSUBSCRIBE，保留 PUBLISH/PUBREL。原因是上游自动重连仍会重发旧订阅控制包，而 `emqtt_` 会在新连接按当前期望列表重新订阅；旧退订包如果随后重发，Broker 订阅集就可能与 READY 不一致。官方自定义 outbox 示例 `examples/custom_outbox/main/custom_outbox.cpp` 同步实现新增的分类删除合同，避免示例在链接时缺符号。改动保留原上游版权与许可，不通过消费者构建时补丁叠加；回归同时核对 outbox 分类删除与真实核心/隔离 Broker 的重连行为。

## IDF/lwIP 与配置结论

本轮读取的原 ESP-IDF checkout 位于固定 `fff9895...`，`git status --short` 为空，内含原 lwIP `c6f2f878e7b0f86033214b85547d579be43351e3`；没有发现 IDF 本体其他源码差异。已准备的独立 SDK 同为固定 IDF 提交，唯一故意工作树差异是 `components/lwip/lwip` 指向公开修正 `2758df4...`。`tools/sdk.py check` 已对这份独立 SDK 通过，组件 CMake 要求同一 SDK 和内建 lwIP 组件路径。此结论只覆盖本轮两份具体 checkout 与 MQTT 相关输入，不声称所有机器的 SDK 都相同。

当前构建锁已切至[公开 ESP-IDF fork](https://github.com/darren-you/esp-idf) `578cf89c343e388db43ba1f4ddcd602fedcb763c`，其父提交 `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 修复 `esp_ota_begin` 擦除失败时的句柄泄漏，新提交修复 `esp_http_client_init` 失败时未加入 transport list 的句柄泄漏；最初的官方基线仍是 `fff9895...`。上段保留 P3a 当时的来源核对记录。当前 SDK 检查还核对 IDF 与 lwIP 的完整提交和唯一 lwIP gitlink 差异。

`esp-base@f3c1e3d34a02d97a88494871f042a3398b2255bb` 已将隔离实验应用的精确 MQTT 依赖升至本仓 `9cac455b0184420353ff0283df3f100abaac3e6b`，并以同一 IDF/lwIP 版本对构建默认与实验 C3 工程。该提交当时尚未在普通固件启用 MQTT；后续 `esp-base@ecf1539` 已接入普通 MQTT owner 与设备命令。

Base 的 MQTT `sdkconfig.defaults` 要求 MQTT 3.1.1、严格 TLS、DELETED 通知和证书日期校验，接收/发送缓冲由运行层设为 1024/2304 字节，outbox 协议字节阈值为 16 KiB。新仓保留这些库侧守卫和配置参数；应用仍须显式装配最终 sdkconfig，库不修改全局 SDK 配置。Base 的设备 Topic、LWT 内容、命令 ACK、request_id 幂等、已提交身份/配置均留在 Base。

## 未闭合项

- `tests/c3-smoke` 已在锁定 SDK 构建并链接通过，但只有空配置 API 调用；`examples/broker-client` 已提供独立网络样例并以非敏感合成输入完整链接。[Linux 隔离 Broker 回归](../verification/mqtt-control-reconnect-linux.md)已用真实核心验证 clean session 的 SUB/UNSUB 与 QoS1 重传；C3 实板、TLS、真实 Broker/证书/订阅及资源矩阵仍无新仓结果，P3b 未完成。
- [P3-04 本机回归记录](../verification/p3-host-regression.md)只证明该历史候选当时的官方核心源码未修改；上述后续断线修正已改变官方来源文件，须以当前提交的真实核心与隔离 Broker 回归单独验收。SDK fake 不能替代核心或网络协议回归。
- 首个第一方提交、公开 `master` 和工作区 gitlink 已建立。`esp-base@a0eabdf` 的隔离实验应用已固定本仓完整 SHA，并删除原 `esp_base_mqtt_*` 运行层、重复测试与官方 Registry 依赖；`esp-base@ecf1539` 后普通固件已接入 MQTT owner 与设备命令。Linux 隔离 Broker 回归已有本仓结果，真实 Broker/设备 ACK、C3 实板与迁移仍待验收。
