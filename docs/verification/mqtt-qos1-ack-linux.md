# MQTT QoS1 ACK 与重投回归

记录日期：2026-09-24。测试从 `esp-mqtt@113bdef20d862a1bf094fd3cdd833f641ab7aa64` 起，在固定 ESP-IDF Linux 目标直接编译本仓真实 `mqtt_client.c`、outbox 和 `emqtt_` 运行层。隔离 Broker 只监听本机 `127.0.0.1` 临时端口，不读取生产凭据。

## 复跑

使用 `sdk-lock.json` 固定的 ESP-IDF `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 和内含的 esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d`。导出该 SDK 的 `export.sh` 后，在本仓根运行：

```bash
bash tests/linux-broker/run.sh "$PWD" /tmp/esp-mqtt-linux-broker-qos1 qos1-ack
```

`run.sh` 把测试应用复制到仓外输出目录，并通过组件 CMake 执行 SDK/lwIP 锁检查。原 `full` 与 `unsub-only` 仍使用原应用和 Broker 场景。

## 实际报文与事件

| 输入 | Broker 观察 | `emqtt_` 观察 |
| --- | --- | --- |
| 同一连接暂扣出站 QoS1 的 PUBACK，跨过 5 秒重传窗口 | 原 ID、原载荷先以 `DUP=0`，后以 `DUP=1` 到达；这次实测约 10 秒后重发 | 收到 PUBACK 后只产生一次 `EMQTT_EVENT_PUBACK` |
| Broker 连续发送相同 ID 的两份 PUBACK | 两份 ACK 已发送 | 第二份不产生第二个完成事件；outbox 清空 |
| Broker 发送 QoS1 入站报文，收到 PUBACK 后按同一 ID/载荷以 `DUP=1` 重投 | 两次分别收到对应 PUBACK | 两次 `EMQTT_EVENT_MESSAGE`，`message.duplicate` 依次为 false、true；运行层不擅自去重 |
| Broker 收到第二条出站 QoS1 后断开 TCP，不回 PUBACK | 新连接仍为 clean session；同一 ID/载荷以 `DUP=1` 重发，随后 Broker 连续发送两份 PUBACK | 断线与重新 READY 可见；该 ID 仅一次完成事件，最终 outbox 为 0 |

应用输出 `TEST PASS ... incoming=2 pubacks=2`，Broker 输出 `BROKER TEST PASS scenario=qos1-ack`。此新场景在上述原版 `113bdef` 上通过，未发现需要修改协议核心的缺陷；本次只增加真实路径覆盖及 API 语义说明。入站 QoS1 按协议可能重复交付，业务处理必须按自身消息身份幂等；PUBACK 只是 Broker 对网络报文的确认，不表示设备命令或远端业务已执行。

此测试不验证 TLS、生产 Broker ACL、ESP32-C3 实板或电源中断后的持久 outbox。RAM outbox 在进程重启后不保留；实板矩阵与百次资源回收仍须单独验收。
