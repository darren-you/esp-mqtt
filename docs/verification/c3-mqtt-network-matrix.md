# C3 MQTT 实板网络矩阵

日期：2026-10-01。维护者提供当前可用的 2.4 GHz Wi-Fi，并允许丢弃 ESP 旧数据。本轮使用同一独立 MQTT 实验镜像完成 30 个实板场景组与 100 次实例销毁重建；ESP32 当前未连接，双板及五能力组合验收仍未完成。

## 冻结输入与写入

| 输入 | 本轮事实 |
| --- | --- |
| MQTT 源码 | `8a02b286b62cb015d02239be0a8e9fc5012a868c` 的 Git 归档 |
| 源码归档 SHA-256 | `2770d70250e5a0f18b7283e88d9bfea821139c0f0d1684a1be123606438ed7c1` |
| ESP-IDF | `578cf89c343e388db43ba1f4ddcd602fedcb763c`，独立 SDK 合同检查通过 |
| esp-lwIP | 锁定 SDK 的 `2758df4cd3666b3b2a5b53830148379326425c0d` |
| target / Flash | 实际核对的 `esp32c3` / 4 MiB；唯一 USB Serial/JTAG 端点 |
| 应用 | 956528 字节，SHA-256 `83da82cf3938edde73d284cedf9cff2a7c1b891e7c5436d37e9412919591cd46` |
| 实验应用槽 | `factory` 从 `0x10000` 起，大小 `0x100000` 字节；与正式 Base 分区不同 |

Wi-Fi、Broker 地址、身份、CA 和实验账户仅从仓外私有输入装配。写前再次核对设备与关闭的 Secure Boot / Flash Encryption。板上 bootloader 与此前已验证的 `d62f75daca92a419ea4aafc70151366cbdee3becb4b7c868636ceb0db93c1b99` 一致，分区表与本轮构建一致。新构建 bootloader 的 35 字节差异只涉及构建时间和末尾镜像摘要，因此本轮保留板上 bootloader，仅擦写实验应用槽；整个 1 MiB 应用槽回读与新应用及其后 `0xff` 字节逐项一致。没有写入 eFuse。

所有场景均运行这份应用。没有通过修改组件、SDK、固件入口或虚拟时间制造负例。普通场景使用隔离 Mosquitto 2.1.2；证书、账号、SUBACK 和 ACK 故障使用仓外 TLS 报文测试器，解析器来自本仓既有 `tests/linux-broker/run_broker_test.py`，仍由实际 C3 的 MQTT 核心及运行层处理协议。

## 设备与 Broker 共同验证的场景

| 场景组 | 证据与结果 |
| --- | --- |
| 正常网络与收发，10 组 | Wi-Fi、SNTP 可信时间、严格 TLS、CONNECTED / SUBACK / READY；QoS0/1 `ping` 和出站 4096 字节逐字节匹配；入站 QoS0/1 和 4096 字节匹配长度与 CRC32；动态订阅可交付，UNSUBACK 后不再交付；另含下节百次生命周期组 |
| 断线恢复与容量，8 组 | 新订阅收到带 retain 位的 `online`；停止 Wi-Fi 后收到真实 `offline` LWT，再次订阅证明其已 retained；离线填充接受 3 条、outbox 为 12477 字节，下一次返回 `OUTBOX_FULL=0x7501`；Wi-Fi 恢复后取得新 SUBACK / READY 和空 outbox；4097 字节入站被拒绝，后续分片均未成为完整 MESSAGE；销毁重建后恢复 READY |
| TLS / 认证 / 订阅拒绝，4 组 | 无关 CA 与正确 CA 下错误主机名均产生 `EMQTT_ERROR_TLS`、`-0x2700` 握手失败且零 MQTT CONNECT；账号与测试器认可的凭据不匹配，真实 CONNACK 5 映射为 AUTH；真实 SUBACK `0x80` 映射为 SUBSCRIPTION；结束后恢复有效 Broker 和 READY |
| QoS 与压力，7 组 | 扣留 PUBACK 后约 6.061 秒收到同 ID / 同载荷 / DUP=1 重传；跨连接重投仍用原 ID / DUP=1；两次发送相同 PUBACK 后设备只报告一次完成，之后至少 6 秒无再次重传；入站同 ID 的 DUP=0/1 消息均核对 CRC，Broker 收到两次原 ID 的 PUBACK；32 条 4096 字节入站突发产生 QUEUE 拒绝，随后重建恢复；离线 3 条消息约 30.653 秒后各自收到一次 DELETED、outbox 清零，恢复 Wi-Fi 后重新 READY |
| 实际 Broker 进程重启，1 组 | 终止并重建已核对归属的 Mosquitto 进程；设备在未发送 `cycle`、未复位的同一实例中完成 DISCONNECTED、新 SUBACK / READY、在线状态 PUBACK 和空 outbox；设备 cycle 不变、单调运行时间增加 |

锁定 SDK 的证书拒绝会暴露 `tls_flags=0`，与[既有 Linux TLS 记录](mqtt-tls-linux.md)一致。拒绝判断同时依赖冻结证书、OpenSSL 对正确链／错误链或主机名的对照验证、设备握手日志和 Broker 零 CONNECT，不能用非零 `tls_flags` 作为唯一判据。

主动断网可能先报告 TRANSPORT 或无证书验证位的 TLS 传输错误，再报告 DISCONNECTED。断线测试只在明确关闭 Wi-Fi 的区间接受这些预期错误；恢复后仍要求新的订阅证明、READY 和空 outbox。超限消息被 SDK 分片时可产生多次 FRAGMENT 回执，测试核对每片属于同一超限消息，并确认没有完整 4097 字节交付。

## 百次同状态资源采样

串口驱动沿用本仓 `examples/broker-client/serial_cycles.py`。cycle 序号 1–100 连续；每轮销毁并重建实例，等待本轮 READY、在线状态原消息 ID 的 PUBACK，再要求空 outbox。单次启动或末轮结果不代替逐轮证明。

| 采样值（字节） | 首轮 | 末轮 | 100 轮范围 |
| --- | ---: | ---: | ---: |
| 当前空闲 heap | 157720 | 157736 | 157636–157768 |
| 历史最低 heap | 145024 | 145024 | 145024 |
| 当前最大连续块 | 114688 | 114688 | 114688 |
| outbox | 0 | 0 | 0 |

本轮没有观察到同状态下持续资源下降或 panic。历史最低 heap 不是逐轮占用；这些结果只涵盖独立 MQTT 镜像，不能证明 Base / FRP / OTA / Container / MQTT 同存峰值、正式双固件和三包槽容量。

## 私有证据与后续边界

固件、私有输入、证书私钥、设备身份、完整串口／Broker 日志和 JSON / CSV 结果均保存在仓外受控目录。17 项证据清单的 SHA-256 为 `3ba7c3207d0817c54f314fa094dea8df390cc75f2950aeba567ef288dc4e503d`；逐场景结果分别在 normal、recovery、TLS failure、QoS fault 与实际 Broker restart 结果中，清单固定其字节摘要。公开仓只保留本页的无敏感摘要。

结束时取得本 boot 的 `wifi_down error=0`，终止本轮已核对的 Broker；实际进程、TCP LISTEN 与串口 owner 均已消失。端口 bind 可能因尚存 TCP 状态失败，清理判断采用进程与实际 LISTEN 核查。C3 保留本轮实验镜像；实验 Wi-Fi 停止回执不定义产品停止后的跨重启策略。

本页推进 P3-06 / P3-07 的 C3 切片。ESP32 同候选实板矩阵、五能力组合动态资源、正式分区／签名产品安装、物理断电和 72 小时验收仍缺证据，不据此将五仓计划或双板阶段标记为完成。
