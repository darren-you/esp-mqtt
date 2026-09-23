# MQTT 订阅控制报文断线回归

记录日期：2026-09-24。本记录核对 clean session 下未确认的动态 SUBSCRIBE / UNSUBSCRIBE 是否跨自动重连重放，同时核对 QoS1 PUBLISH 是否仍能重试。测试使用仓内真实 `mqtt_client.c`、`lib/mqtt_outbox.c` 和 `runtime/emqtt.c`，运行于固定 ESP-IDF 的 Linux 目标；Broker 仅监听本机 `127.0.0.1` 临时端口。

## 输入与复跑

- 固定 ESP-IDF：`855937cf9dcee13ee9c423fb0319238cdc8d53fd`；其内 `esp-lwip`：`2758df4cd3666b3b2a5b53830148379326425c0d`。组件 CMake 会执行 `tools/sdk.py check`。
- 对照旧版：`0117bde3f5a482a75add29df31eea0520edd13f5`。新版为本修正后的同一隔离 checkout 工作树。
- 导出固定 SDK 的 `export.sh` 后，从本仓根运行以下命令。输出目录位于仓外，每个场景使用独立目录：

```bash
bash tests/linux-broker/run.sh "$PWD" /tmp/esp-mqtt-linux-broker-full full
bash tests/linux-broker/run.sh "$PWD" /tmp/esp-mqtt-linux-broker-unsub unsub-only
```

`full` 依次扣留动态 SUBACK 和 UNSUBACK 约 9 秒、断开 TCP，经历三次连接与两次自动重连；`unsub-only` 单独覆盖先确认动态订阅、再扣留 UNSUBACK 的两连接路径。两者均跨过 5 秒重传检查窗口，且混合未获 PUBACK 的 QoS1 PUBLISH。Broker 记录实际订阅集、报文 ID、DUP 位和 ACK；应用侧记录 READY、PUBACK 与错误。

## 结果

| 场景 | 旧版 `0117bde` | 修正后 |
| --- | --- | --- |
| 扣留 SUBACK 后断线 | 第二连接在正常订阅 `test/base` 后重放旧 `test/dynamic` SUBSCRIBE；Broker 集合被额外增加动态 Topic，运行层因非当前 ID 的 SUBACK 报订阅错误 | 第二连接只订阅 `test/base`；旧 SUBSCRIBE 未跨 clean session；第三连接完整订阅期望的 `test/base` 和 `test/dynamic` |
| 扣留 UNSUBACK 后断线 | 第二连接重放旧 UNSUBSCRIBE，Broker 的 `test/dynamic` 被删除，运行层报错 | 第二连接完整订阅 `test/base` 与 `test/dynamic`；旧 UNSUBSCRIBE 未重放 |
| QoS1 PUBLISH | 与旧控制报文相互干扰 | 两个场景中的未获确认 PUBLISH 均在重连后以 `DUP=1` 重发并收到 PUBACK |

修正后的 `full` 和 `unsub-only` 都输出 `APP TEST PASS` 与 `BROKER TEST PASS`。真实 `lib/mqtt_outbox.c` 的 IDF Linux host 测试另有 12 项、302 个断言通过，覆盖排队/已发送 SUB/UNSUB 删除及 PUBLISH/PUBREL 保留；`bash tests/host/run.sh` 的 ASan/UBSan 测试通过。固定 SDK 的 ESP32-C3 `tests/c3-smoke` 与 `examples/custom_outbox` 均完整编译并链接，后者启用 `CONFIG_MQTT_CUSTOM_OUTBOX=y`，证明示例实现了新增 outbox 接口。

上述结果证明本机 Linux 目标和隔离 Broker 的控制报文行为，以及 C3 镜像的编译链接。未刷写 ESP32-C3，也未验证实板、TLS 或生产网络；这些仍按独立实板矩阵验收。
