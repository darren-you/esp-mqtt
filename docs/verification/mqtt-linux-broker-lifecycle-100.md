# MQTT 真实核心与隔离 Broker 百次生命周期回归

日期：2026-09-27。本记录只覆盖主机软件。输入为公开 `esp-mqtt@a4184fe2a54610d1a7302b01e87fe131b64ae3bf` 加本次测试入口；`mqtt_client.c`、`runtime/emqtt.c` 和正式组件配置未修改。实验在 `mac-work-1` 仓外独立源码副本与仓外输出目录进行，没有读取真实设备身份、Broker 凭据或写入设备。

## 输入与方法

| 项目 | 精确值 |
| --- | --- |
| ESP-IDF | `esp-space/esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c`，仓内 `tools/sdk.py check` 通过 |
| lwIP | `esp-space/esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d` |
| 官方核心 `mqtt_client.c` SHA-256 | `e92157b9562cced85b7c542e596971401c2b42f54badb4b84f9f205b69a0967e` |
| 运行层 `runtime/emqtt.c` SHA-256 | `4532059f8cd792b9489450c78d0f0752c2dee632d47e89e37f5fdf0dadf9af0d` |
| 目标与传输 | 固定 SDK `linux` 目标，MQTT 3.1.1 clean session，本机 `127.0.0.1` 临时端口 TCP；明文实验开关开启 |
| 本轮 Linux ELF SHA-256 | `58352fcbc695ab4500d4e530ddb5bd73ff39b5a2a4d6fe3cccabf39f3179d379` |

`tests/linux-broker/main/lifecycle_main.c` 使用同一进程依次创建并销毁 100 个 `emqtt_` 实例。每轮先等待初始 SUBACK→READY，动态新增 filter 并等待第二次 SUBACK→READY，再发布本轮唯一载荷并核对对应 QoS1 PUBACK。隔离 Broker 随后强制关闭 TCP；客户端必须收到 DISCONNECTED，重新 CONNECT，凭 clean session 再次完成初始加动态 filter 的 SUBACK→READY，退订动态 filter 并等到 UNSUBACK。最后确认 outbox 为零，显式 `stop`、`destroy`，记录进程当前 RSS、macOS 默认 malloc zone 的使用量及 0–255 范围内已打开 fd 数。

`tests/linux-broker/run_lifecycle_test.py` 复用已有 Broker 测试的 MQTT 3.1.1 报文读写函数。它逐连接核对 ClientID、clean session、两个连接阶段各自的订阅集、唯一 QoS1 载荷、PUBACK、退订目标和正常停止时的 DISCONNECT。每轮两个连接，每轮新建一个实例；整个过程不依赖设备样例、Mosquitto、生产 Broker 或外部账户。

先导出固定 SDK，然后从独立 checkout 根运行：

```bash
EMQTT_LIFECYCLE_COUNT=2 bash tests/linux-broker/run.sh "$PWD" /private/tmp/esp-mqtt-linux-broker-lifecycle-smoke lifecycle
bash tests/linux-broker/run.sh "$PWD" /private/tmp/esp-mqtt-linux-broker-lifecycle-full lifecycle
```

两个调用使用独立输出目录；100 轮默认值不需要设置计数环境变量。固定 SDK 的 Linux 目标构建只需执行一次后也可直接调用已构建 ELF 的 `run_lifecycle_test.py --count 100`，本次完整运行使用该方式，因此 2 轮与 100 轮的 ELF 字节完全相同。原始输出保存在 `mac-work-1:/private/tmp/esp-mqtt-linux-broker-lifecycle-20260927/smoke.log` 和 `full.log`。

## 执行结果

| 检查 | 结果 |
| --- | --- |
| 2 轮短测 | 2/2 轮通过，4 次 CONNECT、6 次 SUBACK、2 次 QoS1 PUBACK、2 次 UNSUBACK |
| 100 轮完整矩阵 | 100/100 轮通过，200 次 CONNECT、300 次 SUBACK、100 次 QoS1 PUBACK、100 次 UNSUBACK；每轮第二条连接在显式停止时发 MQTT DISCONNECT |
| 预期故障与意外失败 | Broker 每轮强制关闭第一条连接，SDK 报 100 次预期 TCP EOF；随后均收到 DISCONNECTED 并重连。没有 `TEST FAIL`、`BROKER TEST FAIL`、超时或正式源码修复 |
| 销毁后 fd | 100 个采样均为 3；本测试枚举 0–255 范围内的打开 fd |
| 销毁后 macOS 默认 malloc zone 使用量 | 100 个采样均为 99,280 B |
| 销毁后当前 RSS | 首轮 2,260,992 B，末轮 2,867,200 B；第 41–100 轮范围为 2,834,432–2,883,584 B；首十轮中位数 2,572,288 B，末十轮中位数 2,867,200 B。后 60 轮在该范围内波动，未观察到持续增长 |
| 同候选其他回归 | `bash tests/host/run.sh` 的 ASan/UBSan 通过；SDK 工具单测 8/8；设备样例串口伪端口单测 3/3 |

完整日志 SHA-256 为 `4dd56cd5bf68a99b8c72aea53cffa6c4b99a6f9fb99908f4f84423b44cae88c5`，短测构建与运行日志 SHA-256 为 `b1ce6b2b4b28c46b26100bcc89a5233dcc22387c7c1c92e5bae529bf7e6c81bd`。逐轮记录在 100 个 `BROKER CYCLE` 与 100 个 `APP TEST CYCLE` 行中，序号连续且各自唯一；资源判断基于逐轮原始值，不能由末轮数值替代全程趋势。

## 证据边界

主机测试用真实 MQTT 核心、`emqtt_` 运行层与固定 SDK 的 FreeRTOS/transport，但没有 ESP Wi-Fi、SNTP、设备堆、真实 TLS、账户 ACL、两块板的独立资源曲线或业务命令最终 ACK。macOS 的 RSS 与默认 malloc zone 数值仅是主机趋势，不等同于 ESP heap，也不能单独证明无内存泄漏。`examples/broker-client` 的 C3/ESP32 双目标构建和串口驱动另有各自记录；本次没有刷新或执行两块实板，P3-07 不因此完成。
