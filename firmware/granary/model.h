#pragma once
#include <stdint.h>
#include <math.h>

// 主循环、网络任务共用的数据模型。无效传感器读数用 NAN + Valid=false 表示，
// 上传 JSON 时转换为 null，避免把断线误报成 0。
struct Measurements {
  float airC = NAN, humidity = NAN, grainC = NAN, lux = NAN;
  float tvoc = NAN, eco2 = NAN;
  bool airValid = false, grainValid = false, lightValid = false, gasValid = false;
  bool gasOnline = false, gasWarming = true, tvocTrend = false;
  float tvocBaseline = NAN;
};
struct Thresholds {
  // On 为触发点，Off 为解除点；必须满足 Off < On。
  float humidityOn = 65, humidityOff = 60;
  float grainOn = 30, grainOff = 28;
};
struct DoorState {
  // 计数从本次启动开始；litCount 是开门后光照上升的确认次数。
  bool open = false;
  uint32_t openSeconds = 0, longestSeconds = 0, count = 0, litCount = 0;
};
struct ControlState {
  bool humid = false, hot = false, delta = false, alarm = false;
  bool doorAlarm = false;
  uint8_t sensorFaults = 0; // bit0=SHT30，bit1=DS18B20，bit2=BH1750，bit3=SGP30
};
struct Command {
  bool hasThresholds = false;
  Thresholds thresholds;
};
struct Snapshot {
  // 每次复制成完整快照，避免网络任务读取到一半更新的状态。
  Measurements values;
  DoorState door;
  ControlState control;
  Thresholds thresholds;
  uint32_t uptimeMs = 0;
};
struct NetworkState { bool wifi = false, mqtt = false; uint32_t ip = 0; };
