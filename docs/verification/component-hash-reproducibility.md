# 组件摘要跨缓存检查点

2026-10-02，Base 的官方依赖解析发现：MQTT 仍使用同一个公开 SHA `a46e209cc98c7b910774dbb77d11b34f79492720`，但新缓存生成的组件摘要与旧锁不同。实际下载的 `test/tools/paho.mqtt.testing/.git` 含指向当前 Git 缓存的定位路径；官方 Component Manager 3.1.2 默认排除 Git 目录内容，却没有排除该普通文件，因此将运行环境路径计入摘要。

本仓在事实源 `idf_component.yml` 的 `files.exclude` 中排除 `**/.git`。它只控制组件分发文件集合，保留子模块本身的固定源码、README 与许可；没有修改下载目录、工具源码或 MQTT C 实现。

`tests/test_component_hash.py` 使用官方 ManifestManager、文件过滤及摘要函数：两个缓存夹具保留相同实际源码与子模块内容，只改变 `.git` 定位路径，未过滤输入的摘要必须不同，按真实清单过滤后的包摘要必须相同；定位文件不存在，其他源码与子模块内容逐字节保持。该回归在 Component Manager 3.1.2 通过。

```bash
python -m unittest discover -s tests -p test_component_hash.py -v
```

此测试需要固定 SDK 环境提供的 `idf-component-manager==3.1.2`，不新增 MQTT 设备运行依赖。公开新 SHA 的两个独立官方缓存解析仍须分别核对源版本、完整组件摘要与下载文件集合；Base 只在该核对通过后消费。历史 C3 三十组网络矩阵仍绑定原 `8a02b286b62cb015d02239be0a8e9fc5012a868c`，本次清单修正不宣称新的实板候选已验收。
