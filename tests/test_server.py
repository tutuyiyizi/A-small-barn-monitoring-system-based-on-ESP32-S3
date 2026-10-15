import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "local_server"))
from server import Receiver, make_command, validate_telemetry


def sample():
    return {"device": "test-granary", "session": "0123456789abcdef", "uptime_ms": 45000,
            "sensors": {"air_c": 25.6, "humidity_pct": 58.3, "grain_c": 27.1,
                        "lux": 12.0, "tvoc_ppb": 126, "eco2_ppm": 630},
            "alarm": False, "sensor_fault_bits": 0,
            "gas_warming": False, "door": {"open": False, "count": 2, "lit_count": 1, "open_s": 0, "longest_s": 12},
            "thresholds": {"humidity_on": 65, "humidity_off": 60, "grain_on": 30, "grain_off": 28}}


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.receiver = Receiver({"device_id": "test-granary"}, Path(self.temp.name) / "test.sqlite3")
        self.receiver.connected = True

    def tearDown(self):
        self.temp.cleanup()

    def message(self, payload, retain=False, suffix="telemetry"):
        return SimpleNamespace(topic="granary/test-granary/" + suffix,
                               payload=json.dumps(payload).encode(), retain=retain)

    def test_store_and_history(self):
        self.receiver.on_message(None, None, self.message(sample()))
        self.assertTrue(self.receiver.current()["online"])
        self.assertEqual(self.receiver.history()[0]["air_c"], 25.6)

    def test_retain_and_malformed_rejected(self):
        self.receiver.on_message(None, None, self.message(sample(), True))
        self.assertIsNone(self.receiver.current()["data"])
        bad = sample(); bad["sensors"]["air_c"] = float("nan")
        self.receiver.on_message(None, None, self.message(bad))
        self.assertEqual(self.receiver.history(), [])

    def test_null_valid_and_boolean_invalid(self):
        data = sample(); data["sensors"]["grain_c"] = None
        validate_telemetry(data)
        data["sensors"]["grain_c"] = True
        with self.assertRaises(ValueError): validate_telemetry(data)

    def test_stale_or_offline_blocks_commands(self):
        self.receiver.on_message(None, None, self.message(sample()))
        self.receiver.received_at = time.time() - 16
        with self.assertRaises(ValueError): self.receiver.command({"thresholds": sample()["thresholds"]})
        self.receiver.received_at = time.time(); self.receiver.status = "offline"
        self.assertFalse(self.receiver.current()["online"])

    def test_thresholds_and_session(self):
        result = make_command({"thresholds": sample()["thresholds"], "session": "attacker"}, "newsession")
        self.assertEqual(result["session"], "newsession")
        self.assertEqual(result["thresholds"], sample()["thresholds"])
        for data in ({"fan_mode": "on"}, {}, {"thresholds": {"humidity_on": 40}},
                     {"thresholds": {"humidity_on": 50, "humidity_off": 60, "grain_on": 30, "grain_off": 28}}):
            with self.assertRaises(ValueError): make_command(data, "token")


if __name__ == "__main__":
    unittest.main()
