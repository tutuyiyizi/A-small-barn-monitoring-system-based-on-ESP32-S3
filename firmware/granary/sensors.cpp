#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_SGP30.h>
#include <BH1750.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"
#include "sensors.h"

// 各传感器按独立周期轮询；初始化失败后定时重试，不让缺失的模块阻塞其他功能。
namespace {
Adafruit_SHT31 sht;
Adafruit_SGP30 gas;
BH1750 light;
OneWire oneWire(cfg::GRAIN_PIN);
DallasTemperature grain(&oneWire);
DeviceAddress grainAddress;
bool shtReady = false, gasReady = false, lightReady = false, grainReady = false;
bool conversionPending = false;
uint32_t retryAt = 0, airAt = 0, lightAt = 0, gasAt = 0, grainAt = 0, conversionAt = 0, gasStarted = 0;
uint8_t gasErrors = 0;
// 保存最近 600 个有效的 1 秒 TVOC 样本，用于与此前 10 分钟均值比较。
uint16_t tvocWindow[600] = {};
uint16_t tvocIndex = 0, tvocCount = 0;
uint32_t tvocSum = 0;
bool rawOpen = false, stableOpen = false, litRecorded = false;
uint32_t rawSince = 0, openedAt = 0;
float closedLux = NAN, eventBaseline = NAN;

// MC-38 本身是无源磁簧开关；默认关门时接地为 LOW，开门后上拉为 HIGH。
bool readDoor() { return digitalRead(cfg::DOOR_PIN) == (cfg::DOOR_CLOSED_LOW ? HIGH : LOW); }
void resetTrend() { tvocIndex = tvocCount = 0; tvocSum = 0; }
void discover(uint32_t now) {
  if (!shtReady) shtReady = sht.begin(cfg::SHT_ADDRESS);
  if (!lightReady) lightReady = light.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, cfg::LIGHT_ADDRESS, &Wire);
  if (!gasReady) {
    gasReady = gas.begin(&Wire);
    if (gasReady) { gasStarted = now; gasErrors = 0; resetTrend(); }
  }
  if (!grainReady && !conversionPending) {
    grain.begin(); grain.setWaitForConversion(false);
    grainReady = grain.getAddress(grainAddress, 0);
    if (grainReady) grain.setResolution(grainAddress, 12);
  }
}
}

void sensorsBegin() {
  Wire.begin(cfg::SDA_PIN, cfg::SCL_PIN, 100000);
  Wire.setTimeOut(30);
  Serial.println("[I2C] scan start");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) Serial.printf("[I2C] found 0x%02X\n", address);
  }
  discover(millis());
  airAt = grainAt = millis() - cfg::SAMPLE_MS;
  lightAt = millis() - 2000;
  gasAt = retryAt = millis();
}

void sensorsTick(uint32_t now, Measurements &m) {
  // 读取失败时写入 NAN/Valid=false；网络层会将对应数值上传为 JSON null。
  if (uint32_t(now - retryAt) >= 10000) { retryAt = now; discover(now); }
  if (uint32_t(now - airAt) >= cfg::SAMPLE_MS) {
    airAt = now;
    float t = NAN, h = NAN;
    const bool readOk = shtReady && sht.readBoth(&t, &h);
    m.airValid = readOk && isfinite(t) && isfinite(h) && t >= -40 && t <= 125 && h >= 0 && h <= 100;
    m.airC = m.airValid ? t : NAN; m.humidity = m.airValid ? h : NAN;
    if (!m.airValid) shtReady = false;
    if (gasReady) {
      // Sensirion/Adafruit 的补偿单位为 mg/m³；0 表示禁用补偿。
      uint32_t absoluteHumidity = 0;
      if (m.airValid) {
        const double ah = 216.7 * (h / 100.0 * 6.112 * exp(17.62 * t / (243.12 + t))) / (273.15 + t);
        absoluteHumidity = uint32_t(fmin(255999.0, fmax(0.0, ah * 1000.0)));
      }
      if (!gas.setHumidity(absoluteHumidity)) Serial.println("[SGP30] humidity compensation failed");
    }
  }
  // DS18B20 12bit 转换约750ms，等待期间继续运行门磁和控制逻辑。
  if (conversionPending && uint32_t(now - conversionAt) >= 750) {
    const float value = grain.getTempC(grainAddress);
    m.grainValid = isfinite(value) && value >= -55 && value <= 125 && value != DEVICE_DISCONNECTED_C;
    m.grainC = m.grainValid ? value : NAN;
    conversionPending = false;
    if (!m.grainValid) grainReady = false;
  }
  if (!conversionPending && uint32_t(now - grainAt) >= cfg::SAMPLE_MS) {
    grainAt = now;
    if (grainReady) {
      if (grain.requestTemperaturesByAddress(grainAddress)) {
        conversionAt = millis(); conversionPending = true;
      } else {
        grainReady = false; m.grainValid = false; m.grainC = NAN;
      }
    } else { m.grainValid = false; m.grainC = NAN; }
  }
  if (uint32_t(now - lightAt) >= 2000) {
    lightAt = now;
    const float lux = lightReady ? light.readLightLevel() : NAN;
    m.lightValid = isfinite(lux) && lux >= 0;
    m.lux = m.lightValid ? lux : NAN;
    if (!m.lightValid) lightReady = false;
  }
  m.gasOnline = gasReady;
  if (uint32_t(now - gasAt) >= 1000) {
    // 不通过连续补读追赶遗漏的1s采样；大幅延迟时重新开始IAQ算法。
    if (uint32_t(now - gasAt) > 2000 && gasReady) {
      gasReady = false; m.gasOnline = false; resetTrend();
    }
    gasAt = now;
    const bool ok = gasReady && gas.IAQmeasure();
    m.gasWarming = !gasReady || uint32_t(now - gasStarted) < 15000;
    m.gasValid = ok && !m.gasWarming;
    m.tvoc = m.gasValid ? gas.TVOC : NAN;
    m.eco2 = m.gasValid ? gas.eCO2 : NAN;
    m.tvocTrend = false;
    // 连续测量失败时丢弃旧趋势窗口，三次失败后等待重新发现 SGP30。
    if (!ok) {
      resetTrend();
      if (++gasErrors >= 3) { gasReady = false; m.gasOnline = false; }
    } else {
      gasErrors = 0;
      if (m.gasValid) {
        // 与此前完整10分钟均值比较，演示阈值为1.8倍且至少增加100ppb。
        if (tvocCount == 600) {
          m.tvocBaseline = tvocSum / 600.0f;
          m.tvocTrend = m.tvoc > m.tvocBaseline * 1.8f && m.tvoc > m.tvocBaseline + 100;
          tvocSum -= tvocWindow[tvocIndex];
        } else { ++tvocCount; m.tvocBaseline = NAN; }
        tvocWindow[tvocIndex] = gas.TVOC; tvocSum += gas.TVOC;
        tvocIndex = (tvocIndex + 1) % 600;
      }
    }
    if (!m.gasValid) m.tvocBaseline = NAN;
  }
}

void doorBegin() {
  pinMode(cfg::DOOR_PIN, INPUT_PULLUP);
  // 开机先记住当前门磁状态；若此时已开门，不凭空增加一次开门事件。
  stableOpen = rawOpen = readDoor();
  rawSince = openedAt = millis();
}
void doorTick(uint32_t now, const Measurements &m, DoorState &d) {
  const bool read = readDoor();
  // 输入保持 50 ms 才确认状态变化，过滤机械触点抖动。
  if (read != rawOpen) { rawOpen = read; rawSince = now; }
  if (rawOpen != stableOpen && uint32_t(now - rawSince) >= 50) {
    stableOpen = rawOpen;
    if (stableOpen) {
      openedAt = now; ++d.count; litRecorded = false; eventBaseline = closedLux;
    } else {
      const uint32_t seconds = uint32_t(now - openedAt) / 1000;
      if (seconds > d.longestSeconds) d.longestSeconds = seconds;
    }
  }
  d.open = stableOpen;
  d.openSeconds = d.open ? uint32_t(now - openedAt) / 1000 : 0;
  if (d.openSeconds > d.longestSeconds) d.longestSeconds = d.openSeconds;
  if (!d.open && m.lightValid) closedLux = m.lux;
  // 每次开门最多记一次光照确认；同时要求绝对增量和相对增量达标。
  if (d.open && !litRecorded && m.lightValid && isfinite(eventBaseline) &&
      m.lux > eventBaseline + 20 && m.lux > eventBaseline * 1.5f) {
    ++d.litCount; litRecorded = true;
  }
}
