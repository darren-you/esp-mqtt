## ESP MQTT 边界

- 本仓保留官方 ESP-MQTT v1.1.0 基线、历史、许可和官方 `mqtt` 组件/API；第一方通用运行接口只使用 `emqtt_` 命名空间。
- 独立开发只依赖本仓与锁定的公开 SDK 源，不从相邻 Base、external 或私有 checkout 导入构建输入。
- MQTT 协议和 outbox 由本仓保留的官方核心实现；真实修正必须修改该源码并以真实实现回归，不能增设平行 parser 或补丁叠加。
- 本仓不拥有 Base 的持久 UUID、设备 Topic、业务 ACK、NVS 或 Wi-Fi 策略；调用方负责可信时间和目标装配。
- host fake 只证明运行层边界，不能当作 Broker、实板、资源或 P3a/P3b 验收。刷板前核对精确板卡、分区与完整 Flash 恢复基线。
- 公开内容不得包含 Broker 凭据、私钥、真实固件恢复资料或私有实验输入。
