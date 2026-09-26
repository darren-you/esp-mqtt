#!/usr/bin/env python3
"""用真实 ESP-MQTT Linux 核心验证本机严格 TLS、订阅和 QoS1。"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import queue
import select
import socket
import ssl
import subprocess
import tempfile
import threading
import time

from run_broker_test import packet, take_packet, text_at


def certificates(directory: Path) -> tuple[Path, Path, Path, Path]:
    def openssl(*args: str) -> None:
        subprocess.run(["openssl", *args], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    ca = directory / "ca.crt"
    ca_key = directory / "ca.key"
    server = directory / "server.crt"
    server_key = directory / "server.key"
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
            "-keyout", str(ca_key), "-out", str(ca), "-subj", "/CN=ESP MQTT TLS test CA")
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", str(server_key),
            "-out", str(directory / "server.csr"), "-subj", "/CN=127.0.0.1")
    (directory / "server.ext").write_text("subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n")
    openssl("x509", "-req", "-in", str(directory / "server.csr"), "-CA", str(ca),
            "-CAkey", str(ca_key), "-CAcreateserial", "-out", str(server),
            "-days", "1", "-sha256", "-extfile", str(directory / "server.ext"))
    wrong_ca = directory / "wrong-ca.crt"
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
            "-keyout", str(directory / "wrong-ca.key"), "-out", str(wrong_ca),
            "-subj", "/CN=Unrelated MQTT test CA")
    return ca, wrong_ca, server, server_key


class Broker:
    def __init__(self, certificate: Path, key: Path) -> None:
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.context.load_cert_chain(str(certificate), str(key))
        ipv4 = socket.create_server(("127.0.0.1", 0))
        self.port = ipv4.getsockname()[1]
        ipv6 = socket.create_server(("::1", self.port), family=socket.AF_INET6)
        self.listeners = [ipv4, ipv6]
        for listener in self.listeners:
            listener.setblocking(False)
        self.client: ssl.SSLSocket | None = None
        self.buffer = bytearray()
        self.events: list[str] = []

    def close(self) -> None:
        if self.client is not None:
            self.client.close()
            self.client = None
        for listener in self.listeners:
            listener.close()

    def handle(self, header: int, body: bytes) -> None:
        assert self.client is not None
        kind = header >> 4
        if kind == 1:
            name, at = text_at(body, 0)
            client_id, _ = text_at(body, at + 4)
            if name != "MQTT" or body[at] != 4 or not body[at + 1] & 2 or \
                    client_id != "linux-tls-real-core":
                raise AssertionError("CONNECT 身份或 clean session 不符")
            self.events.append("connect")
            self.client.sendall(packet(0x20, b"\x00\x00"))
        elif kind == 8:
            if header != 0x82:
                raise AssertionError("SUBSCRIBE flags 不符")
            topic, at = text_at(body, 2)
            if topic != "test/tls/in" or body[at:] != b"\x01":
                raise AssertionError("订阅 Topic/QoS 不符")
            self.events.append("subscribe")
            self.client.sendall(packet(0x90, body[:2] + b"\x01"))
        elif kind == 3:
            topic, at = text_at(body, 0)
            message_id = body[at:at + 2]
            if topic != "test/tls/out" or ((header >> 1) & 3) != 1 or header & 8 or \
                    len(message_id) != 2 or body[at + 2:] != b"tls-proof":
                raise AssertionError("TLS QoS1 PUBLISH 内容不符")
            self.events.append("publish")
            self.client.sendall(packet(0x40, message_id))
        elif kind == 12:
            self.client.sendall(packet(0xD0, b""))
        elif kind == 14:
            self.client.close()
            self.client = None
        else:
            raise AssertionError(f"意外的 MQTT 控制包：{kind}")

    def step(self) -> None:
        if self.client is None:
            ready, _, _ = select.select(self.listeners, [], [], 0.03)
            if not ready:
                return
            raw, _ = ready[0].accept()
            raw.settimeout(2)
            try:
                self.client = self.context.wrap_socket(raw, server_side=True)
                self.client.setblocking(False)
            except (OSError, ssl.SSLError):
                raw.close()
            return
        ready, _, _ = select.select([self.client], [], [], 0.03)
        if not ready and not self.client.pending():
            return
        try:
            data = self.client.recv(8192)
        except (ssl.SSLWantReadError, ssl.SSLWantWriteError):
            return
        except (OSError, ssl.SSLError):
            data = b""
        if not data:
            self.client.close()
            self.client = None
            self.buffer.clear()
            return
        self.buffer.extend(data)
        while parsed := take_packet(self.buffer):
            self.handle(*parsed)
            if self.client is None:
                break


def run_case(app_path: str, ca: Path, server: Path, key: Path,
             *, label: str, hostname: str, expected: str) -> None:
    broker = Broker(server, key)
    app = subprocess.Popen([app_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1,
                           env=dict(os.environ, EMQTT_TEST_PORT=str(broker.port),
                                    EMQTT_TEST_HOST=hostname, EMQTT_TEST_CA_FILE=str(ca),
                                    EMQTT_TEST_MODE=expected))
    lines: queue.Queue[str] = queue.Queue()
    transcript: list[str] = []

    def read_app() -> None:
        assert app.stdout is not None
        for line in app.stdout:
            lines.put(line.rstrip())

    reader = threading.Thread(target=read_app, daemon=True)
    reader.start()
    deadline = time.monotonic() + 20
    try:
        while time.monotonic() < deadline:
            broker.step()
            while not lines.empty():
                line = lines.get_nowait()
                transcript.append(line)
                print(f"APP {label} {line}", flush=True)
            if app.poll() is not None:
                break
        if app.poll() is None:
            raise TimeoutError("Linux MQTT TLS 应用未在期限内退出")
        reader.join(timeout=1)
        while not lines.empty():
            line = lines.get_nowait()
            transcript.append(line)
            print(f"APP {label} {line}", flush=True)
        if app.returncode != 0 or not any(line.startswith("TEST PASS ") for line in transcript):
            raise AssertionError(f"{label} 应用退出码={app.returncode}，缺少 PASS")
        wanted = ["connect", "subscribe", "publish"] if expected == "valid" else []
        if broker.events != wanted:
            raise AssertionError(f"{label} Broker 报文顺序不符：{broker.events}")
        print(f"BROKER TLS PASS case={label} events={broker.events}", flush=True)
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=3)
            except subprocess.TimeoutExpired:
                app.kill()
        broker.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True)
    args = parser.parse_args()
    current_umask = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(prefix="emqtt-linux-tls-") as name:
            directory = Path(name)
            ca, wrong_ca, server, key = certificates(directory)
            run_case(args.app, ca, server, key, label="valid", hostname="127.0.0.1", expected="valid")
            run_case(args.app, wrong_ca, server, key, label="wrong-ca", hostname="127.0.0.1", expected="reject")
            run_case(args.app, ca, server, key, label="wrong-host", hostname="localhost", expected="reject")
    finally:
        os.umask(current_umask)
    print("BROKER TEST PASS scenario=tls: real core, trusted time, CA, hostname, QoS1", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
