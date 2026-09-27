# 当前精确锁的 Linux Broker 回归

日期：2026-09-27。输入为 `esp-mqtt@c0677e5e779c3e51e814f2920420be7ec54f1d88`，即本轮 ESP Base 固定的公开 MQTT 源码。本次重跑仓内现有测试；未修改 MQTT 核心、运行层或测试逻辑，未连接生产 Broker 或写入设备。

## 输入与执行

| 项目 | 本次事实 |
| --- | --- |
| 源码 | 独立检出 `c0677e5e779c3e51e814f2920420be7ec54f1d88`；复制到 `mac-work-1` 仓外目录后，以 `rsync --checksum --dry-run --delete` 核对一致 |
| 固定 SDK | `esp-space/esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c`、`esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d`；`python3 tools/sdk.py check --path "$IDF_PATH"` 通过 |
| 核心与运行层 SHA-256 | `mqtt_client.c`: `e92157b9562cced85b7c542e596971401c2b42f54badb4b84f9f205b69a0967e`；`runtime/emqtt.c`: `b51473ac435e1e9b6e700c2c7f65b0812e4b5f029f56daa9b2bdaab159077590` |
| 主机合同 | `mac-ci-1` 独立检出中执行 `bash tests/host/run.sh`：合同与运行层通过，启用 ASan/UBSan；该 fake 不连接 Broker |
| Linux 构建 | `mac-work-1`，固定 SDK 的 `linux` target、仓内真实 MQTT 核心和 `emqtt_` 运行层；严格 TLS 与生命周期各用独立构建目录 |

在导出上述固定 SDK 环境后，从该独立源码根运行：

```bash
bash tests/linux-broker/run.sh "$PWD" /tmp/esp-mqtt-current-lock-broker-recheck-20260927-evidence/tls tls
bash tests/linux-broker/run.sh "$PWD" /tmp/esp-mqtt-current-lock-broker-recheck-20260927-evidence/lifecycle lifecycle
```

非交互 SSH 会话只在执行环境显式提供 Homebrew Python 路径以导出 SDK；仓库测试入口与系统配置未变。两个场景只监听本机回环的临时端口。TLS 测试 CA、服务端证书和密钥在权限受限的临时目录生成，退出时删除。

## 结果

| 场景 | 结果 |
| --- | --- |
| 严格 TLS | `CONFIG_MQTT_TRANSPORT_SSL=y` 且明文实验开关关闭。无可信时间时拒绝启动；正确 CA 与 `127.0.0.1` IP SAN 时依次完成 CONNECT、SUBACK、QoS1 PUBACK；无关 CA 与错误主机名 `localhost` 均在 MQTT CONNECT 前被 TLS 拒绝，Broker 对这两例收到零条 MQTT 报文。三例全部通过。 |
| 100 次生命周期 | `CONFIG_EMQTT_PLAINTEXT_LAB=y`，本机隔离明文 Broker。100 个 `BROKER CYCLE` 与 100 个 `APP TEST CYCLE` 序号分别连续为 1–100；200 次连接、300 次 SUBACK、100 次 QoS1 PUBACK、100 次 UNSUBACK，逐轮清理并重连；客户端和 Broker 均输出最终 PASS。 |
| 销毁后主机资源 | macOS 默认 malloc zone 使用量每轮均为 100,480 B，0–255 范围内打开的 fd 每轮均为 3；RSS 首轮 2,260,992 B、末轮 2,916,352 B，后 60 轮范围 2,883,584–2,916,352 B。没有 `TEST FAIL` 或 `BROKER TEST FAIL`。 |

TLS ELF SHA-256 为 `028372e5c67ac97d36cc480e59d514fd58454744355145328b1cd3fc698d24fd`；生命周期 ELF 为 `062311f3c0a244a3461ae5b5be209665bcc80b65b76590ecdc0efffc9689ed34`。原始日志暂存在 `mac-work-1:/tmp/esp-mqtt-current-lock-broker-recheck-20260927-evidence/`，并同步到 `mac-ci-1:/private/tmp/esp-mqtt-current-lock-broker-recheck-20260927-evidence/`；`tls.log` SHA-256 为 `451f28e6a17461d9099489753c941af3a982839e13d703dc6904bc87d4dcf487`，`lifecycle.log` 为 `e6b1f6481f8c257b61ecf5501a78f692fd65ece590ceadb9f92adb2b07741321`。

Linux 回归证明当前精确锁的真实 MQTT 核心在这些隔离软件场景没有回归。主机 RSS 与 malloc 采样不等于 ESP 堆；这里没有两板 Wi-Fi/SNTP、生产 Broker ACL、真实设备资源曲线或业务命令 ACK。因此 P3-07 的两板实测与 P3-08 的设备命令闭环仍未完成。
