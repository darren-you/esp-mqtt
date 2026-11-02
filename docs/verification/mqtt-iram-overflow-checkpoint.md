# ESP32 IRAM 溢出标志修正

2026-11-02 在实际单核 ESP32 上复现运行层的队列错误被吞掉。配置启用 `CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY`，运行对象按原策略放在 IRAM；`atomic_exchange(&overflow, false)` 清掉 true 后返回 false，导致 owner 未按合同关闭会话。此问题会使已经丢失入站消息的会话继续报告 ready。

## 对照证据

实验使用同一个固定容量统计 SDK、Xtensa GCC 15.2.0 工具链和 pure RAM app，经 ROM 加载；完整 Flash 恢复快照已读取并与芯片 MD5 核对。没有 Flash／NVS 写入。原子标志与生产对象相同，后接 started／faulted 字节；普通数据内存为对照。

| 对照 | 实际结果 |
| --- | --- |
| 普通数据内存中的 atomic bool，8 次 true→false | 8 次均返回 true，邻接字节正确 |
| IRAM 中的相同 atomic bool，8 次 true→false | 8 次均错误返回 false，标志被清除，邻接字节正确 |
| 原运行层，四轮三槽满后送入第四条完整消息 | 4 次仍返回 MESSAGE／READY，未停止 client |
| 修正运行层，相同 fixture 和逐字一致 sdkconfig | 4 次均停止 client、报告 QUEUE／FAILED |
| 仓内最终 RAM 回归工程 | 4 次均通过，并核对运行对象实际位于 IRAM |

原运行层 `emqtt.c` SHA-256 为 `a6273eeef965a6a2079d268d78647e2ad7ba93b7cc839faec64443ddf5530333`；修正后为 `8b533cfcea4bd8355234c114863c6d010408ab2115622ea6d4731c23605bf71c`。原控制镜像为 `674a8924053fab99f4ee6964e6707b4912a505bd3424e49191b590abb61305d3`，相同 fixture 修正镜像为 `6903dd10c33fdb8d3101bc4e2544b3e9d809c950bec844f55c94efe318e57958`，仓内最终回归镜像为 `1288549fdd6cb7ca9863c6f45aec8dc4e03e634481ae1f8081a26c7d3ee9b284`。这些是隔离测试镜像，不是生产签名制品。

两份对照 ELF 中的运行对象均为 2504 B。修正复用已有资源临界区，在锁内设置标志或读取并清零；队列操作、SDK stop、分配和释放仍在锁外。IRAM 路径只用于单核 ESP32，SDK 临界区通过屏蔽调度保护这段访问。三槽、第四条 partial、载荷上限、公开接口与动态所有权保持原合同。

## 验证边界

`tests/host/run.sh` 的 ASan/UBSan 合同及运行层测试通过。固定 SDK 的真实核心 Linux `full` 和 `qos1-ack` 场景通过，覆盖 clean session 重连、QoS1 DUP、丢失／重复 PUBACK 与入站重投。局部治理三项通过属于静态检查。

RAM 回归使用真实 CPU、IRAM 分配和 FreeRTOS，但 client API／事件由 fake 注入，不运行 Broker、TLS、Base 业务或 OTA。原生产应用已在实验后恢复，独立固件／配置／原收据回读由私有现场证据记录。正式 Base 仍须精确升级组件 pin、重新构建生产候选并进行联合负载验收；本批不授 R5、百次或连续72小时资格。稳定复现入口见[测试说明](../../tests/iram-overflow/README.md)，动态进展见 [#35](https://github.com/darren-you/darren-space/issues/35)。
