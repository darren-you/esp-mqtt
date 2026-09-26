# MQTT 真实核心 Linux TLS 回归

日期：2026-09-26。起点为 `esp-mqtt@9d6d95e779f4f5ff387a6d9b54015bf4e43565f2`。本轮只修改独立测试入口与说明，没有修改运行层或官方核心，没有使用两台 ESP 实板。

## 测试边界

`tests/linux-broker` 的 `tls` 场景使用本仓实际 `mqtt_client.c`、`runtime/emqtt.c` 和固定 ESP-IDF 的 Mbed TLS/transport。Linux 测试程序先要求 `emqtt_start` 在未给可信时间时返回 `ESP_ERR_EMQTT_TIME_REQUIRED`，再允许启动。Python Broker 只监听本机 IPv4/IPv6 loopback；运行时在仓外、仅当前用户可读的临时目录生成一次性 CA 与带 `127.0.0.1` IP SAN 的服务端证书，结束后删除。TLS 构建配置启用 `CONFIG_MQTT_TRANSPORT_SSL`，关闭 `CONFIG_EMQTT_PLAINTEXT_LAB`。每个场景使用独立客户端进程和监听端口。

| 场景 | 真实核心与 Broker 共同核对的事实 |
| --- | --- |
| 正确 CA 与 `127.0.0.1` | MQTT 3.1.1 clean session CONNECT，预设 filter 的 SUBSCRIBE/SUBACK，原 Topic/载荷的 QoS1 PUBLISH/PUBACK；客户端收到 READY 与原消息 ID 的 PUBACK 后销毁 |
| 无关 CA | 客户端收到 `EMQTT_ERROR_TLS`，没有 READY；Broker 没有收到 MQTT CONNECT |
| 正确 CA、主机名 `localhost` | 服务端证书只含 `127.0.0.1` IP SAN；客户端收到 `EMQTT_ERROR_TLS`，没有 READY；Broker 没有收到 MQTT CONNECT |

Linux 回归仍是软件证明：它没有模拟 ESP Wi-Fi、SNTP 的时间来源、真实 Broker ACL、设备堆和两板长期回收。两台真实设备的 P3-07 TLS/Broker 矩阵与百次资源回收仍待分别执行。

## 执行结果

在 `mac-work-1` 使用公开锁定 `esp-space/esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c` 与 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d`，把本仓独立源码副本放在仓外后导出 SDK，运行：

```bash
bash tests/linux-broker/run.sh "$PWD" /private/tmp/esp-mqtt-broker-offline-tls-final-20260926 tls
```

三场景依次通过。正确身份的 Broker 报文顺序为 `connect → subscribe → publish`；两种拒绝场景均为零 MQTT 报文。两种拒绝时 Mbed TLS 握手返回 `-0x2700`，运行层事件为 `EMQTT_ERROR_TLS`，但该锁定 SDK 通过 ESP-MQTT 暴露的 `tls_flags` 为 0；不能把非零 `tls_flags` 当作拒绝是否生效的唯一判据。本轮不修改 SDK 的错误结构，P3-07 实板记录须保留 TLS 日志、设备事件和 Broker 双侧证据。

本机 `bash tests/host/run.sh` 的 ASan/UBSan 合同/运行层回归通过；`python3 -m unittest discover -s tools/tests -p 'test_*.py'` 为 8/8；`python3 -m unittest discover -s examples/broker-client -p 'test_serial_cycles.py'` 为 3/3。上述 host/fake 与串口伪端口结果均不作为真实 TLS 或实板验收。
