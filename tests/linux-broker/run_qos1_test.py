#!/usr/bin/env python3
"""Test the real MQTT 3.1.1 core's QoS1 ACK and duplicate behavior."""

from __future__ import annotations

import argparse
import os
import queue
import subprocess
import threading
import time

from run_broker_test import Broker, Connection, text_at


TOPIC_BASE = "test/base"
TOPIC_PUBLISH = "test/publish"
INCOMING_ID = 0x4455
INCOMING_BODY = (
    len(TOPIC_BASE).to_bytes(2, "big") + TOPIC_BASE.encode()
    + INCOMING_ID.to_bytes(2, "big") + b"incoming"
)


class QoS1Broker(Broker):
    def __init__(self) -> None:
        super().__init__("qos1-ack")
        self.delayed_id: int | None = None
        self.delayed_first_at: float | None = None
        self.delayed_repeat_at: float | None = None
        self.reconnect_id: int | None = None
        self.incoming_pubacks = 0
        self.ack_pairs_sent = 0

    def maybe_timers(self) -> None:
        # Only actual packets advance this test; no artificial retransmit or ACK.
        return

    def send_ack_pair(self, conn: Connection, message_id: int) -> None:
        ack = message_id.to_bytes(2, "big")
        self.send(conn, 0x40, ack)
        self.send(conn, 0x40, ack)
        self.ack_pairs_sent += 1
        self.note(conn, "puback_pair", message_id)

    def handle(self, conn: Connection, header: int, body: bytes) -> None:
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            if name != "MQTT" or len(body) < at + 4 or body[at] != 4 or not body[at + 1] & 0x02:
                raise ValueError("expected MQTT 3.1.1 clean session")
            client_id, _ = text_at(body, at + 4)
            if client_id != "linux-qos1-ack-test" or conn.index > 2:
                raise ValueError("unexpected client or connection")
            self.note(conn, "connect", client_id)
            self.send(conn, 0x20, b"\x00\x00")
        elif kind == 8:
            if header != 0x82 or len(body) < 5:
                raise ValueError("malformed SUBSCRIBE")
            message_id = int.from_bytes(body[:2], "big")
            topic, at = text_at(body, 2)
            if topic != TOPIC_BASE or body[at:] != b"\x01":
                raise ValueError("unexpected subscription")
            conn.subscriptions.add(topic)
            self.note(conn, "subscribe", (message_id, topic))
            self.send(conn, 0x90, body[:2] + b"\x01")
        elif kind == 3:
            qos = (header >> 1) & 3
            topic, at = text_at(body, 0)
            if qos != 1 or header & 1 or len(body) < at + 2:
                raise ValueError("unexpected PUBLISH flags")
            message_id = int.from_bytes(body[at:at + 2], "big")
            payload = body[at + 2:]
            duplicate = bool(header & 8)
            self.note(conn, "publish", (topic, payload.decode("ascii"), message_id, duplicate))
            if topic != TOPIC_PUBLISH:
                raise ValueError("unexpected PUBLISH topic")
            if payload == b"delayed-ack" and conn.index == 1:
                if self.delayed_id is None:
                    if duplicate:
                        raise AssertionError("first QoS1 PUBLISH was DUP")
                    self.delayed_id = message_id
                    self.delayed_first_at = time.monotonic()
                elif self.delayed_repeat_at is None:
                    if not duplicate or message_id != self.delayed_id:
                        raise AssertionError("same-connection retransmit changed ID or lost DUP")
                    self.delayed_repeat_at = time.monotonic()
                    self.send_ack_pair(conn, message_id)
                    self.send(conn, 0x32, INCOMING_BODY)
                    self.note(conn, "incoming", False)
                else:
                    raise AssertionError("QoS1 PUBLISH continued after PUBACK")
            elif payload == b"reconnect" and conn.index == 1:
                if self.incoming_pubacks != 2 or self.reconnect_id is not None or duplicate:
                    raise AssertionError("reconnect PUBLISH began before incoming ACKs")
                self.reconnect_id = message_id
                self.close_current()
            elif payload == b"reconnect" and conn.index == 2:
                if self.reconnect_id is None or message_id != self.reconnect_id or not duplicate:
                    raise AssertionError("post-reconnect PUBLISH changed ID or lost DUP")
                if self.ack_pairs_sent != 1:
                    raise AssertionError("unexpected second-connection ordering")
                self.send_ack_pair(conn, message_id)
            else:
                raise ValueError("unexpected PUBLISH payload or connection")
        elif kind == 4:
            if header != 0x40 or body != INCOMING_ID.to_bytes(2, "big") or conn.index != 1:
                raise ValueError("unexpected incoming PUBACK")
            self.incoming_pubacks += 1
            self.note(conn, "incoming_puback", self.incoming_pubacks)
            if self.incoming_pubacks == 1:
                self.send(conn, 0x3A, INCOMING_BODY)
                self.note(conn, "incoming", True)
            elif self.incoming_pubacks > 2:
                raise AssertionError("unexpected extra incoming PUBACK")
        elif kind == 12:
            self.send(conn, 0xD0, b"")
        elif kind == 14:
            self.close_current()
        else:
            raise ValueError(f"unexpected MQTT packet kind {kind}")

    def verify(self) -> None:
        if len(self.connections) != 2 or self.ack_pairs_sent != 2 or self.incoming_pubacks != 2:
            raise AssertionError("incomplete QoS1 exchanges")
        if self.delayed_id is None or self.reconnect_id is None or self.delayed_id == self.reconnect_id:
            raise AssertionError("wrong outgoing message IDs")
        if self.delayed_first_at is None or self.delayed_repeat_at is None or \
                self.delayed_repeat_at - self.delayed_first_at < 5:
            raise AssertionError("lost PUBACK did not cross the 5 s retransmission window")
        publishes = [(index, detail) for _, index, kind, detail in self.events if kind == "publish"]
        expected = [
            (1, (TOPIC_PUBLISH, "delayed-ack", self.delayed_id, False)),
            (1, (TOPIC_PUBLISH, "delayed-ack", self.delayed_id, True)),
            (1, (TOPIC_PUBLISH, "reconnect", self.reconnect_id, False)),
            (2, (TOPIC_PUBLISH, "reconnect", self.reconnect_id, True)),
        ]
        if publishes != expected:
            raise AssertionError(f"wrong QoS1 packet sequence: {publishes}")
        if self.connections[0].subscriptions != {TOPIC_BASE} or \
                self.connections[1].subscriptions != {TOPIC_BASE}:
            raise AssertionError("reconnect did not restore the expected subscription")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, help="built IDF Linux .elf")
    args = parser.parse_args()
    broker = QoS1Broker()
    app = subprocess.Popen([args.app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1,
                           env=dict(os.environ, EMQTT_TEST_PORT=str(broker.port)))
    lines: queue.Queue[str] = queue.Queue()
    transcript: list[str] = []

    def read_app() -> None:
        assert app.stdout is not None
        for line in app.stdout:
            lines.put(line.rstrip())

    threading.Thread(target=read_app, daemon=True).start()
    deadline = time.monotonic() + 53
    try:
        while time.monotonic() < deadline:
            broker.step()
            while not lines.empty():
                line = lines.get_nowait()
                transcript.append(line)
                print("APP " + line, flush=True)
            if app.poll() is not None:
                break
        if app.poll() is None:
            raise TimeoutError("IDF Linux app did not exit")
        while not lines.empty():
            line = lines.get_nowait()
            transcript.append(line)
            print("APP " + line, flush=True)
        if app.returncode != 0 or not any(line.startswith("TEST PASS ") for line in transcript):
            raise AssertionError(f"IDF Linux app exited {app.returncode} without test PASS")
        broker.verify()
        print("BROKER TEST PASS scenario=qos1-ack: lost/double PUBACK, inbound duplicate, reconnect")
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
