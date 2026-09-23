#!/usr/bin/env python3
"""在获准运行的实验 C3 上逐次验证 100 次 MQTT 实例回收。"""

import argparse
import csv
import sys
import time


def fields(line):
    parts = line.strip().split()
    if not parts or parts[0] not in ("EMQTT_SAMPLE", "EMQTT_SAMPLE_EVENT"):
        return None, {}
    return parts[0], dict(part.split("=", 1) for part in parts[1:] if "=" in part)


def await_line(port, timeout_seconds, accept):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        kind, values = fields(raw.decode("utf-8", errors="replace"))
        if kind == "EMQTT_SAMPLE_EVENT" and values.get("kind") == "7":
            raise RuntimeError(f"运行层报告错误：{values}")
        if accept(kind, values):
            return values
    raise TimeoutError(f"等待设备回执超过 {timeout_seconds} 秒")


def send(port, name):
    port.write((name + "\n").encode("ascii"))
    port.flush()


def ready_stats(port, timeout_seconds, expected_cycle=None):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        send(port, "stats")
        try:
            return await_line(port, min(3.0, deadline - time.monotonic()),
                              lambda kind, values: kind == "EMQTT_SAMPLE" and
                              values.get("phase") == "requested" and
                              values.get("state") == "3" and
                              values.get("wifi") == "1" and values.get("trusted") == "1" and
                              values.get("outbox") == "0" and
                              (expected_cycle is None or values.get("cycle") == str(expected_cycle)))
        except TimeoutError:
            continue
    raise TimeoutError("设备未进入联网、可信时间、MQTT READY 与空 outbox 状态")


def run(port, count, timeout_seconds):
    baseline = ready_stats(port, timeout_seconds)
    cycle = int(baseline["cycle"])
    writer = csv.writer(sys.stdout)
    writer.writerow(("cycle", "heap_free", "heap_min", "heap_largest", "outbox", "time_ms"))
    for _ in range(count):
        cycle += 1
        send(port, "cycle")
        response = await_line(
            port, timeout_seconds,
            lambda kind, values: kind == "EMQTT_SAMPLE" and values.get("command") == "cycle")
        if response.get("error") != "0" or response.get("cycle") != str(cycle):
            raise RuntimeError(f"第 {cycle} 次回收命令失败：{response}")
        await_line(port, timeout_seconds,
                   lambda kind, values: kind == "EMQTT_SAMPLE_EVENT" and values.get("kind") == "1")
        online = await_line(port, timeout_seconds,
                            lambda kind, values: kind == "EMQTT_SAMPLE" and "online_error" in values)
        if online["online_error"] != "0" or int(online.get("message_id", "-1")) <= 0:
            raise RuntimeError(f"第 {cycle} 次在线状态入队失败：{online}")
        await_line(port, timeout_seconds,
                   lambda kind, values: kind == "EMQTT_SAMPLE_EVENT" and
                   values.get("kind") == "4" and values.get("message_id") == online["message_id"])
        snapshot = ready_stats(port, timeout_seconds, cycle)
        writer.writerow((cycle, snapshot["heap_free"], snapshot["heap_min"],
                         snapshot["heap_largest"], snapshot["outbox"], snapshot["time_ms"]))
        sys.stdout.flush()
    print(f"完成 {count} 次回收与逐次 READY；请比较 CSV 资源趋势及 Broker 侧连接记录。",
          file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="当次明确核对的实验 C3 串口")
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--timeout-seconds", type=float, default=45)
    args = parser.parse_args()
    if not 1 <= args.count <= 1000 or args.timeout_seconds <= 0:
        parser.error("count 必须为 1..1000，timeout-seconds 必须大于 0")
    try:
        import serial
    except ImportError as error:
        parser.error(f"缺少 pyserial；使用锁定 ESP-IDF 导出的 Python：{error}")
    try:
        with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=3) as port:
            run(port, args.count, args.timeout_seconds)
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as error:
        print(f"ESP MQTT 串口回收验证失败：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
