#!/usr/bin/env python3
"""100 real-core create/connect/QoS1/reconnect/unsubscribe/destroy cycles."""

from __future__ import annotations

import argparse
import os
import queue
import re
import select
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, field

from run_broker_test import packet, take_packet, text_at


BASE = "test/lifecycle/base"
DYNAMIC = "test/lifecycle/dynamic"
OUT = "test/lifecycle/out"
CLIENT_ID = "linux-lifecycle-real-core"
CYCLE_LINE = re.compile(
    r"^TEST CYCLE cycle=(\d+) rss_bytes=(\d+) malloc_in_use_bytes=(\d+) fd_0_255=(\d+)$"
)


@dataclass
class Connection:
    index: int
    sock: socket.socket
    buffer: bytearray = field(default_factory=bytearray)
    subscriptions: set[str] = field(default_factory=set)
    connect_count: int = 0
    subscribe_calls: int = 0
    publish_count: int = 0
    unsubscribe_count: int = 0
    drop_at: float | None = None
    close_reason: str | None = None

    @property
    def cycle(self) -> int:
        return (self.index + 1) // 2

    @property
    def first(self) -> bool:
        return self.index % 2 == 1


class Broker:
    def __init__(self, cycles: int) -> None:
        self.cycles = cycles
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.listener.setblocking(False)
        self.port = self.listener.getsockname()[1]
        self.connections: list[Connection] = []
        self.current: Connection | None = None

    def close_current(self, reason: str) -> None:
        conn = self.current
        if conn is None:
            return
        conn.close_reason = reason
        try:
            conn.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        conn.sock.close()
        self.current = None
        if not conn.first:
            print(f"BROKER CYCLE cycle={conn.cycle} connect=2 suback=3 "
                  "qos1_puback=1 reconnect=1 unsuback=1 closed=2", flush=True)

    def handle(self, conn: Connection, header: int, body: bytes) -> None:
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            if name != "MQTT" or len(body) < at + 4 or body[at] != 4 or not body[at + 1] & 2:
                raise AssertionError("expected MQTT 3.1.1 clean-session CONNECT")
            client_id, _ = text_at(body, at + 4)
            if client_id != CLIENT_ID or conn.connect_count:
                raise AssertionError(f"wrong CONNECT identity/order: {client_id}")
            conn.connect_count += 1
            conn.sock.sendall(packet(0x20, b"\x00\x00"))
        elif kind == 8:
            if header != 0x82 or len(body) < 5 or conn.connect_count != 1:
                raise AssertionError("malformed SUBSCRIBE")
            message_id = body[:2]
            at = 2
            topics: list[str] = []
            grants = bytearray()
            while at < len(body):
                topic, at = text_at(body, at)
                if at >= len(body) or body[at] != 1:
                    raise AssertionError("expected QoS1 subscription")
                topics.append(topic)
                grants.append(body[at])
                at += 1
            expected = ([BASE] if conn.subscribe_calls == 0 else [DYNAMIC]) if conn.first else [BASE, DYNAMIC]
            if topics != expected or (not conn.first and conn.subscribe_calls):
                raise AssertionError(f"cycle {conn.cycle}: unexpected SUBSCRIBE {topics}, expected {expected}")
            conn.subscribe_calls += 1
            conn.subscriptions.update(topics)
            conn.sock.sendall(packet(0x90, message_id + grants))
        elif kind == 3:
            topic, at = text_at(body, 0)
            qos = (header >> 1) & 3
            message_id = body[at: at + 2]
            payload = body[at + 2:]
            if not conn.first or conn.publish_count or conn.subscribe_calls != 2 or \
                    topic != OUT or qos != 1 or header & 8 or header & 1 or \
                    len(message_id) != 2 or int.from_bytes(message_id, "big") == 0 or \
                    payload != f"cycle={conn.cycle}".encode("ascii"):
                raise AssertionError(f"cycle {conn.cycle}: unexpected QoS1 PUBLISH")
            conn.publish_count += 1
            conn.sock.sendall(packet(0x40, message_id))
            # A short delay lets the real worker dispatch PUBACK before the forced TCP close.
            conn.drop_at = time.monotonic() + 0.25
        elif kind == 10:
            if header != 0xA2 or len(body) < 4 or conn.first or \
                    conn.unsubscribe_count or conn.subscribe_calls != 1:
                raise AssertionError("unexpected UNSUBSCRIBE")
            topic, end = text_at(body, 2)
            if topic != DYNAMIC or end != len(body):
                raise AssertionError(f"cycle {conn.cycle}: wrong unsubscribe {topic}")
            conn.unsubscribe_count += 1
            conn.subscriptions.remove(DYNAMIC)
            conn.sock.sendall(packet(0xB0, body[:2]))
        elif kind == 12:
            conn.sock.sendall(packet(0xD0, b""))
        elif kind == 14:
            if conn.first:
                raise AssertionError("client disconnected before forced reconnect")
            self.close_current("client_disconnect")
        else:
            raise AssertionError(f"cycle {conn.cycle}: unexpected packet kind {kind}")

    def step(self) -> None:
        conn = self.current
        if conn and conn.drop_at is not None and time.monotonic() >= conn.drop_at:
            self.close_current("forced_drop")
            return
        if conn is None:
            readable, _, _ = select.select([self.listener], [], [], 0.02)
            if readable:
                sock, _ = self.listener.accept()
                sock.setblocking(False)
                conn = Connection(len(self.connections) + 1, sock)
                if conn.cycle > self.cycles:
                    raise AssertionError("more CONNECTs than requested cycles")
                self.connections.append(conn)
                self.current = conn
            return
        readable, _, _ = select.select([conn.sock], [], [], 0.02)
        if not readable:
            return
        data = conn.sock.recv(8192)
        if not data:
            self.close_current("client_eof")
            return
        conn.buffer.extend(data)
        while parsed := take_packet(conn.buffer):
            self.handle(conn, *parsed)
            if self.current is None:
                break

    def verify(self) -> None:
        if len(self.connections) != self.cycles * 2 or self.current is not None:
            raise AssertionError(f"connection count/close: {len(self.connections)} / {self.cycles * 2}")
        for conn in self.connections:
            expected_subscriptions = {BASE, DYNAMIC} if conn.first else {BASE}
            if conn.connect_count != 1 or conn.subscribe_calls != (2 if conn.first else 1) or \
                    conn.publish_count != (1 if conn.first else 0) or \
                    conn.unsubscribe_count != (0 if conn.first else 1) or \
                    conn.subscriptions != expected_subscriptions or \
                    conn.close_reason != ("forced_drop" if conn.first else "client_disconnect"):
                raise AssertionError(f"cycle {conn.cycle}: incomplete broker lifecycle: {conn}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    parser.add_argument("--count", type=int, default=100)
    args = parser.parse_args()
    if not 1 <= args.count <= 100:
        parser.error("--count must be 1..100")
    broker = Broker(args.count)
    env = dict(os.environ, EMQTT_TEST_PORT=str(broker.port), EMQTT_TEST_COUNT=str(args.count))
    app = subprocess.Popen([args.app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1, env=env)
    lines: queue.Queue[str] = queue.Queue()

    def read_app() -> None:
        assert app.stdout is not None
        for line in app.stdout:
            lines.put(line.rstrip())

    threading.Thread(target=read_app, daemon=True).start()
    samples: list[tuple[int, int, int, int]] = []
    deadline = time.monotonic() + args.count * 10 + 25
    try:
        while time.monotonic() < deadline:
            broker.step()
            while not lines.empty():
                line = lines.get_nowait()
                print("APP " + line, flush=True)
                if match := CYCLE_LINE.fullmatch(line):
                    samples.append(tuple(map(int, match.groups())))
            if app.poll() is not None:
                break
        if app.poll() is None:
            raise TimeoutError("IDF Linux app did not complete lifecycle cycles")
        for _ in range(50):
            if broker.current is None:
                break
            broker.step()
        while not lines.empty():
            line = lines.get_nowait()
            print("APP " + line, flush=True)
            if match := CYCLE_LINE.fullmatch(line):
                samples.append(tuple(map(int, match.groups())))
        if app.returncode != 0:
            raise AssertionError(f"IDF Linux app exited {app.returncode}")
        broker.verify()
        if [sample[0] for sample in samples] != list(range(1, args.count + 1)):
            raise AssertionError("missing or duplicate lifecycle resource samples")
        fd_values = [sample[3] for sample in samples]
        if fd_values[-1] > fd_values[0] or max(fd_values) > fd_values[0] + 2:
            raise AssertionError(f"file descriptors accumulated: {fd_values}")
        print(f"BROKER TEST PASS scenario=lifecycle cycles={args.count} "
              f"connections={len(broker.connections)} subacks={args.count * 3} "
              f"qos1_pubacks={args.count} unsubacks={args.count} "
              f"rss_first_last={samples[0][1]}/{samples[-1][1]} "
              f"malloc_in_use_first_last={samples[0][2]}/{samples[-1][2]} "
              f"fd_first_last={fd_values[0]}/{fd_values[-1]}", flush=True)
        return 0
    except (AssertionError, OSError, TimeoutError, ValueError) as error:
        print(f"BROKER TEST FAIL {error}", flush=True)
        return 1
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=2)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
        if broker.current is not None:
            broker.close_current("test_cleanup")
        broker.listener.close()


if __name__ == "__main__":
    raise SystemExit(main())
