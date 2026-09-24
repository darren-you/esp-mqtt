# MQTT 生命周期与 transport OOM 回归

记录日期：2026-09-24。本次从 C3 入站缓冲的失败释放审阅发现三个官方核心缺陷，修正在 `mqtt_client.c` 与 `lib/include/mqtt_client_priv.h`，同时应用到 `master` 和 `codex/c3-low-memory` 工作树。未提交、未推送、未刷设备；本记录的通过结论绑定下列基线加源码摘要，不代表这些基线提交本身已包含修正。

## 输入与候选

| 输入 | 精确值 |
| --- | --- |
| C3 分支基线 | `ccf81df2215cfddd87aff97afdd2e7f17e50fbaa` |
| master 基线 | `5bff093646d8db810d64c50c39edc004e78bf40c`；保留本轮开始前已有元数据和来源说明修改 |
| SDK | `esp-space/esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c` |
| lwIP | `2758df4cd3666b3b2a5b53830148379326425c0d` |
| 两分支 `mqtt_client.c` SHA-256 | `e92157b9562cced85b7c542e596971401c2b42f54badb4b84f9f205b69a0967e` |
| 两分支 `lib/include/mqtt_client_priv.h` SHA-256 | `34ebd66c4cc707234706a40294d4818a84c9f09fdadaed57a778a5794e69e4e4` |
| `tests/linux-lifecycle/main/main.c` SHA-256 | `c73574d4df2e83e5cf9b51faba088febbd8aa17a6e17728515776b52e5a833de` |

两个分支的原始核心文件逐字相同；C3 专有 `emqtt_` 重组缓冲未复制到主线。SDK 通过仓内工具和组件 CMake 守卫核对，没有向 SDK 或消费者注入生产补丁。

## 缺陷与修正

| 真实路径 | 旧实现 | 本次修正 |
| --- | --- | --- |
| 高优先级 owner 在 `start` 返回后立即 `stop` | `run` 由尚未调度的 worker 设置，`stop` 返回 `ESP_FAIL`；立即销毁也可能与随后启动的任务冲突 | 创建任务前持有 API 锁设置运行标志、初始状态并清除旧 STOPPED 位；worker 不再覆盖 owner 已发出的停止请求 |
| TCP/TLS transport 初始化 OOM | worker 自行设置 `run=false` 后退出，没有错误事件；运行层仍持有 started 实例，后续 `stop/destroy` 失败 | 发送带原始错误码的 transport 错误事件；停止依据已创建的任务句柄等待真实退出，成功后清除句柄；destroy 不忽略停止失败 |
| `esp_transport_list_add` 注册 OOM | 返回值被忽略，尚未交给列表的 TCP/TLS 句柄泄漏 | 注册成功才转移所有权，失败立即销毁该句柄并返回错误；相同所有权规则覆盖 WS/WSS 包装层 |

`run` 改为原子布尔量，使 worker 循环与 owner 停止请求之间没有普通布尔量的并发读写。API 锁保护单个 `stopping` 标志；正在退出时第二个 `stop` 仍返回失败，不能在第一次等待结束后清空新任务的句柄。该保护的定向回归在未加标志的中间候选确定失败，最终候选通过。公开状态枚举和 API 未扩展。

固定 SDK 的 `transport_ws.c` 中，WS 销毁只释放包装层自身的缓冲和配置，不销毁 parent transport；所以 WS/WSS 注册失败的清理不会重复销毁已入列表的 TCP/TLS。网络正常断线的自动重连、QoS、outbox 与订阅证明逻辑保持原路径。

## 复现与验证

复跑入口见 [Linux 生命周期回归](../../tests/linux-lifecycle/README.md)。测试直接编译真实核心、运行层、FreeRTOS 任务和队列、TCP/TLS transport。只在测试编译中替换 MQTT 对 SDK 的初始化、注册和释放调用点，分别注入 `NULL` 和 `ESP_ERR_NO_MEM`；TLS 场景在握手前失败，没有测试证书信任或网络 TLS。

旧 `ccf81df` 的仓外 `git archive` 在立即停止场景报告 `TEST FAIL stop`，四种 TCP/TLS OOM 场景均报告 `TEST FAIL missing_start_error`；原始首次复现还确认 OOM 后 `stop` 失败，注册场景的未入列表句柄没有释放。最终候选结果：

| 验证 | C3 分支 | master |
| --- | --- | --- |
| 立即停止、TCP/TLS 初始化 OOM、TCP/TLS 注册 OOM | 五项各 100 次 create/destroy、200 次 start/stop，通过 | 同样通过 |
| 同一任务退出期间两个 caller 调用官方 stop | 100 次，通过；第二个 caller 被拒绝 | 同样通过 |
| TCP 与 TLS 注册 OOM 的未入列表句柄 | 每项 100 次注入、100 次释放 | 同样通过 |
| `tests/host/run.sh` | ASan/UBSan 通过，含 C3 三槽/第四分片、队列满、断线、停止和动态 OOM | ASan/UBSan 通过，保持主线常驻重组缓冲 |
| 真实核心 Linux 隔离 Broker | `full`、`unsub-only`、`qos1-ack` 通过 | `qos1-ack` 通过 |
| 固定 SDK ESP32-C3 smoke | 150,912 B；SHA-256 `1dee0ccbb01cbd34d95695eaa213e659b7c9d37309462e714accb3724d50ef92` | 150,912 B；SHA-256 `da6fe4e288aad8a8804d40fbacb033fc10ef5588cdf7b4cb91358431fe6ac679` |

Broker 回归覆盖 clean session 旧 SUBSCRIBE/UNSUBSCRIBE 不重放、正常订阅恢复、丢失/重复 PUBACK、入站 QoS1 DUP 和断线重发；没有新增第二套协议测试实现。

## C3 峰值与剩余边界

用相同 C3 GCC 15.2.0 编译对象的 DWARF 核对，核心 `struct esp_mqtt_client` 修改前后均为 248 B，新增布尔量使用现有对齐空隙；C3 `struct emqtt_runtime` 仍为 21,144 B。本次没有新增堆申请。

原 C3 账本继续成立：相对主线运行实例常态少申请 4,360 B；三个 4,368 B 消息槽都满、第四条在途时另申请 4,368 B，运行层请求总量为 **25,512 B**，比主线 25,504 B 多 8 B，另有分配器元数据。此次修复没有降低这一瞬时峰值，也不改变三槽满、OOM、通知丢失时的 fail-closed。

生命周期计数仅证明本测试覆盖的任务和 transport 所有权，不是全堆泄漏或实板百次资源验收。C3 实际 Wi-Fi、TLS、Broker、设备命令 ACK、五组件并发峰值及真实设备回收仍未验证；不据此勾选 P3-07、P3-08 或完整 C3 支持。
