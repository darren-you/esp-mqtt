import contextlib
import io
import unittest

from serial_cycles import run


class FakePort:
    def __init__(self, online_error=0, publish_ack=True):
        self.cycle = 0
        self.lines = []
        self.online_error = online_error
        self.publish_ack = publish_ack

    def write(self, data):
        command = data.decode().strip()
        if command == "stats":
            self.lines.append(
                f"EMQTT_SAMPLE cycle={self.cycle} phase=requested state=3 wifi=1 "
                "trusted=1 outbox=0 heap_free=9000 heap_min=8000 "
                "heap_largest=6000 time_ms=123\n".encode())
        elif command == "cycle":
            self.cycle += 1
            self.lines.extend((
                f"EMQTT_SAMPLE command=cycle error=0 cycle={self.cycle}\n".encode(),
                b"EMQTT_SAMPLE_EVENT kind=1 error=0\n",
                f"EMQTT_SAMPLE online_error={self.online_error} message_id=17\n".encode(),
            ))
            if self.publish_ack:
                self.lines.append(b"EMQTT_SAMPLE_EVENT kind=4 message_id=17\n")

    def flush(self):
        pass

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


class SerialCycleTest(unittest.TestCase):
    def test_two_rounds_emit_quiescent_snapshots(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            run(FakePort(), 2, 0.2)
        self.assertEqual(output.getvalue().splitlines(), [
            "cycle,heap_free,heap_min,heap_largest,outbox,time_ms",
            "1,9000,8000,6000,0,123",
            "2,9000,8000,6000,0,123",
        ])

    def test_online_enqueue_error_stops_the_matrix(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "在线状态入队失败"):
                run(FakePort(online_error=1), 1, 0.2)

    def test_missing_puback_does_not_report_success(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(TimeoutError):
                run(FakePort(publish_ack=False), 1, 0.01)


if __name__ == "__main__":
    unittest.main()
