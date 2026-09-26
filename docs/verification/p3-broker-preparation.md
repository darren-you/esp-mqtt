# ESP MQTT P3-06/P3-07 Broker 软件准备记录

首次记录日期：2026-09-23。样例与驱动源码候选为 `49ccc4662e267a92040bcd645b10556547ab7338`；本节只证明当时的主机回环、C3 样例构建和诊断入口，不证明真实 Broker/C3 运行。2026-09-26 的双目标构建追加记录见末节。

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

在 2026-09-23 这轮，尚未核对真实实验 Broker 的可达 DNS/SAN、测试 CA/ACL/账号输入、授权后的板卡与串口事实及两份匹配的完整 Flash 恢复基线，也未执行设备写入。P3-06 的真实联网 C3 条件与 P3-07 全矩阵、百次资源回收均未验收。当时人工断电仍暂缓；2026-09-26 两板恢复基线与人工条件已具备，真实断电仍未执行，不能用软件复位代替。

## 2026-09-26 双目标构建追加

当前样例已为 ESP32-C3（`esp32c3`）和 ESP32-D0WD-V3（`esp32`）拆分控制台配置：前者使用 USB Serial/JTAG，后者使用 UART0/CH340、115200 baud；两个 target 的实验 ClientID、Topic 和 Broker 账户/ACL 应在各自仓外输入头中隔离。本轮使用仓内空示例，未提供网络或 Broker 凭据。空输入在联网前拒绝启动，因此以下结果只证明编译和链接。

源码基线是本仓 `master@84ad972d94850cdfd9b6cba606d3af7a56fab22a` 加本轮未提交工作树修改，并非已发布的固定组件提交。构建在 `mac-work-1` 的仓外私密源码副本运行；其 `build.sh`、`main/CMakeLists.txt`、`main/app_main.c`、三份 `sdkconfig.defaults*`、根组件入口、官方客户端和运行层源码摘要与当前工作树逐文件一致。固定 SDK/lwIP 经本仓 `tools/sdk.py check` 核对为 `578cf89c343e388db43ba1f4ddcd602fedcb763c` / `2758df4cd3666b3b2a5b53830148379326425c0d`。工具链为 Xtensa/RISC-V GCC 15.2.0（esp-15.2.0_20251204）、CMake 3.31.10 和 Ninja 1.13.2。

从该副本根目录导出固定 SDK 后，分别执行：

```bash
bash examples/broker-client/build.sh /Users/darrenyou/.cache/darren-space/esp-mqtt-broker-c3 esp32c3
bash examples/broker-client/build.sh /Users/darrenyou/.cache/darren-space/esp-mqtt-broker-esp32-final esp32
```

| Target | 最终配置 / 编译核对 | 镜像大小 | SHA-256 |
| --- | --- | ---: | --- |
| `esp32c3` | USB Serial/JTAG；4 MiB；`mqtt_client.c`、`runtime/emqtt.c`、`app_main.c` 均在 `compile_commands.json` | 954096 B | `787666947fe75737f4597c51ecb479273e41e89188f8cdb21d5bd0a903dcafb6` |
| `esp32` | UART0/115200；4 MiB；上述三份源码均在 `compile_commands.json` | 899920 B | `ead92c75358adcc527a681d72bb204b1bf0c4579df3b6312edc535be83bb819a` |

两份最终 `sdkconfig` 都启用 `MQTT_REPORT_DELETED_MESSAGES` 与 `MBEDTLS_HAVE_TIME_DATE`，禁用 PHY 校准 NVS 存储和实验明文开关；`esp32` 没有 USB Serial/JTAG 或第二控制台配置项。本机 host ASan/UBSan 运行层测试、SDK 工具 8 项及串口驱动伪端口 3 项通过；`build.sh` 语法与 Git diff 检查通过。P1-04 已在两板建立私有恢复基线，但本次构建没有连接 Broker、串口或设备，也没有刷写。候选须保存为公开精确提交并在两台真实设备分别完成第 7.4 节矩阵后，才能关闭 P3-06/P3-07；C3 低内存分支的运行层改动还须与本轮样例改动合并并重新验证对应制品。
