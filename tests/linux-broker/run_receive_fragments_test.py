#!/usr/bin/env python3
"""Actual core: delayed header/body, progressing packet and idle expiry."""
import argparse
import os
import queue
import subprocess
import threading
import time

from run_broker_test import Broker, packet, text_at


class FragmentBroker(Broker):
    def __init__(self):
        super().__init__("receive-fragments")
        self.schedule = []
        self.acked = []
        self.stall_started = None
        self.stall_closed = None
        self.heartbeats = 0
        self.fragment_times = {}

    def start_packet(self, conn, number):
        payload = {201: b'\x01' * 4096, 202: b'\x02' * 128, 203: b'\x03' * 4096,
                   204: b'not-delivered', 205: b'\x04' * 128}[number]
        body = b'\x00\x09test/base' + number.to_bytes(2, 'big') + payload
        raw = packet(0x32, body)
        now = time.monotonic()
        if number == 201:
            pieces = [(.1, raw[:600]), (.35, raw[600:])]
        elif number == 202:
            pieces = [(.1, raw[:1]), (.35, raw[1:2]), (.6, raw[2:])]
        elif number == 203:
            pieces = [(.1, raw[:400]), (1.3, raw[400:600]), (2.5, raw[600:800]), (3.7, raw[800:])]
        elif number == 204:
            pieces = [(.1, raw[:1])]
        else:
            pieces = [(.1, raw[:1]), (.35, raw[1:])]
        self.schedule = [(now + delay, number, part) for delay, part in pieces]

    def maybe_timers(self):
        now = time.monotonic()
        while self.schedule and now >= self.schedule[0][0]:
            _, number, data = self.schedule.pop(0)
            assert self.current is not None
            self.current.sock.sendall(data)
            self.fragment_times.setdefault(number, []).append(time.monotonic())
            self.note(self.current, 'fragment', (number, len(data)))
            if number == 204:
                self.stall_started = now

    def close_current(self):
        if self.current is not None and self.current.index == 1 and self.stall_started is not None:
            self.stall_closed = time.monotonic()
        self.schedule.clear()
        super().close_current()

    def handle(self, conn, header, body):
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            client, _ = text_at(body, at + 4)
            assert name == 'MQTT' and body[at] == 4 and body[at + 1] & 2
            assert client == 'linux-receive-fragments' and conn.index <= 2
            self.send(conn, 0x20, b'\x00\x00')
        elif kind == 8:
            topic, at = text_at(body, 2)
            assert header == 0x82 and topic == 'test/base' and body[at:] == b'\x01'
            self.send(conn, 0x90, body[:2] + b'\x01')
            self.start_packet(conn, 201 if conn.index == 1 else 205)
        elif kind == 3:
            topic, at = text_at(body, 0)
            assert header == 0x30 and topic == 'test/heartbeat' and body[at:] == b'live'
            self.heartbeats += 1
        elif kind == 4:
            number = int.from_bytes(body, 'big')
            assert header == 0x40 and len(body) == 2 and number in [201, 202, 203, 205]
            self.acked.append(number)
            self.note(conn, 'puback', number)
            if number < 204:
                self.start_packet(conn, number + 1)
        elif kind == 14:
            self.close_current()
        else:
            raise AssertionError('unexpected packet kind ' + str(kind))

    def verify(self):
        assert len(self.connections) == 2 and self.acked == [201, 202, 203, 205]
        assert self.stall_started is not None and self.stall_closed is not None
        elapsed = self.stall_closed - self.stall_started
        assert 2.9 <= elapsed <= 4.0, ('wrong idle expiry', elapsed)
        assert self.heartbeats >= 10
        progress_times = self.fragment_times[203]
        assert len(progress_times) == 4 and progress_times[-1] - progress_times[0] > 3.0
        assert all(1.0 < right - left < 2.0 for left, right in zip(progress_times, progress_times[1:]))
        print('BROKER TEST PASS scenario=receive-fragments idle_seconds=%.3f heartbeats=%d' % (elapsed, self.heartbeats))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True)
    args = parser.parse_args()
    broker = FragmentBroker()
    app = subprocess.Popen([args.app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           env=dict(os.environ, EMQTT_TEST_PORT=str(broker.port)))
    lines = queue.Queue()
    transcript = []
    def read_app():
        for line in app.stdout:
            lines.put(line.rstrip())
    reader = threading.Thread(target=read_app, daemon=True)
    reader.start()
    deadline = time.monotonic() + 28
    try:
        while time.monotonic() < deadline:
            broker.step()
            while not lines.empty():
                line = lines.get_nowait(); transcript.append(line); print('APP ' + line, flush=True)
            if app.poll() is not None:
                break
        assert app.poll() is not None, 'test app deadline'
        reader.join(timeout=1)
        assert not reader.is_alive(), 'app output did not close'
        while not lines.empty():
            line = lines.get_nowait(); transcript.append(line); print('APP ' + line, flush=True)
        assert app.returncode == 0 and any(line.startswith('TEST PASS receive-fragments') for line in transcript)
        broker.verify()
        return 0
    except (AssertionError, OSError, TimeoutError, ValueError) as error:
        print('BROKER TEST FAIL', type(error).__name__, str(error), flush=True)
        return 1
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=3)
            except subprocess.TimeoutExpired:
                app.kill(); app.wait()
        broker.close_current(); broker.listener.close()


if __name__ == '__main__':
    raise SystemExit(main())
