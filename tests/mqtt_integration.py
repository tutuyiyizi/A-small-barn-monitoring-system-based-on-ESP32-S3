"""仅使用本机18883端口和合成数据，退出时关闭自建 Broker，不连接实物。"""
import argparse
import json
import math
import queue
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local_server"))
from server import Receiver, handler_for
from test_server import sample
import paho.mqtt.client as mqtt


def wait_until(predicate, message, timeout=10):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--broker-exe", type=Path, required=True)
    parser.add_argument("--preview", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="granary-test-") as folder:
        folder = Path(folder)
        conf = folder / "broker.conf"
        conf.write_text("listener 18883 127.0.0.1\nallow_anonymous true\npersistence false\n", encoding="ascii")
        broker = subprocess.Popen([str(args.broker_exe.resolve()), "-c", str(conf)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
        receiver = Receiver({"device_id": "test-granary"}, folder / "samples.sqlite3")
        http = ThreadingHTTPServer(("127.0.0.1", 18080), handler_for(receiver))
        threading.Thread(target=http.serve_forever, daemon=True).start()
        device = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="synthetic-test-device")
        received = queue.Queue()
        device.on_message = lambda client, userdata, msg: received.put(json.loads(msg.payload))
        base = "granary/test-granary"
        try:
            time.sleep(.5)
            assert broker.poll() is None, "test broker failed to start"
            receiver.client.connect_async("127.0.0.1", 18883)
            receiver.client.loop_start()
            device.connect("127.0.0.1", 18883); device.loop_start()
            device.subscribe(base + "/command", qos=1)
            wait_until(lambda: receiver.connected, "receiver not connected")
            time.sleep(.3)
            device.publish(base + "/status", "online", retain=True).wait_for_publish()
            device.publish(base + "/telemetry", json.dumps(sample())).wait_for_publish()
            wait_until(lambda: receiver.current()["online"], "sample not received")
            with urlopen("http://127.0.0.1:18080/api/current") as r:
                assert json.load(r)["data"]["sensors"]["grain_c"] == 27.1
            assert len(receiver.history()) == 1
            request = Request("http://127.0.0.1:18080/api/command", data=json.dumps({"thresholds": sample()["thresholds"]}).encode(),
                              headers={"Content-Type": "application/json", "Origin": "http://127.0.0.1:18080"})
            with urlopen(request) as r: assert json.load(r)["sent"]
            command = received.get(timeout=5)
            assert command == {"session": "0123456789abcdef", "thresholds": sample()["thresholds"]}
            bad = Request("http://127.0.0.1:18080/api/command", data=json.dumps({"thresholds": sample()["thresholds"]}).encode(),
                          headers={"Content-Type": "application/json", "Origin": "http://example.com"})
            try:
                urlopen(bad); raise AssertionError("cross-origin request accepted")
            except HTTPError as exc: assert exc.code == 403
            legacy = Request("http://127.0.0.1:18080/api/command", data=b'{"fan_mode":"on"}',
                             headers={"Content-Type": "application/json", "Origin": "http://127.0.0.1:18080"})
            try:
                urlopen(legacy); raise AssertionError("removed command accepted")
            except HTTPError as exc: assert exc.code == 400
            device.publish(base + "/status", "offline", retain=True).wait_for_publish()
            wait_until(lambda: not receiver.current()["online"], "offline state not applied")
            print("PASS: real Mosquitto -> MQTT receiver -> SQLite -> HTTP; command session; cross-origin rejection; offline state", flush=True)
            if args.preview:
                device.publish(base + "/status", "online", retain=True)
                print("SYNTHETIC DATA preview: http://127.0.0.1:18080", flush=True)
                i = 0
                while True:
                    payload = sample()
                    payload["sensors"]["air_c"] += math.sin(i / 5) * .5
                    device.publish(base + "/telemetry", json.dumps(payload))
                    i += 1
                    time.sleep(2)
        finally:
            device.disconnect(); device.loop_stop()
            receiver.client.disconnect(); receiver.client.loop_stop()
            http.shutdown(); http.server_close()
            broker.terminate(); broker.wait(timeout=5)


if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: pass
