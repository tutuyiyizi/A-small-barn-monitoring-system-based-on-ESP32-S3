"""谷仓本地接收端：MQTT → SQLite → 本机浏览器。Python 3.10+。"""
import argparse
import json
import math
import re
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent


def validate_telemetry(data):
    """校验设备遥测；传感器失效用 JSON null 表示，禁止 NaN/Infinity 入库。"""
    if not isinstance(data, dict) or not re.fullmatch(r"[0-9a-f]{16}", str(data.get("session", ""))):
        raise ValueError("invalid session")
    sensors = data.get("sensors")
    if not isinstance(sensors, dict):
        raise ValueError("missing sensors")
    for key in ("air_c", "humidity_pct", "grain_c", "lux", "tvoc_ppb", "eco2_ppm"):
        if key not in sensors:
            raise ValueError("missing sensor field")
        value = sensors[key]
        if value is not None and (type(value) not in (int, float) or not math.isfinite(value)):
            raise ValueError("invalid sensor value")
    # 禁止非标准 NaN/Infinity 进入数据库或 HTTP 响应。
    json.dumps(data, allow_nan=False)
    return data


def make_command(data, session):
    """将网页阈值表单转为设备命令，并注入当前 MQTT 会话令牌。"""
    if not isinstance(data, dict):
        raise ValueError("命令必须为对象")
    if set(data) - {"session", "thresholds"}:
        raise ValueError("不支持的命令字段")
    command = {"session": session}
    if "thresholds" in data:
        t = data["thresholds"]
        names = ("humidity_on", "humidity_off", "grain_on", "grain_off")
        if not isinstance(t, dict) or any(type(t.get(k)) not in (int, float) or not math.isfinite(t[k]) for k in names):
            raise ValueError("阈值必须是四个有限数值")
        if not (0 <= t["humidity_off"] < t["humidity_on"] <= 100 and -20 <= t["grain_off"] < t["grain_on"] <= 60):
            raise ValueError("阈值范围或回差无效")
        command["thresholds"] = {k: t[k] for k in names}
    if len(command) == 1:
        raise ValueError("空命令")
    return command


class Receiver:
    """接收 MQTT 数据、保存历史，并向仅在本机开放的 HTTP 页面提供状态。"""
    def __init__(self, config, database):
        self.config = config
        self.database = database
        self.lock = threading.Lock()
        self.latest = None
        self.received_at = 0
        self.status = "unknown"
        self.ack = None
        self.connected = False
        self.cleanup_at = 0
        self.base = f"granary/{config['device_id']}"
        with sqlite3.connect(database) as db:
            db.execute("CREATE TABLE IF NOT EXISTS samples (received REAL NOT NULL, payload TEXT NOT NULL)")
            db.execute("CREATE INDEX IF NOT EXISTS samples_time ON samples(received)")
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"dashboard-{time.time_ns():x}")
        if config.get("username"):
            self.client.username_pw_set(config["username"], config.get("password", ""))
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message
        self.client.reconnect_delay_set(1, 15)

    def on_connect(self, client, userdata, flags, reason, properties):
        with self.lock:
            self.connected = not reason.is_failure
            self.latest = None  # 重连后等待新样本，不能用旧会话控制。
        if not reason.is_failure:
            client.subscribe([(self.base + "/telemetry", 0), (self.base + "/status", 1), (self.base + "/ack", 1)])
        print(f"[MQTT] {reason}")

    def on_disconnect(self, client, userdata, flags, reason, properties):
        with self.lock:
            self.connected = False

    def on_message(self, client, userdata, message):
        # status 可 retained；telemetry/ack 必须是本次收到的新消息。
        if len(message.payload) > 8192:
            return
        try:
            if message.topic == self.base + "/status":
                with self.lock:
                    self.status = message.payload.decode("utf-8")
                return
            if message.retain:
                return  # 历史 retained 数据/确认消息不能当作实时消息。
            data = json.loads(message.payload)
            if message.topic == self.base + "/ack":
                with self.lock:
                    self.ack = data
                return
            if message.topic != self.base + "/telemetry":
                return
            validate_telemetry(data)
            now = time.time()
            with sqlite3.connect(self.database) as db:
                db.execute("INSERT INTO samples VALUES (?, ?)", (now, json.dumps(data, allow_nan=False)))
                # 每小时清理一次 24 小时前的数据，避免每条样本都执行 DELETE。
                if now - self.cleanup_at > 3600:
                    db.execute("DELETE FROM samples WHERE received < ?", (now - 86400,))
                    self.cleanup_at = now
            with self.lock:
                self.latest, self.received_at = data, now
        except (ValueError, UnicodeError, TypeError, sqlite3.Error) as exc:
            print(f"[MQTT] 拒绝样本/存储失败: {exc}")

    def current(self):
        """只有 Broker 连接正常、样本不足 15 秒且状态未离线，才允许下发命令。"""
        with self.lock:
            age = time.time() - self.received_at if self.latest else None
            online = bool(self.connected and self.latest and age < 15 and self.status != "offline")
            return {"mqtt_connected": self.connected, "online": online, "age_s": age,
                    "data": self.latest, "ack": self.ack}

    def history(self):
        """查询最近一小时，按时间正序给前端绘图，最多返回 720 条。"""
        with sqlite3.connect(self.database) as db:
            rows = db.execute("SELECT received, payload FROM samples WHERE received > ? ORDER BY received DESC LIMIT 720",
                              (time.time() - 3600,)).fetchall()
        return [{"time": t, **json.loads(payload)["sensors"]} for t, payload in reversed(rows)]

    def command(self, data):
        """网页提交阈值后发布非 retained 命令；设备随后通过遥测报告实际状态。"""
        current = self.current()
        if not current["online"]:
            raise ValueError("设备离线或数据超过15秒未更新")
        command = make_command(data, current["data"]["session"])
        result = self.client.publish(self.base + "/command", json.dumps(command), qos=1, retain=False)
        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            raise ValueError("MQTT 发送失败")
        return {"sent": True}


def handler_for(receiver):
    """生成本机 HTTP 处理器；页面只公开当前读数、历史和阈值接口。"""
    class Handler(BaseHTTPRequestHandler):
        def respond(self, value, code=200):
            body = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def valid_host(self):
            # 限定本机 Host，配合 loopback 监听及 POST Origin 检查。
            port = self.server.server_address[1]
            return self.headers.get("Host") in (f"127.0.0.1:{port}", f"localhost:{port}")

        def do_GET(self):
            if not self.valid_host():
                self.respond({"error": "invalid host"}, 403)
            elif self.path == "/api/current":
                self.respond(receiver.current())
            elif self.path == "/api/history":
                self.respond(receiver.history())
            elif self.path == "/":
                body = (ROOT / "index.html").read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.respond({"error": "not found"}, 404)

        def do_POST(self):
            if self.path != "/api/command":
                self.respond({"error": "not found"}, 404)
                return
            # 命令接口仅本机同源页面可用，防止跨站请求修改报警阈值。
            origin = self.headers.get("Origin")
            if not self.valid_host() or origin != "http://" + self.headers.get("Host", "") or self.headers.get("Content-Type") != "application/json":
                self.respond({"error": "仅接受本机页面请求"}, 403)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 1024:
                    raise ValueError("请求大小无效")
                self.respond(receiver.command(json.loads(self.rfile.read(length))))
            except (ValueError, UnicodeError) as exc:
                self.respond({"error": str(exc)}, 400)

        def log_message(self, fmt, *args):
            pass
    return Handler


def main():
    """读取私有配置并启动 MQTT 客户端和本机监测页面。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=ROOT / "config.json")
    args = parser.parse_args()
    if not args.config.exists():
        parser.error("请复制 config.example.json 为 config.json，并填写本地 Broker 参数")
    config = json.loads(args.config.read_text(encoding="utf-8-sig"))
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", config.get("device_id", "")):
        parser.error("device_id 只能包含1~32位英文字母、数字、下划线和连字符")
    receiver = Receiver(config, ROOT / "granary.sqlite3")
    receiver.client.connect_async(config["broker"], int(config.get("port", 1883)), keepalive=20)
    receiver.client.loop_start()
    server = ThreadingHTTPServer(("127.0.0.1", int(config.get("http_port", 8080))), handler_for(receiver))
    print(f"页面：http://127.0.0.1:{server.server_port}  | Ctrl+C 退出")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        receiver.client.disconnect()
        receiver.client.loop_stop()


if __name__ == "__main__":
    main()
