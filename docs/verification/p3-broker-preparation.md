# ESP MQTT P3-06/P3-07 Broker 软件准备记录

记录日期：2026-09-23。样例与驱动源码候选为 `49ccc4662e267a92040bcd645b10556547ab7338`；本记录只证明主机回环、样例构建和诊断入口，不证明真实 Broker/C3 运行。

## 固定输入与结果

| 项目 | 本轮事实 |
| --- | --- |
| 官方 MQTT 基线 | v1.1.0，`1a1e5788a5cf57a0f44a3c6c061407f6c9be1026`；该提交仍为候选祖先，`mqtt_client.c`、`mqtt5_client.c`、`lib/`、`include/` 相比基线无差异 |
| ESP-IDF | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| esp-lwip | `2758df4cd3666b3b2a5b53830148379326425c0d` |
| `bash tests/host/run.sh` | ASan/UBSan 的运行层合同与 SDK fake 测试通过；未运行官方 MQTT wire/TLS/Broker |
| `python3 -m unittest discover -s tools/tests -p 'test_*.py'` | SDK 工具 8/8 通过 |
| `python3 -m unittest discover -s examples/broker-client -p 'test_serial_cycles.py'` | 串口驱动伪端口 3/3 通过，包含在线入队失败及缺失 PUBACK 的拒绝；未连接真实串口 |
| `bash examples/broker-client/lab-broker-loopback.sh` | 本机 Mosquitto 2.1.2 的一次性 TLS 回环通过：QoS0/1、retained、4096/4097 字节原样传输；仅监听 `127.0.0.1`，退出清理；不运行 ESP-MQTT 客户端 |
| 固定 SDK C3 样例 | `examples/broker-client/build.sh` 在空实验输入下完整编译/链接，`compile_commands.json` 确认本仓 `mqtt_client.c`、`runtime/emqtt.c` 和样例源码均参与编译；镜像 953,440 字节，SHA-256 `7949aeb7aa0bfe90f2956e279aef54b88d31e74124024a447e218dcec514578f`；未刷板 |

固定 SDK 检查命令为 `python3 tools/sdk.py check --path <已锁定独立 SDK checkout>`，返回上述完整 IDF/lwIP SHA。C3 构建沿用[样例 README](../../examples/broker-client/README.md)的仓外私密输出目录，不设置 `EMQTT_SAMPLE_INPUTS`，所以生成镜像的空输入会在联网前拒绝启动；这个镜像不能用来声称 TLS/Broker 通过。实际 `sdkconfig` 开启 `CONFIG_MQTT_REPORT_DELETED_MESSAGES=y` 与 `CONFIG_MBEDTLS_HAVE_TIME_DATE=y`，没有启用实验明文开关或 PHY 校准 NVS 存储。

## 待执行的独立实板矩阵

同一已提交候选 SHA 的隔离 Broker 与真实 ESP32-C3 需要分别留下设备事件、Broker 记录、输入/配置摘要和资源 CSV：

| 场景 | 当前准备 | 真实通过条件 |
| --- | --- | --- |
| 严格 TLS、QoS0/1、4096/4097 字节 | 固定命令、接收长度/CRC32、主机回环已备 | 设备 READY/PUBACK/完整 4 KiB 收发；超限拒绝；Broker 原始字节对应 |
| 动态订阅/退订、SUBACK 拒绝 | 固定 Topic 命令和错误日志已备 | 逐项回执与实际交付/不再交付对应，ACL 拒绝进入失败 |
| retained/LWT、错误账号/CA | 样例有 retained 在线和 LWT 配置、错误映射日志 | Broker 与设备双侧证据；异常断开可见 LWT，错误输入不得 READY |
| Broker 重启、Wi-Fi 恢复、丢 ACK/重复消息 | `wifi_down`/`wifi_up`、离线 `fill`、outbox/事件日志已备 | 真实恢复、DELETED/满错误、故障注入报文与 DUP/重传语义对应；普通 Mosquitto 回环没有丢 ACK 注入能力 |
| stop/start/destroy 与 100 次回收 | `cycle`、逐轮 READY/PUBACK/空 outbox CSV 驱动已备 | 同候选实板跑满 100 次，heap/最大连续块/socket/计时器及 Broker 连接记录无持续累积 |

本轮未核对真实实验 Broker 的可达 DNS/SAN、测试 CA/ACL/账号输入、授权后的板卡与串口事实及两份匹配的完整 Flash 恢复基线；也未执行设备写入。P3-06 的真实联网 C3 条件与 P3-07 全矩阵、百次资源回收都保持未验收。人工断电仍按总计划暂缓，不能用软件复位代替。
