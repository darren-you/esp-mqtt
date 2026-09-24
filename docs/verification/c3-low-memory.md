# ESP32-C3 MQTT 入站重组内存收敛

本记录只覆盖 `esp-mqtt` 运行层的入站消息重组。使用现有 ESP32-C3／4 MiB 目标；不改变 TLS 验证、QoS、三个待处理消息槽或 4 KiB 最大载荷，也不代表五组件并行、实板或生产 Broker 已验收。

## 变更与所有权

原运行实例同时拥有 `emqtt_receiver_t.message` 和三个 `emqtt_message_t` 事件槽。每条分片消息先写前者，完成后再复制到空闲槽。现在首片到达时从 `free_slots` 预留一个槽，后续片直接写该槽；完成后将槽号入通知队列，`emqtt_poll` 复制事件并归还槽。畸形片、连接重置和断线立即归还正在重组的槽；主动停止在 SDK 回调退出后重置全部槽。三条待处理消息仍可排队，第四条仍触发溢出并关闭会话。

`emqtt_receiver_t.message` 改为调用方提供的消息存储指针，重组期间不得复用。空指针直接拒绝。官方 MQTT 核心、outbox、QoS 1 ACK 和重发逻辑均未修改。

## C3 字节账本

使用同一 ESP32-C3 `riscv32-esp-elf-gcc` 15.2.0，分别从改动前后源码编译 `runtime/emqtt.c`，从 DWARF 读取 `struct emqtt_runtime` 大小；头文件类型从同一工具链生成的 `.rodata` 常量读取。它们是 `emqtt_create` 单次 `calloc(sizeof(*r))` 请求的字节数，不含分配器元数据、FreeRTOS 队列、官方 MQTT 核心、outbox、TLS 和任务栈，也不是运行峰值。

| 类型或分配 | 改动前 | 改动后 | 差值 |
| --- | ---: | ---: | ---: |
| `emqtt_receiver_t` | 4,376 B | 12 B | −4,364 B |
| `struct emqtt_runtime`／单次 `calloc` | 25,504 B | 21,144 B | **−4,360 B** |
| 三个 `emqtt_message_t` 槽 | 13,104 B | 13,104 B | 0 B |

运行实例另增加一个 4 B 的在途槽号，故实际结构节省 4,360 B。`emqtt_message_t` 保持每个 4,368 B，`emqtt_config_t` 保持 7,716 B。

## 本机验证

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| `bash tests/host/run.sh` | 通过，ASan／UBSan | 分片、三个并列待处理消息、断线与主动停止的在途槽归还、畸形片、第四条溢出、100 次生命周期 |
| 固定 SDK 的 `tests/c3-smoke/build.sh` | ESP32-C3 编译链接通过，镜像 `0x24d80` B | 空配置 smoke，不建立 Wi-Fi／MQTT／TLS 会话 |
| `tests/linux-broker/run.sh ... qos1-ack` | `BROKER TEST PASS` | 真实 MQTT 核心与本机 Broker：丢／重复 PUBACK、入站 DUP、断线重投；未使用 TLS 或 C3 实板 |

本项只减少 MQTT 运行实例的一次常驻申请。FRP、OTA、Wi-Fi、Wasm 与 TLS 并行时的可用堆和最大连续块仍须在实际组合中测量；不能把上述 4,360 B 当作完整五能力的容量证明。
