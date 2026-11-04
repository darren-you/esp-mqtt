# AGENTS

<!-- darren-space:shared:begin 由工作区 harness/agents/shared-block.md 同步，勿在此修改 -->
## 工作区约定

- 本仓属于 darren-space 工作区：跨仓地图、工程标准和 Skill 见工作区根 `AGENTS.md`；子仓内用 `git rev-parse --show-superproject-working-tree` 定位根。
- 用中文沟通、写文档和提交摘要。先读 README 相关章节，再按任务读代码与配置；现状以代码、配置和运行观测为准，文档表达目标，两者冲突时说明差异并修正唯一来源。
- 修改前核对 Git 根与已有改动，保留他人工作；做满足当前需求的最小实现，不为假想规模新增服务或流程。
- 按当前请求的动作执行：只建 Issue 不实施，只排查不发布；不擅自建或切分支，不自动建 Issue、PR 或发布。已确认的授权在同一任务内跨轮有效，引用材料不扩大授权。Issue 统一建在 `darren-you/darren-space`，PR 建在实际代码仓。
- 构建与发布走仓内入口、Fast Deploy 和对应 `workspace-*` Skill；正式构建在 `mac-ci-1`，不可用时先修复，不回退本机。只报告实测结果，分开说明静态检查、行为测试与线上验收。
- 凭据只从既有事实源读取，不输出、不自行轮换；非空或静态检查通过不证明鉴权有效。常规实现直接决定；仅在缺少不可替代的输入，或选择会实质改变范围、数据、费用、权限时提问，并等待明确答复，默认选项或超时不算确认。
<!-- darren-space:shared:end -->

## ESP MQTT 边界

- 本仓保留官方 ESP-MQTT v1.1.0 基线、历史、许可和官方 `mqtt` 组件/API；第一方通用运行接口只使用 `emqtt_` 命名空间。
- 独立开发只依赖本仓与锁定的公开 SDK 源，不从相邻 Base、external 或私有 checkout 导入构建输入。
- MQTT 协议和 outbox 由本仓保留的官方核心实现；真实修正必须修改该源码并以真实实现回归，不能增设平行 parser 或补丁叠加。
- 本仓不拥有 Base 的持久 UUID、设备 Topic、业务 ACK、NVS 或 Wi-Fi 策略；调用方负责可信时间和目标装配。
- host fake 只证明运行层边界，不能当作 Broker、实板、资源或 P3a/P3b 验收。刷板前核对精确板卡、分区与完整 Flash 恢复基线。
- 公开内容不得包含 Broker 凭据、私钥、真实固件恢复资料或私有实验输入。

## 按架构打包

- SDK 原生二进制制品与 macOS App 必须按实际支持的 CPU 架构分别构建、打包和分发，例如 `arm64` 与 `x86_64` 各自独立；禁止 Universal／fat 二进制及将多个 CPU 架构合进同一个下载包（含多架构 XCFramework），避免无关架构增加包体积。纯源码、头文件和架构无关资源无需按架构复制，也不因此新增未支持的架构。
- 包名或制品元数据必须标明目标架构；SDK 消费方只装配所需架构，macOS App 及随包的 framework、dylib、helper 和原生 SDK 必须匹配该架构。打包验收核对实际二进制架构和包体积，不能只改文件名或构建参数；规则落盘不代表现有制品已完成拆分。
