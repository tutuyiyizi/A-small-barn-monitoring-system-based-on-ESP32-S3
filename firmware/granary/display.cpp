#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "config.h"
#include "display.h"

// OLED 与环境传感器共用 I2C；本文件只在 Arduino 主循环中访问 Wire。
namespace {
Adafruit_SSD1306 screen(cfg::OLED_WIDTH, cfg::OLED_HEIGHT, &Wire, -1, 100000, 100000);
Adafruit_SSD1306 second(cfg::OLED_WIDTH, cfg::OLED_HEIGHT, &Wire, -1, 100000, 100000);
bool screenOk = false, secondOk = false;
uint32_t renderedAt = 0;
// 把“逻辑开启”转换为引脚电平：当前实物蜂鸣器为低电平触发。
void output(uint8_t pin, bool on, bool activeHigh) { digitalWrite(pin, on == activeHigh ? HIGH : LOW); }
bool startScreen(Adafruit_SSD1306 &s, uint8_t addr) {
  // 先查 I2C 应答，未接 OLED 时不让初始化结果误判为屏幕可用。
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() != 0) return false;
  return s.begin(SSD1306_SWITCHCAPVCC, addr, false, false);
}
void number(Adafruit_SSD1306 &s, float value) { if (isfinite(value)) s.print(value, 1); else s.print("--"); }
void render(Adafruit_SSD1306 &s, const Snapshot &d, const NetworkState &n, uint8_t page) {
  s.clearDisplay(); s.setTextColor(SSD1306_WHITE); s.setTextSize(1); s.setCursor(0, 0);
  if (page == 0) {
    s.print("Air: "); number(s, d.values.airC); s.println(" C");
    s.print("RH: "); number(s, d.values.humidity); s.println(" %");
    s.print("Grain: "); number(s, d.values.grainC); s.println(" C");
    s.print("Door: "); s.println(d.door.open ? "OPEN" : "CLOSED");
    s.print("RH high: "); s.println(d.control.humid ? "YES" : "NO");
    s.print("Alarm: "); s.println(d.control.alarm ? "YES" : "NO");
    s.print("Fault bits: "); s.println(d.control.sensorFaults);
  } else {
    s.print("TVOC: "); number(s, d.values.tvoc); s.println(" ppb");
    s.print("eCO2: "); number(s, d.values.eco2); s.println(" ppm");
    s.print("Lux: "); number(s, d.values.lux); s.println();
    s.print("Gas: "); s.println(d.values.gasValid ? "READY" : (d.values.gasOnline ? "WARM/ERR" : "OFFLINE"));
    s.print("WiFi/MQTT: "); s.print(n.wifi); s.print('/'); s.println(n.mqtt);
    IPAddress ip(n.ip); s.println(ip);
    s.print("Door count: "); s.println(d.door.count);
  }
  s.display();
}
}
void outputsBegin() {
  // 先设置关断电平，再切为输出，降低软件初始化阶段误动作风险。
  output(cfg::BUZZER_PIN, false, cfg::BUZZER_ACTIVE_HIGH);
  pinMode(cfg::BUZZER_PIN, OUTPUT);
  pinMode(cfg::GREEN_PIN, OUTPUT); pinMode(cfg::RED_PIN, OUTPUT);
  digitalWrite(cfg::GREEN_PIN, LOW); digitalWrite(cfg::RED_PIN, LOW);
}
void outputsTick(const ControlState &s, const NetworkState &n, uint32_t now) {
  // 蜂鸣器每 2 秒鸣 200 ms；红灯报警闪烁，绿灯 MQTT 在线常亮。
  output(cfg::BUZZER_PIN, s.alarm && now % 2000 < 200, cfg::BUZZER_ACTIVE_HIGH);
  digitalWrite(cfg::RED_PIN, s.alarm && now % 1000 < 500);
  digitalWrite(cfg::GREEN_PIN, n.mqtt || now % 2000 < 100);
}
void displayBegin() {
  if (cfg::OLED_ENABLED) screenOk = startScreen(screen, cfg::OLED_ADDRESS);
  if (cfg::SECOND_OLED_ENABLED) secondOk = startScreen(second, cfg::SECOND_OLED_ADDRESS);
  Serial.printf("[OLED] primary=%d second=%d\n", screenOk, secondOk);
}
void displayTick(const Snapshot &d, const NetworkState &n) {
  if (uint32_t(d.uptimeMs - renderedAt) < 2000) return;
  renderedAt = d.uptimeMs;
  if (screenOk) render(screen, d, n, secondOk ? 0 : (d.uptimeMs / 2000) % 2);
  if (secondOk) render(second, d, n, 1);
}
