# P3-07 ESP32 独立 Broker 样例写前检查点

记录日期：2026-09-27。对象是 mac-pro-1 上的 ESP32-D0WD-V3 旧 ESP-AT 设备和公开 esp-mqtt 的独立 Broker 样例。本记录只发布离线布局、软件构建与写前条件；同板原始 Flash、设备身份和实验凭据不进入公开仓。本文所称“当前槽”均指历史恢复件反映的状态，写入当天必须重新读回确认。

## 固定源码、SDK 与空输入镜像

本轮用公开 esp-mqtt@7308cc74811df36283af8e3038e5d6b251e084d3 的干净源码构建。它相对 9cfff83610378b12a017c34a3d447e5fb7403390 只增加 C3 检查点文档，样例与 MQTT 源码、sdk-lock.json 均未变化。仓外固定 SDK 经 tools/sdk.py check 核对为 ESP-IDF 578cf89c343e388db43ba1f4ddcd602fedcb763c、esp-lwip 2758df4cd3666b3b2a5b53830148379326425c0d。隔离工具链为 Xtensa GCC 15.2.0（esp-15.2.0_20251204）、CMake 4.0.3 和 Ninja 1.12.1。

不设置 EMQTT_SAMPLE_INPUTS，以仓内空示例运行 examples/broker-client/build.sh，target 为 esp32，完整编译链接成功。生成的 esp_mqtt_broker_client.bin 为 900,176 字节（0xdbc50），SHA-256 为 95b0fcef9a0c9652a0f0fc26cc860ba757f0f11f3ca7350e33f6a6199663c270；镜像头为 ESP32、4 MiB、DIO、40 MHz，追加哈希有效，app version 为 7308cc7。最终配置使用 UART0/115200，启用 MQTT 删除消息和 TLS 时间校验，关闭明文实验开关及 PHY 校准 NVS 存储。样例源码将 Wi-Fi 配置留在 RAM，不初始化或擦除旧 NVS。空输入镜像会在联网前拒绝，不能作为 TLS/Broker 实板证据。

## 历史同板 Flash 与样例默认布局

2026-09-26 留存的同板两份 4 MiB 完整 Flash 恢复件逐字节一致。离线解析的旧分区表如下：

| 分区 | 类型 | 偏移 | 长度 |
| --- | --- | ---: | ---: |
| phy_init | data/phy | 0xf000 | 0x1000 |
| otadata | data/ota | 0x10000 | 0x2000 |
| nvs | data/nvs | 0x12000 | 0xe000 |
| at_customize | 64/0 | 0x20000 | 0xe0000 |
| ota_0 | app/ota_0 | 0x100000 | 0x180000 |
| ota_1 | app/ota_1 | 0x280000 | 0x180000 |

旧 bootloader 位于 0x1000，分区表位于 0x8000。历史 otadata 两份选择记录均处于擦除状态，分区表没有 factory app；ota_0 是可识别且哈希有效的旧 ESP-AT app，ota_1 全部擦除。因此历史镜像中只有 ota_0 已证明存在有效 app，不能把 ota_1 当作设备内回退。旧 ota_0 镜像头标 2 MiB、DIO、40 MHz，尽管整片 Flash 和旧 bootloader 头标为 4 MiB；旧 bootloader 的校验和有效，但当前 esptool 对其追加哈希报告无效。这些离线头信息不代替当次启动、eFuse 与 bootloader 行为核对。

样例默认使用单 factory app 分区表：nvs 0x9000/0x6000、phy_init 0xf000/0x1000、factory 0x10000/0x100000。实际 flash_args 写 bootloader 0x1000、分区表 0x8000、样例 app 0x10000。默认整机 flash 不符合“不迁分区”：它会替换旧 bootloader/分区表，并从 0x10000 开始覆盖旧 otadata、NVS 和 at_customize 的部分内容。不得用默认 idf.py flash 或其生成的整机 flash_args 操作这台旧板。

样例 app 的 900,176 字节小于每个旧 OTA 槽的 1,572,864 字节。image-info 给出的 DROM/IROM 映射地址分别为 0x3f400020、0x400d0020；镜像段和两个旧槽的物理起点都符合 64 KiB 页对齐。锁定 IDF 从分区起点加载 app 的格式支持把该镜像放入现有 OTA 槽的离线技术判断，但旧 ESP-AT bootloader 能否实际启动新镜像仍待现场验证。

## 单槽写入与恢复前提

| 候选动作 | 结论 |
| --- | --- |
| 只写历史非活动 ota_1 | 镜像能装入槽内，但擦除态 otadata 且旧表无 factory，当前已证明的启动仍落在 ota_0；只写 app 不会运行样例 |
| 只写历史活动 ota_0 | 可能由旧 bootloader 装载，但会覆盖唯一已证明可启动的 ESP-AT app，没有设备内回退；只能在新鲜双份同板完整恢复件、ROM 下载模式和物理回刷可执行时考虑 |
| 先写 ota_1 再改变启动选择 | 可保留 ota_0 原字节，但需要明确的 otadata 写入、旧 bootloader 兼容、试运行确认与失败恢复合同；样例没有 OTA 确认入口，不能当作单次 app 写入 |

写入当天须先重新枚举两板端点，只绑定目标 ESP32，核对芯片/MAC、容量、原设备身份、eFuse 安全状态、旧 bootloader、分区表、otadata、两个 app 槽、NVS 与 at_customize 边界；探测可能复位板卡，应记录 boot 变化。随后为该板重新取得两份一致的完整 Flash 恢复件，固定拟写槽与原 otadata 的精确恢复范围。只有受控实验 Wi-Fi 和可达的隔离 TLS Broker、DNS/SAN、CA、独立 ClientID、Topic、ACL 真实输入齐备时，才构建最终联网镜像并记录新的大小、摘要和镜像头。

获得精确写入授权后，只能按当次确认的槽与选择合同执行，写后读回比较最终镜像，再用设备与 Broker 双侧事实验证。失败时由同板本轮恢复件还原被改变的 OTA app 槽；若改变过 otadata，也还原原选择记录，读回核对并重启确认旧 ESP-AT、身份和配置。普通启动不可用时必须已有经核对可达的目标板 ROM 下载模式及物理接入。不得跨板回刷，不擦除 NVS/at_customize，不写 eFuse。

本轮只安装并核对仓外固定工具链、构建空输入镜像、离线读取历史两份恢复件和检查源码；未打开串口、未写板、未连接真实 Wi-Fi/TLS Broker。P3-07 的 ESP32 实板矩阵与百次资源回收仍未验收。
