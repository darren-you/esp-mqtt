#!/usr/bin/env python3
"""Real MQTT core: bounded runtime queue and partial-packet receive gate."""
import argparse
import json
import os
import queue
import re
import subprocess
import threading
import time

from run_broker_test import Broker, packet, text_at


def message(number):
    value = number - (200 if number < 210 else 210) if number < 300 else number - 300
    return packet(0x32, b'\x00\x09test/base' + number.to_bytes(2, 'big') + bytes([value]) * 4096)


class BackpressureBroker(Broker):
    def __init__(self, mode):
        super().__init__('receive-backpressure')
        self.mode = mode
        self.schedule = []
        self.acks = []
        self.phases = []
        self.pings = []

    def maybe_timers(self):
        while self.schedule and time.monotonic() >= self.schedule[0][0]:
            _, data = self.schedule.pop(0)
            assert self.current is not None
            self.current.sock.sendall(data)

    def handle(self, conn, header, body):
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            client, _ = text_at(body, at + 4)
            assert name == 'MQTT' and body[at] == 4 and body[at + 1] & 2
            assert client == ('linux-receive-backpressure' if self.mode == 'runtime' else 'linux-receive-gate-partial')
            assert conn.index <= (1 if self.mode == 'runtime' else 2)
            self.send(conn, 0x20, b'\x00\x00')
        elif kind == 8:
            topic, at = text_at(body, 2)
            assert header == 0x82 and topic == 'test/base' and body[at:] == b'\x01'
            self.send(conn, 0x90, body[:2] + b'\x01')
            if self.mode != 'runtime':
                now = time.monotonic()
                if conn.index == 1:
                    raw = message(301); cutoff = int(self.mode)
                    self.schedule = [(now + .1, raw[:cutoff]), (now + .35, raw[cutoff:] + message(302))]
                else:
                    self.schedule = [(now + .1, message(303))]
        elif kind == 3:
            topic, at = text_at(body, 0)
            qos = (header >> 1) & 3
            mid = int.from_bytes(body[at:at + 2], 'big') if qos else None
            at += 2 if qos else 0
            phase = body[at:].decode()
            assert topic == 'test/phase' and not header & 9
            assert phase in (('burst', 'held', 'stop') if self.mode == 'runtime' else ('paused',))
            assert qos == (1 if phase == 'held' else 0)
            # Match the C test's clock, not Python's platform-dependent monotonic epoch.
            self.phases.append((phase, time.clock_gettime(time.CLOCK_MONOTONIC)))
            self.note(conn, 'phase', phase)
            if qos: self.send(conn, 0x40, mid.to_bytes(2, 'big'))
            if phase in ('burst', 'stop'):
                start = 201 if phase == 'burst' else 211
                conn.sock.sendall(b''.join(message(number) for number in range(start, start + 4)))
        elif kind == 4:
            assert header == 0x40 and len(body) == 2
            mid = int.from_bytes(body, 'big')
            self.acks.append((mid, time.clock_gettime(time.CLOCK_MONOTONIC)))
            self.note(conn, 'puback', mid)
        elif kind == 12:
            self.pings.append(time.clock_gettime(time.CLOCK_MONOTONIC))
            self.send(conn, 0xd0, b'')
        elif kind == 14:
            assert header == 0xe0 and not body
            self.close_current()
        else:
            raise AssertionError('unexpected packet kind ' + str(kind))

    def verify(self, transcript):
        assert not self.schedule
        if self.mode == 'runtime':
            assert len(self.connections) == 1
            assert [mid for mid, _ in self.acks] == [201, 202, 203, 204, 211, 212, 213]
            drain = [int(re.fullmatch(r'TEST DRAIN at_us=(\d+)', line)[1]) / 1e6
                     for line in transcript if line.startswith('TEST DRAIN ')]
            assert len(drain) == 1
            assert all(stamp < drain[0] for mid, stamp in self.acks if mid in (201, 202, 203))
            assert next(stamp for mid, stamp in self.acks if mid == 204) >= drain[0]
            assert [name for name, _ in self.phases] == ['burst', 'held', 'stop']
            assert self.phases[1][1] < drain[0], 'outbound did not progress while paused'
            assert drain[0] - self.phases[0][1] >= 1.0, 'no actual queue hold window'
        else:
            assert len(self.connections) == 2 and [mid for mid, _ in self.acks] == [301, 302, 303]
            release = [re.fullmatch(r'TEST RAW_RELEASE at_us=(\d+) cutoff=(\d+) actual_read=(\d+)', line)
                       for line in transcript if line.startswith('TEST RAW_RELEASE ')]
            assert len(release) == 1 and release[0] is not None
            assert int(release[0][2]) == int(self.mode)
            assert int(self.mode) <= int(release[0][3]) < len(message(301))
            stamp = int(release[0][1]) / 1e6
            assert self.acks[0][1] < stamp <= self.acks[1][1], ('gate ACK times', self.acks, stamp)
            assert self.pings and self.acks[0][1] < self.pings[0] < stamp, 'paused keepalive did not run'
            assert len(self.phases) == 1 and self.phases[0][0] == 'paused' and self.phases[0][1] < stamp
        print('BROKER TIMELINE ' + json.dumps({'mode': self.mode, 'clock': 'CLOCK_MONOTONIC',
            'pubacks': self.acks, 'outbound_phases': self.phases, 'pingreqs': self.pings}), flush=True)
        print('BROKER TEST PASS scenario=receive-backpressure mode=' + self.mode, flush=True)


def run(app_path, mode):
    broker = BackpressureBroker(mode)
    app = subprocess.Popen([app_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        env=dict(os.environ, EMQTT_TEST_PORT=str(broker.port), EMQTT_GATE_MODE=mode))
    lines = queue.Queue(); transcript = []
    def read_app():
        for line in app.stdout: lines.put(line.rstrip())
    reader = threading.Thread(target=read_app, daemon=True); reader.start()
    deadline = time.monotonic() + 20
    try:
        while time.monotonic() < deadline:
            broker.step()
            while not lines.empty():
                line = lines.get_nowait(); transcript.append(line); print('APP ' + line, flush=True)
            if app.poll() is not None: break
        assert app.poll() is not None, 'test app deadline'
        reader.join(timeout=1); assert not reader.is_alive(), 'app output did not close'
        while not lines.empty():
            line = lines.get_nowait(); transcript.append(line); print('APP ' + line, flush=True)
        assert app.returncode == 0 and any(line.startswith('TEST PASS receive-backpressure') for line in transcript)
        broker.verify(transcript)
    finally:
        if app.poll() is None:
            app.terminate()
            try: app.wait(timeout=3)
            except subprocess.TimeoutExpired: app.kill(); app.wait()
        broker.close_current(); broker.listener.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True)
    args = parser.parse_args()
    try:
        for mode in ('runtime', '1', '2', '600'): run(args.app, mode)
        return 0
    except (AssertionError, OSError, TimeoutError, ValueError) as error:
        print('BROKER TEST FAIL', type(error).__name__, str(error), flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
