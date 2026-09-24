# ESP32-C3 MQTT 入站重组内存收敛

本记录只覆盖 `esp-mqtt` 运行层的入站消息重组。使用现有 ESP32-C3／4 MiB 目标；不改变 TLS 验证、QoS、三个待处理消息槽或 4 KiB 最大载荷，也不代表五组件并行、实板或生产 Broker 已验收。

## 变更与所有权

原运行实例同时拥有一个常驻重组消息和三个 `emqtt_message_t` 事件槽。每条分片消息先写常驻缓冲，完成后再复制到空闲槽。现在首片优先预留空闲槽并直接写入；三槽均被已完成消息占用时，最多临时分配一个 `emqtt_message_t` 给第四条在途消息。即使 owner 在第四条续片前消费并释放一个槽，第四条仍按原顺序完成和交付。临时缓冲从首片持有到完成或拒绝；完成时必须有空闲槽承接，否则按原有边界触发溢出并关闭会话。

畸形片、连接重置和断线释放正在重组的临时缓冲；主动停止在 SDK 回调退出后清理在途缓冲。消息完成时先复制到释放出的固定槽，再释放临时缓冲；每次释放前按运行层现有的 volatile 字节写法擦除消息内容。通知队列发送失败时归还该槽。动态申请失败时运行层标记溢出，owner 下一次 `emqtt_poll` 停止会话并返回 `EMQTT_ERROR_QUEUE`。官方 MQTT 核心可能在回调前已向 Broker 发送 QoS 1 PUBACK，因此 OOM／溢出不能声称端到端交付可靠；C3 实板仍需测量这一峰值与实际 Broker 行为。

`emqtt_receiver_t.message` 改为调用方提供的消息存储指针，重组期间不得复用。空指针直接拒绝。这次重组优化未修改官方 MQTT 核心、outbox、QoS 1 ACK 和重发逻辑；后续真实核心生命周期修正单独记录在 [transport OOM 回归](mqtt-lifecycle-transport-oom.md)。

## C3 字节账本

使用同一 ESP32-C3 `riscv32-esp-elf-gcc` 15.2.0，分别从改动前后源码编译 `runtime/emqtt.c`，从 DWARF 读取 `struct emqtt_runtime` 大小；头文件类型从同一工具链生成的 `.rodata` 常量读取。运行实例是 `emqtt_create` 的单次申请，临时消息仅在三槽满的在途期间申请。以下不含分配器元数据、FreeRTOS 队列、官方 MQTT 核心、outbox、TLS 和任务栈。

| 类型或分配 | 改动前 | 改动后 | 差值 |
| --- | ---: | ---: | ---: |
| `emqtt_receiver_t` | 4,376 B | 12 B | −4,364 B |
| `struct emqtt_runtime`／单次 `calloc` | 25,504 B | 21,144 B | **−4,360 B** |
| 三个 `emqtt_message_t` 槽 | 13,104 B | 13,104 B | 0 B |
| 三槽满时第四条临时消息 | 0 B | 最多 4,368 B | +4,368 B 峰值 |

运行实例增加的在途槽号落在原有对齐空隙内，故常态节省 4,360 B。三槽满且第四条仍在重组时，多申请 4,368 B；与旧实现相比，此时申请字节数增加 8 B，另有分配器元数据。`emqtt_message_t` 每个仍为 4,368 B，`emqtt_config_t` 仍为 7,716 B；通知队列元素仍为 48 B。FreeRTOS 队列头、分配器元数据和碎片未计入。

## 本机验证

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| `bash tests/host/run.sh` | 通过，ASan／UBSan | 三队列→第四首片→owner 消费→续片、断线／停止／畸形片／通知队列失败清理、动态 OOM、第四条完成仍无槽时溢出、100 次生命周期 |
| 固定 SDK 的 `tests/c3-smoke/build.sh` | ESP32-C3 编译链接通过，镜像 `0x24d80` B | 空配置 smoke，不建立 Wi-Fi／MQTT／TLS 会话 |
| `tests/linux-broker/run.sh ... qos1-ack` | `BROKER TEST PASS` | 真实 MQTT 核心与本机 Broker：丢／重复 PUBACK、入站 DUP、断线重投；未使用 TLS 或 C3 实板 |

本项只减少 MQTT 常态内存申请，三槽满时第四条在途期间没有净节省。FRP、OTA、Wi-Fi、Wasm 与 TLS 并行时的可用堆和最大连续块仍须在实际组合中测量；不能把上述常态节省当作完整五能力的容量证明。

2026-09-24 的[生命周期与峰值复核](mqtt-lifecycle-transport-oom.md)确认运行实例仍为 21,144 B、官方核心句柄仍为 248 B；修复没有新增堆申请。三槽满时第四条在途的运行层申请峰值仍为 25,512 B，不能计作净节省。
