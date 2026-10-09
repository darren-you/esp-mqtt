# MQTT 入站消息体存活期检查点

2026-09-28。本轮前 Base 消费的 MQTT 源提交 `c0677e5e779c3e51e814f2920420be7ec54f1d88` 在 `emqtt_create` 时把三份完整 `emqtt_message_t` 放在运行实例里。固定 ESP32-C3 与 ESP32 编译器均确认单份结构为 `0x1110`（4,368）B；三份常驻共 **13,104 B**。本次仅把三份结构改成三枚指针，仍由原三个空槽 token 约束已完成且待 owner 取走的消息数。首个 DATA 片段到达才分配完整消息体；`poll` 将内容按原公开 API 复制给调用方后擦除释放，畸形片、通知队列满、断线、stop 和 destroy 也清退。三枚 32 位指针占 **12 B**，因此创建后没有入站消息时运行实例的这部分占用减少 **13,092 B**。这来自精确类型尺寸与同一实例字段的替换，不是设备总堆低水实测。

三条消息排队时允许第四条分片临时占有缓冲；若第四条完成前 owner 归还一个槽，就直接把临时缓冲转给该槽而不再复制完整消息体。若没有空槽、分配失败或通知丢失，仍按原合同关闭会话；完整事件交付、QoS1 DUP 和三条待处理消息上限未改变。活跃入站消息仍按实际数量消耗堆，满队列或 FRP、OTA 并发时不能按 13,092 B 固定抵扣。

本仓 `bash tests/host/run.sh` 的 ASan／UBSan 通过：覆盖空闲零消息体、100 次创建／入站／复制／释放、三条排队加第四条续片、首条与第四条 OOM、畸形片、断线、stop、通知溢出、超时及尾部清零。`python3 -m unittest discover -s tools/tests -p 'test_*.py'` 为 **8/8**。在 `mac-work-1:/private/tmp/esp-mqtt-lazy-message-20260928/` 的固定 SDK `578cf89c` 与本仓精确 lwIP 下，Linux 真实核心 `qos1-ack` 完成丢失／重复 PUBACK、两次入站 DUP、断线重连；`tls` 完成可信时间、正确 CA／主机名的 SUBACK／QoS1 PUBACK，并拒绝错误 CA 或主机名。真实核心短生命周期 **2/2**，C3 独立 smoke 编译与 `check_sizes.py` 通过；原始日志分别为 `qos1.log`、`tls.log`、`lifecycle.log`、`smoke.log`。所有输入均为本机／仓外软件试验，未刷板。

同一源码的真实核心完整生命周期随后完成 **100/100** 轮：200 次连接、300 次 SUBACK、100 次 QoS1 PUBACK、100 次 UNSUBACK；轮末 malloc zone 使用量首末均为 **100,480 B**，0–255 文件描述符首末均为 **3**。RSS 首末为 **2,244,608／2,916,352 B**，不能据此宣称物理堆无碎片。原始 `lifecycle-100.log` SHA-256 `ff03ea1cd1667269df4a4286b37641ee82737b6eb15a12ce4c4ab6ea7ad1f27f`；`qos1.log`／`tls.log`／`smoke.log` 的 SHA-256 分别为 `ba7e5bc5cba3bf07f0e8f2dc15862e6474d92b596be93bcfbc71327789f7465b`、`92c51cd77e87508bcd05f449b3a14eac05c8bf2775274a47d08f7d37c348ee54`、`3d5bb96cf8935d239b8c04e06bf61e3177944c758263f91251fa83c2c047be60`。

本收据只覆盖 MQTT 源仓；Base 的公开精确依赖锁、C3／ESP32 签名产品和原生业务组合资源数据需单独记账。前一轮 Base 的 **57,020 B** FRPS 诊断低水没有配置 MQTT，不能加上本次静态差额后当作新组合容量结论。
