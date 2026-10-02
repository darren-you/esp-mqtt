# MQTT 常驻配置所有权检查点

2026-10-02，本次修改以 ESP MQTT `0617f7edeb31d9d9fd409072ea62ac125c4fb762` 的运行源码为输入；该输入与 Base 既有 `50c9c45f0fe95d4e99ab39584ff04d45d432efbc` 的 runtime 逐字节相同。本记录覆盖运行层常驻配置所有权收缩的软件与双目标 SDK 验证；Base 新版本消费和实体板组合结果须另行核验。

## 实际修改与不变量

实际 SDK 的 `esp_mqtt_set_config` 复制 hostname、client ID、用户名、密码及遗嘱主题／消息；`cacert_buf` 借用调用方的 certificate 指针，并在后续建连继续使用。运行层因此保留独立按实际长度申请的 CA，以及 TLS 时间策略和八槽期望订阅列表，删除完整 `emqtt_config_t` 的重复常驻副本。

CA 始终使用既有目标内存域，申请 `strlen+1`，最大 4097 字节。SDK client 销毁成功后才清零并释放；销毁失败保留原 owner 和证书，可重试。创建时元数据／CA OOM、队列或 SDK 创建／注册失败清理所有已拥有的存储。公开 `emqtt_config_t`、函数签名和订阅／入站／出站／队列／outbox 限额保持，调用方仍可在 create 返回后清零或复用输入。

官方 C3 编译器确认 runtime 从 8056 降到 2416 字节，另持有实际长度 CA；公开 config 输入仍为 7716，event／message 仍为 4388／4368 字节。该数值不是完整 MQTT、TLS 或五能力预算。尺寸探针仅在独立对象中禁用 LTO 以导出常量，目标 ABI 参数保持，正式构建优化未改。

## 已验证范围

- ASan／UBSan：原合同、事件、订阅和队列回归；新增两类存储 OOM、释放前全部清零、最大 CA 的输入复用、SDK 销毁失败后的 CA 存活和再次销毁。
- SDK 工具八项测试通过，固定 IDF／lwIP 检查通过。
- 固定 SDK Linux 真实核心六类生命周期各 100 次，立即停止、TCP／TLS 初始化与注册失败、同实例再次停止及双 caller 竞争通过。
- 原严格 TLS 测试通过；百次真实 Broker 生命周期各两次连接，初始／动态订阅、QoS1 PUBACK、断线重连及 UNSUBACK 均由 Broker 核对。
- C3 与 ESP32 的 SDK 编译和链接通过；ESP32 启用单核及既有 8BIT IRAM 策略。这是编译检验，没有刷写两板，也不替代 Base 的签名完整消费链。

完整仓外软件证据 58 份索引 SHA-256 为 `0eed83ab7fd421ca5afa09b6ccd8265364431863d7d00db7f9dff29328a390ed`，其中双目标镜像、ELF、map、配置、编译输入与日志共 22 份均核对摘要。前序首批 25 份索引 `0779e9d231c1c05d58eb6e3d520dac5f12ceb136bd5d3c0753debff070198a3d` 保持；证据含私有测试输入，不进入公开仓。

## 尚未完成

消费者需固定本次 canonical 完整 SHA，由官方 Component Manager 生成 Base 的真实目标锁，再完成两目标签名／官方验签及 C3 联合复测。局部尺寸、独立编译和 Linux Broker 通过不能证明实板容量或完整五能力通过。ESP32 未连接；满队列／outbox、FRP 满长记录和合法重叠峰值、掉电、72 小时及生产仍开放。
