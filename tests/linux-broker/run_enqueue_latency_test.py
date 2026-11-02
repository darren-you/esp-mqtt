#!/usr/bin/env python3
"""Measure actual core enqueue-to-wire latency on an isolated loopback broker."""
from __future__ import annotations

import argparse
import json
import os
import queue
import socket
import statistics
import subprocess
import threading
import time
from pathlib import Path

from run_broker_test import Broker, Connection, packet, text_at

BLOCKS = 12
BLOCK_BYTES = 1024


class LatencyBroker(Broker):
    def __init__(self) -> None:
        super().__init__("enqueue-latency")
        self.records: list[dict] = []
        self.requests: list[int] = []
        self.data_acks: list[int] = []

    def record(self, kind: str, **fields) -> None:
        # Python monotonic() may use a different macOS epoch from clock_gettime.
        self.records.append({"kind": kind, "us": time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000, **fields})

    def maybe_timers(self) -> None:
        pass

    def handle(self, conn: Connection, header: int, body: bytes) -> None:
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            if name != "MQTT" or len(body) < at + 4 or body[at:at + 2] != b"\x04\x06":
                raise AssertionError("expected MQTT 3.1.1 clean session with will")
            client_id, _ = text_at(body, at + 4)
            if client_id != "linux-enqueue-latency" or conn.index != 1:
                raise AssertionError("unexpected client/connection")
            conn.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.send(conn, 0x20, b"\x00\x00")
        elif kind == 8:
            topic, at = text_at(body, 2)
            if header != 0x82 or topic != "test/data" or body[at:] != b"\x01" or conn.subscriptions:
                raise AssertionError("unexpected subscription")
            conn.subscriptions.add(topic)
            self.send(conn, 0x90, body[:2] + b"\x01")
        elif kind == 3:
            topic, at = text_at(body, 0)
            message_id = int.from_bytes(body[at:at + 2], "big")
            block = len(self.requests)
            if header != 0x32 or topic != "test/request" or body[at + 2:] != str(block).encode() or \
                    block >= BLOCKS or message_id == 0 or message_id in self.requests:
                raise AssertionError("request sequence/flags/ID")
            if self.data_acks != list(range(block)):
                raise AssertionError("next request before previous DATA PUBACK")
            self.record("request_wire", block=block, message_id=message_id)
            self.requests.append(message_id)
            payload = bytes((block + i) % 256 for i in range(BLOCK_BYTES))
            topic_bytes = b"test/data"
            data = len(topic_bytes).to_bytes(2, "big") + topic_bytes + \
                (0x4000 + block).to_bytes(2, "big") + payload
            # Real MQTT packets, immediate reply, no synthetic clock/wakeup.
            conn.sock.sendall(packet(0x40, message_id.to_bytes(2, "big")) + packet(0x32, data))
            self.record("puback_and_data_sent", block=block, message_id=message_id)
        elif kind == 4:
            block = int.from_bytes(body, "big") - 0x4000
            if header != 0x40 or len(body) != 2 or block != len(self.data_acks):
                raise AssertionError("DATA PUBACK sequence")
            self.data_acks.append(block)
            self.record("data_puback_wire", block=block)
        elif kind == 12:
            self.send(conn, 0xD0, b"")
        elif kind == 14:
            self.close_current()
        else:
            raise AssertionError(f"unexpected packet {kind}")


def verify(broker: LatencyBroker, transcript: list[str], poll_ms: int) -> dict:
    traces = []
    for line in transcript:
        if line.startswith("TRACE "):
            _, kind, us, a, b = line.split()
            traces.append(dict(kind=kind, us=int(us), a=int(a), b=int(b)))
    if len(broker.connections) != 1 or len(broker.requests) != BLOCKS or broker.data_acks != list(range(BLOCKS)):
        raise AssertionError("incomplete single-session transfer")

    def exactly(kind: str, a: int | None = None) -> dict:
        matches = [x for x in traces if x["kind"] == kind and (a is None or x["a"] == a)]
        if len(matches) != 1:
            raise AssertionError(f"expected one {kind}/{a}: {matches}")
        return matches[0]

    measurements = []
    for block, message_id in enumerate(broker.requests):
        begin, end = exactly("enqueue_begin", block), exactly("enqueue_end", block)
        poll_begin, poll_end = exactly("poll_begin", begin["b"]), exactly("poll_end", begin["b"])
        puback, data = exactly("puback", block), exactly("data", block)
        wire = next(x for x in broker.records if x["kind"] == "request_wire" and x["block"] == block)
        if poll_begin["b"] != poll_ms or poll_end["b"] != 0 or end["b"] != message_id or puback["b"] != message_id:
            raise AssertionError("wrong configured poll / timeout / message ID")
        if not poll_begin["us"] < begin["us"] <= end["us"] < poll_end["us"] <= wire["us"] <= puback["us"]:
            raise AssertionError("enqueue did not wait inside real poll before reaching wire/PUBACK")
        delay = wire["us"] - end["us"]
        if not poll_ms * 500 <= delay <= poll_ms * 1000 + 250000:
            raise AssertionError(f"unexpected real-poll latency: {delay}")
        measurements.append({"block": block, "message_id": message_id,
                             "enqueue_to_wire_us": delay, "enqueue_to_puback_us": puback["us"] - end["us"],
                             "enqueue_api_us": end["us"] - begin["us"],
                             "poll_us": poll_end["us"] - poll_begin["us"], "data_received_us": data["us"]})
    idle_begin, idle_end = exactly("idle_begin"), exactly("idle_end")
    elapsed = exactly("data", BLOCKS - 1)["us"] - exactly("enqueue_begin", 0)["us"]
    return {"poll_ms": poll_ms, "blocks": BLOCKS, "block_bytes": BLOCK_BYTES,
            "transfer_us": elapsed, "throughput_bytes_per_second": BLOCKS * BLOCK_BYTES * 1e6 / elapsed,
            "latency_median_us": statistics.median(x["enqueue_to_wire_us"] for x in measurements),
            "idle_wall_us": idle_end["us"] - idle_begin["us"], "idle_process_cpu_us": idle_end["b"],
            "idle_polls_started": idle_end["a"] - idle_begin["a"],
            "measurements": measurements, "core_traces": traces, "broker_records": broker.records,
            "scope": "real SDK Linux TCP/FreeRTOS core; controlled enqueue phase; no TLS, device, Flash or production broker"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    parser.add_argument("--poll-ms", type=int, choices=(1000, 100, 50), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    broker = LatencyBroker()
    app = subprocess.Popen([args.app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           env=dict(os.environ, EMQTT_TEST_PORT=str(broker.port)))
    lines: queue.Queue[str] = queue.Queue()
    transcript: list[str] = []

    def read_app() -> None:
        assert app.stdout is not None
        for line in app.stdout:
            lines.put(line.rstrip())

    reader = threading.Thread(target=read_app, daemon=True)
    reader.start()
    result = {"passed": False, "poll_ms": args.poll_ms}
    try:
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline and app.poll() is None:
            broker.step()
        if app.poll() is None:
            raise TimeoutError("app did not exit in 40 seconds")
        reader.join(timeout=2)
        while not lines.empty():
            transcript.append(lines.get_nowait())
        if app.returncode != 0 or "TEST PASS enqueue_latency" not in transcript:
            raise AssertionError(f"app exit={app.returncode} without PASS")
        result.update(verify(broker, transcript, args.poll_ms), passed=True)
        print(json.dumps({k: v for k, v in result.items() if k not in
                          ("measurements", "core_traces", "broker_records")}, indent=2))
        return 0
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=2)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
        reader.join(timeout=2)
        while not lines.empty():
            transcript.append(lines.get_nowait())
        result["transcript"] = transcript
        result["broker_records"] = broker.records
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        broker.close_current()
        broker.listener.close()


if __name__ == "__main__":
    raise SystemExit(main())
