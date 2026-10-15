#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "config.h"
#include "mqtt_link.h"

// MQTT 任务独占 Wi-Fi socket 和 MQTT client；主循环通过 FreeRTOS 队列交换数据。
namespace {
QueueHandle_t snapshots = nullptr, commands = nullptr, states = nullptr;
WiFiClient socketClient;
PubSubClient mqtt(socketClient);
char telemetryTopic[96], statusTopic[96], commandTopic[96], ackTopic[96];
char clientId[64], session[17];
Snapshot latest;
bool haveSnapshot = false;

void acknowledge(const char *message) { mqtt.publish(ackTopic, message, false); }
void onMessage(char *, uint8_t *payload, unsigned int length) {
  // 只接受完整的四项报警阈值，拒绝超长、格式错误和过期会话命令。
  if (length > 512) { acknowledge("{\"accepted\":false,\"reason\":\"too_large\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) || !doc.is<JsonObject>()) {
    acknowledge("{\"accepted\":false,\"reason\":\"invalid_json\"}"); return;
  }
  // 会话令牌每次重连更新；旧 retained 命令不能在重启/重连后自动执行。
  const char *token = doc["session"] | "";
  if (strcmp(token, session) != 0) {
    acknowledge("{\"accepted\":false,\"reason\":\"wrong_session\"}"); return;
  }
  if (doc.size() != 2 || !doc["thresholds"].is<JsonObject>()) {
    acknowledge("{\"accepted\":false,\"reason\":\"unsupported_command\"}"); return;
  }
  Command cmd;
  {
    JsonObject t = doc["thresholds"].as<JsonObject>();
    if (!t["humidity_on"].is<float>() || !t["humidity_off"].is<float>() ||
        !t["grain_on"].is<float>() || !t["grain_off"].is<float>()) {
      acknowledge("{\"accepted\":false,\"reason\":\"incomplete_thresholds\"}"); return;
    }
    cmd.thresholds = {t["humidity_on"].as<float>(), t["humidity_off"].as<float>(),
                      t["grain_on"].as<float>(), t["grain_off"].as<float>()};
    const auto &v = cmd.thresholds;
    if (!isfinite(v.humidityOn) || !isfinite(v.humidityOff) || !isfinite(v.grainOn) || !isfinite(v.grainOff) ||
        v.humidityOff < 0 || v.humidityOn > 100 || v.humidityOff >= v.humidityOn ||
        v.grainOff < -20 || v.grainOn > 60 || v.grainOff >= v.grainOn) {
      acknowledge("{\"accepted\":false,\"reason\":\"threshold_range\"}"); return;
    }
    cmd.hasThresholds = true;
  }
  // 回调不直接改主循环状态；排队成功仅代表接收，实际生效由后续遥测确认。
  if (xQueueSend(commands, &cmd, 0) != pdTRUE) acknowledge("{\"accepted\":false,\"reason\":\"queue_full\"}");
  else acknowledge("{\"accepted\":true,\"state\":\"queued\"}");
}
void numeric(JsonObject o, const char *key, float value, bool valid) {
  // 缺失/异常数据显式传 null，页面就不会把传感器故障显示为 0。
  if (valid && isfinite(value)) o[key] = value; else o[key] = nullptr;
}
void publishData() {
  if (!haveSnapshot) return;
  JsonDocument doc;
  doc["device"] = DEVICE_ID; doc["session"] = session;
  doc["uptime_ms"] = latest.uptimeMs;
  auto v = doc["sensors"].to<JsonObject>();
  const auto &m = latest.values;
  numeric(v, "air_c", m.airC, m.airValid); numeric(v, "humidity_pct", m.humidity, m.airValid);
  numeric(v, "grain_c", m.grainC, m.grainValid); numeric(v, "lux", m.lux, m.lightValid);
  numeric(v, "tvoc_ppb", m.tvoc, m.gasValid); numeric(v, "eco2_ppm", m.eco2, m.gasValid);
  numeric(v, "tvoc_baseline_ppb", m.tvocBaseline, m.gasValid);
  doc["gas_warming"] = m.gasWarming; doc["tvoc_trend"] = m.tvocTrend;
  auto d = doc["door"].to<JsonObject>();
  d["open"] = latest.door.open; d["open_s"] = latest.door.openSeconds;
  d["longest_s"] = latest.door.longestSeconds; d["count"] = latest.door.count; d["lit_count"] = latest.door.litCount;
  const auto &c = latest.control;
  doc["alarm"] = c.alarm; doc["sensor_fault_bits"] = c.sensorFaults;
  doc["humidity_high"] = c.humid; doc["grain_hot"] = c.hot;
  doc["grain_delta"] = c.delta; doc["door_timeout"] = c.doorAlarm;
  auto t = doc["thresholds"].to<JsonObject>();
  t["humidity_on"] = latest.thresholds.humidityOn; t["humidity_off"] = latest.thresholds.humidityOff;
  t["grain_on"] = latest.thresholds.grainOn; t["grain_off"] = latest.thresholds.grainOff;
  doc["rssi_dbm"] = WiFi.RSSI();
  char buffer[1536];
  if (measureJson(doc) >= sizeof(buffer)) { Serial.println("[MQTT] payload too large"); return; }
  serializeJson(doc, buffer, sizeof(buffer));
  // 实时数据不 retained，避免订阅者把很久以前的样本误认为当前数据。
  if (!mqtt.publish(telemetryTopic, buffer, false)) Serial.println("[MQTT] publish failed");
}
void networkTask(void *) {
  // 连接和重连可能等待 TCP 超时，因此放在独立任务，主循环不等待网络。
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  const bool wifiConfigured = strlen(WIFI_SSID) > 0;
  const bool mqttConfigured = strlen(MQTT_HOST) > 0;
  if (wifiConfigured) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  else Serial.println("[WiFi] empty SSID; local sensing remains active");
  if (!mqttConfigured) Serial.println("[MQTT] empty broker address; fill config_local.h");
  socketClient.setConnectionTimeout(1500);
  mqtt.setServer(MQTT_HOST, MQTT_PORT); mqtt.setCallback(onMessage);
  if (!mqtt.setBufferSize(2048)) { Serial.println("[MQTT] buffer allocation failed"); vTaskDelete(nullptr); return; }
  mqtt.setKeepAlive(20); mqtt.setSocketTimeout(2);
  uint32_t retryAt = millis() - 5000, wifiAt = millis(), sentAt = millis() - cfg::UPLOAD_MS;
  uint32_t stateAt = millis() - 1000;
  for (;;) {
    if (xQueueReceive(snapshots, &latest, 0) == pdTRUE) haveSnapshot = true;
    uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (!connected) {
      // 直接终止 TCP，让 Broker 遗嘱报告离线；不发送正常 DISCONNECT 吞掉遗嘱。
      if (socketClient.connected()) socketClient.stop();
      if (wifiConfigured && uint32_t(now - wifiAt) >= 15000) { wifiAt = now; WiFi.reconnect(); }
    } else if (mqttConfigured) {
      if (!mqtt.connected() && uint32_t(now - retryAt) >= 5000) {
        retryAt = now;
        snprintf(session, sizeof(session), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
        const char *user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
        const char *pass = user ? MQTT_PASSWORD : nullptr;
        // status 使用 retained 消息和遗嘱；异常断线时 Broker 可发布 offline。
        if (mqtt.connect(clientId, user, pass, statusTopic, 1, true, "offline")) {
          if (mqtt.subscribe(commandTopic, 1) && mqtt.publish(statusTopic, "online", true)) {
            Serial.printf("[MQTT] connected: %s, session=%s\n", telemetryTopic, session);
            sentAt = millis() - cfg::UPLOAD_MS;
          } else { mqtt.publish(statusTopic, "offline", true); mqtt.disconnect(); }
        } else Serial.printf("[MQTT] connect failed, state=%d\n", mqtt.state());
      }
      if (mqtt.connected()) {
        mqtt.loop(); now = millis();
        // 重连可能耗时，发布前再取最新快照。
        if (xQueueReceive(snapshots, &latest, 0) == pdTRUE) haveSnapshot = true;
        if (uint32_t(now - sentAt) >= cfg::UPLOAD_MS) { sentAt = now; publishData(); }
      }
    }
    now = millis();
    if (uint32_t(now - stateAt) >= 500) {
      stateAt = now;
      NetworkState state{WiFi.status() == WL_CONNECTED, mqtt.connected(), uint32_t(WiFi.localIP())};
      xQueueOverwrite(states, &state);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}
bool networkBegin() {
  // 快照和状态队列容量为 1：用最新值覆盖旧值；命令队列容量为 4。
  snapshots = xQueueCreate(1, sizeof(Snapshot)); commands = xQueueCreate(4, sizeof(Command)); states = xQueueCreate(1, sizeof(NetworkState));
  if (!snapshots || !commands || !states) return false;
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "%s-%08lx", DEVICE_ID, (unsigned long)(mac >> 16));
  snprintf(telemetryTopic, sizeof(telemetryTopic), "granary/%s/telemetry", DEVICE_ID);
  snprintf(statusTopic, sizeof(statusTopic), "granary/%s/status", DEVICE_ID);
  snprintf(commandTopic, sizeof(commandTopic), "granary/%s/command", DEVICE_ID);
  snprintf(ackTopic, sizeof(ackTopic), "granary/%s/ack", DEVICE_ID);
  return xTaskCreate(networkTask, "mqtt", 8192, nullptr, 1, nullptr) == pdPASS;
}
void networkSnapshot(const Snapshot &data) { if (snapshots) xQueueOverwrite(snapshots, &data); }
bool networkCommand(Command &command) { return commands && xQueueReceive(commands, &command, 0) == pdTRUE; }
void networkState(NetworkState &state) { if (states) xQueueReceive(states, &state, 0); }
