# ESP32 IRAM 溢出标志回归

验证单核 ESP32 将运行对象放入可按字节访问的 IRAM 时，第四条完整消息在三个槽仍满时必须停止 client、报告 `EMQTT_ERROR_QUEUE` 并进入 `EMQTT_FAILED`。测试直接编译本仓 `emqtt.c`，使用真实 CPU、分配器和 FreeRTOS 队列／临界区；官方 client API 与事件由小型 fake 注入，不连接 Broker，不代表 TLS、公网吞吐、并发调度或 Base 实板资格。

## 构建

从本仓根导出 `sdk-lock.json` 指向的 SDK 环境，将下列路径替换为新的仓外输出目录：

```bash
idf.py -C tests/iram-overflow -B /absolute/private/iram-overflow-build \
  -D IDF_TARGET=esp32 -D SDKCONFIG=/absolute/private/iram-overflow-sdkconfig build
```

CMake 核对固定 SDK，要求单核、IRAM 字节访问与 pure RAM app。产物 `emqtt_iram_overflow.bin` 只供已授权、精确识别并持有独占串口的 ESP32 RAM 实验使用；构建不连接设备，不调用 `flash`。不要把通用构建结束提示中的 Flash 命令用于此测试。

## 运行与判定

先核对当前设备身份、恢复基线和没有未决写操作，再用固定官方 esptool 的 ROM `load-ram` 加载该镜像，采集 115200 串口。测试不写 Flash／NVS，不读取生产配置；结束后由操作方复位回原 Flash 应用，并独立核对原固件、配置和收据。

输出必须有 `runtime_in_iram=1`，四轮 `fourth_complete` 均为 `kind=7 error=7 state=5 stop_delta=1 pass=1`，最后为 `done failures=0`。每轮在同一 runtime 上重新 start、注入四条完整消息并清理；这不是业务／OTA 百次生命周期验收。缺失终行、断线、断言或任一 `pass=0` 都不能记为通过。

该回归针对已在 ESP32 复现的 IRAM `atomic_exchange` 错误返回：原标志为 true，交换清为 false 后仍返回 false，导致溢出被吞掉。运行层改用已有资源临界区读取并清除标志；三槽边界、消息所有权、公开接口和 IRAM 存储策略保持。host ASan/UBSan 继续覆盖其他溢出来源和释放路径，不能替代此 CPU 内存域回归。
