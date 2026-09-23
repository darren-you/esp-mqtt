#!/usr/bin/env python3
"""Real ESP-IDF Linux MQTT core against a controlled local MQTT 3.1.1 broker."""

from __future__ import annotations

import argparse
import os
import queue
import select
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, field


BASE = "test/base"
DYNAMIC = "test/dynamic"
PUBLISH = "test/publish"


def packet(kind: int, body: bytes) -> bytes:
    length = len(body)
    encoded = bytearray()
    while True:
        digit = length % 128
        length //= 128
        encoded.append(digit | (0x80 if length else 0))
        if not length:
            break
    return bytes([kind]) + encoded + body


def take_packet(buffer: bytearray) -> tuple[int, bytes] | None:
    if len(buffer) < 2:
        return None
    length = 0
    factor = 1
    at = 1
    while True:
        if at >= len(buffer):
            return None
        digit = buffer[at]
        length += (digit & 127) * factor
        at += 1
        if not digit & 128:
            break
        if at > 4:
            raise ValueError("malformed MQTT remaining length")
        factor *= 128
    if length > 16_384:
        raise ValueError("unexpected MQTT packet size")
    if len(buffer) < at + length:
        return None
    result = buffer[0], bytes(buffer[at : at + length])
    del buffer[: at + length]
    return result


def text_at(body: bytes, at: int) -> tuple[str, int]:
    if len(body) < at + 2:
        raise ValueError("short MQTT string")
    size = int.from_bytes(body[at : at + 2], "big")
    at += 2
    if len(body) < at + size:
        raise ValueError("short MQTT string content")
    return body[at : at + size].decode("utf-8"), at + size


@dataclass
class Connection:
    index: int
    sock: socket.socket
    began: float
    subscriptions: set[str] = field(default_factory=set)
    held_since: float | None = None
    pending_first_puback: int | None = None
    pending_second_puback: int | None = None
    first_puback_released_at: float | None = None
    second_puback_released_at: float | None = None
    buffer: bytearray = field(default_factory=bytearray)


class Broker:
    def __init__(self, scenario: str) -> None:
        self.scenario = scenario
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.listener.setblocking(False)
        self.port = self.listener.getsockname()[1]
        self.connections: list[Connection] = []
        self.events: list[tuple[float, int, str, object]] = []
        self.current: Connection | None = None

    def note(self, conn: Connection, kind: str, detail: object) -> None:
        stamp = time.monotonic()
        self.events.append((stamp, conn.index, kind, detail))
        print(f"BROKER connection={conn.index} at={stamp - conn.began:.2f}s kind={kind} detail={detail}", flush=True)

    def send(self, conn: Connection, kind: int, body: bytes) -> None:
        conn.sock.sendall(packet(kind, body))

    def close_current(self) -> None:
        conn = self.current
        if conn is None:
            return
        self.note(conn, "drop", tuple(sorted(conn.subscriptions)))
        try:
            conn.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        conn.sock.close()
        self.current = None

    def maybe_timers(self) -> None:
        conn = self.current
        if conn is None:
            return
        now = time.monotonic()
        if self.scenario == "full" and conn.index == 2 and conn.pending_first_puback is not None and now - conn.began >= 5.8:
            self.send(conn, 0x40, conn.pending_first_puback.to_bytes(2, "big"))
            conn.first_puback_released_at = now
            self.note(conn, "puback_release", "first")
            conn.pending_first_puback = None
        final_connection = 3 if self.scenario == "full" else 2
        if conn.index == final_connection and conn.pending_second_puback is not None and now - conn.began >= 5.8:
            self.send(conn, 0x40, conn.pending_second_puback.to_bytes(2, "big"))
            conn.second_puback_released_at = now
            self.note(conn, "puback_release", "second")
            conn.pending_second_puback = None
        if conn.held_since is not None and now - conn.held_since >= 9.0:
            self.close_current()

    def handle(self, conn: Connection, header: int, body: bytes) -> None:
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            if name != "MQTT" or body[at] != 4 or not body[at + 1] & 0x02:
                raise ValueError("expected MQTT 3.1.1 clean session")
            client_id, _ = text_at(body, at + 4)
            self.note(conn, "connect", client_id)
            self.send(conn, 0x20, b"\x00\x00")
        elif kind == 8:
            if header != 0x82 or len(body) < 5:
                raise ValueError("malformed SUBSCRIBE")
            message_id = int.from_bytes(body[:2], "big")
            at = 2
            topics: list[str] = []
            grants = bytearray()
            while at < len(body):
                topic, at = text_at(body, at)
                grants.append(body[at])
                at += 1
                topics.append(topic)
                conn.subscriptions.add(topic)
            self.note(conn, "subscribe", (message_id, tuple(topics)))
            if self.scenario == "full" and conn.index == 1 and topics == [DYNAMIC]:
                conn.held_since = conn.held_since or time.monotonic()
                self.note(conn, "suback_held", message_id)
            else:
                self.send(conn, 0x90, body[:2] + grants)
        elif kind == 10:
            if header != 0xA2 or len(body) < 4:
                raise ValueError("malformed UNSUBSCRIBE")
            message_id = int.from_bytes(body[:2], "big")
            at = 2
            topics: list[str] = []
            while at < len(body):
                topic, at = text_at(body, at)
                topics.append(topic)
                conn.subscriptions.discard(topic)
            self.note(conn, "unsubscribe", (message_id, tuple(topics)))
            held_unsubscribe_connection = 2 if self.scenario == "full" else 1
            if conn.index == held_unsubscribe_connection and topics == [DYNAMIC]:
                conn.held_since = conn.held_since or time.monotonic()
                self.note(conn, "unsuback_held", message_id)
            else:
                self.send(conn, 0xB0, body[:2])
        elif kind == 3:
            topic, at = text_at(body, 0)
            qos = (header >> 1) & 3
            message_id = int.from_bytes(body[at : at + 2], "big") if qos else 0
            if qos:
                at += 2
            payload = body[at:].decode("ascii")
            self.note(conn, "publish", (topic, payload, message_id, qos, bool(header & 8)))
            if topic != PUBLISH or qos != 1 or payload not in ("first", "second"):
                raise ValueError("unexpected PUBLISH")
            if self.scenario == "full" and payload == "first" and conn.index == 1:
                return
            if payload == "second" and conn.index == (2 if self.scenario == "full" else 1):
                return
            if self.scenario == "full" and payload == "first" and conn.index == 2 and conn.first_puback_released_at is None:
                conn.pending_first_puback = message_id
                return
            if payload == "second" and conn.index == (3 if self.scenario == "full" else 2) and conn.second_puback_released_at is None:
                conn.pending_second_puback = message_id
                return
            self.send(conn, 0x40, message_id.to_bytes(2, "big"))
        elif kind == 12:
            self.send(conn, 0xD0, b"")
        elif kind == 14:
            self.close_current()
        else:
            raise ValueError(f"unexpected MQTT packet kind {kind}")

    def step(self) -> None:
        self.maybe_timers()
        conn = self.current
        if conn is None:
            readable, _, _ = select.select([self.listener], [], [], 0.05)
            if readable:
                sock, _ = self.listener.accept()
                sock.setblocking(False)
                conn = Connection(len(self.connections) + 1, sock, time.monotonic())
                self.connections.append(conn)
                self.current = conn
                self.note(conn, "accept", None)
            return
        readable, _, _ = select.select([conn.sock], [], [], 0.05)
        if not readable:
            return
        data = conn.sock.recv(8192)
        if not data:
            self.close_current()
            return
        conn.buffer.extend(data)
        while parsed := take_packet(conn.buffer):
            self.handle(conn, *parsed)
            if self.current is None:
                break

    def verify(self) -> None:
        if self.scenario == "unsub-only":
            self.verify_unsubscribe_only()
            return
        if len(self.connections) != 3:
            raise AssertionError(f"expected 3 clean-session connections, got {len(self.connections)}")
        by_connection = lambda number, kind: [
            (stamp, detail) for stamp, index, event, detail in self.events
            if index == number and event == kind
        ]
        first_hold = by_connection(1, "suback_held")
        second_hold = by_connection(2, "unsuback_held")
        first_drop = by_connection(1, "drop")
        second_drop = by_connection(2, "drop")
        if not first_hold or not first_drop or first_drop[0][0] - first_hold[0][0] < 5:
            raise AssertionError("held SUBACK did not cross the 5 s retransmission window")
        if not second_hold or not second_drop or second_drop[0][0] - second_hold[0][0] < 5:
            raise AssertionError("held UNSUBACK did not cross the 5 s retransmission window")
        release_first = by_connection(2, "puback_release")
        release_second = by_connection(3, "puback_release")
        if len(release_first) != 1 or len(release_second) != 1:
            raise AssertionError("inflight QoS 1 PUBLISH did not receive a post-reconnect PUBACK")
        early_dynamic = [detail for stamp, detail in by_connection(2, "subscribe")
                         if stamp < release_first[0][0] and DYNAMIC in detail[1]]
        if early_dynamic:
            raise AssertionError(f"old unacknowledged SUBSCRIBE leaked across clean session: {early_dynamic}")
        stale_unsubscribe = [detail for _, detail in by_connection(3, "unsubscribe") if DYNAMIC in detail[1]]
        if stale_unsubscribe:
            raise AssertionError(f"old unacknowledged UNSUBSCRIBE leaked across clean session: {stale_unsubscribe}")
        if self.connections[2].subscriptions != {BASE, DYNAMIC}:
            raise AssertionError(f"wrong final Broker subscription set: {self.connections[2].subscriptions}")
        first_packets = [(detail[1], detail[4]) for _, index, kind, detail in self.events
                         if kind == "publish" and index == 2 and detail[1] == "first"]
        second_packets = [(detail[1], detail[4]) for _, index, kind, detail in self.events
                          if kind == "publish" and index == 3 and detail[1] == "second"]
        if not any(dup for _, dup in first_packets) or not any(dup for _, dup in second_packets):
            raise AssertionError("QoS 1 PUBLISH was not resent with DUP after reconnect")

    def verify_unsubscribe_only(self) -> None:
        if len(self.connections) != 2:
            raise AssertionError(f"expected 2 clean-session connections, got {len(self.connections)}")
        held = [stamp for stamp, index, kind, _ in self.events if index == 1 and kind == "unsuback_held"]
        dropped = [stamp for stamp, index, kind, _ in self.events if index == 1 and kind == "drop"]
        if not held or not dropped or dropped[0] - held[0] < 5:
            raise AssertionError("held UNSUBACK did not cross 5 s retransmission window")
        stale = [detail for _, index, kind, detail in self.events
                 if index == 2 and kind == "unsubscribe" and DYNAMIC in detail[1]]
        if stale:
            raise AssertionError(f"old unacknowledged UNSUBSCRIBE leaked across clean session: {stale}")
        if self.connections[1].subscriptions != {BASE, DYNAMIC}:
            raise AssertionError(f"wrong final Broker subscription set: {self.connections[1].subscriptions}")
        retransmitted = [detail for _, index, kind, detail in self.events
                         if index == 2 and kind == "publish" and detail[1] == "second" and detail[4]]
        if not retransmitted or not any(index == 2 and kind == "puback_release" for _, index, kind, _ in self.events):
            raise AssertionError("QoS 1 PUBLISH was not resent with DUP and acknowledged")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, help="built IDF Linux .elf")
    parser.add_argument("--scenario", choices=("full", "unsub-only"), default="full")
    args = parser.parse_args()
    broker = Broker(args.scenario)
    env = dict(os.environ, EMQTT_TEST_PORT=str(broker.port), EMQTT_SCENARIO=args.scenario)
    app = subprocess.Popen([args.app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1, env=env)
    lines: queue.Queue[str] = queue.Queue()

    def read_app() -> None:
        assert app.stdout is not None
        for line in app.stdout:
            lines.put(line.rstrip())

    reader = threading.Thread(target=read_app, daemon=True)
    reader.start()
    deadline = time.monotonic() + 58
    try:
        while time.monotonic() < deadline:
            broker.step()
            while not lines.empty():
                print("APP " + lines.get_nowait(), flush=True)
            if app.poll() is not None:
                break
        if app.poll() is None:
            raise TimeoutError("IDF Linux app did not exit")
        while not lines.empty():
            print("APP " + lines.get_nowait(), flush=True)
        if app.returncode != 0:
            raise AssertionError(f"IDF Linux app exited {app.returncode}")
        broker.verify()
        print(f"BROKER TEST PASS scenario={args.scenario}: real MQTT core, clean reconnect, QoS1 DUP/PUBACK")
        return 0
    except (AssertionError, OSError, TimeoutError, ValueError) as error:
        print(f"BROKER TEST FAIL {error}", flush=True)
        return 1
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=3)
            except subprocess.TimeoutExpired:
                app.kill()
        broker.close_current()
        broker.listener.close()


if __name__ == "__main__":
    raise SystemExit(main())
