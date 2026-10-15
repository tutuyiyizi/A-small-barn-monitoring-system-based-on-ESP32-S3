#include "control.h"

// 页面下发的阈值已在 MQTT 回调处校验；只在这里应用到主循环状态机。
void Controller::command(const Command &cmd) {
  if (cmd.hasThresholds) thresholds = cmd.thresholds;
}

void Controller::update(const Measurements &m, const DoorState &d, uint32_t now) {
  // 报警和解除使用不同阈值形成回差，避免读数在临界点附近反复跳变。
  // 读数无效时不沿用旧的湿度/粮温报警，传感器故障由下方故障位单独报警。
  state.humid = m.airValid && (state.humid ? m.humidity > thresholds.humidityOff : m.humidity > thresholds.humidityOn);
  state.hot = m.grainValid && (state.hot ? m.grainC > thresholds.grainOff : m.grainC > thresholds.grainOn);
  // 粮温比仓温高 3℃以上并持续 10 分钟才报警；降到 2℃及以下才解除。
  // 任一温度无效就清除计时，防止缺测期间误累计持续时间。
  if (m.airValid && m.grainValid) {
    const float delta = m.grainC - m.airC;
    if (delta > 3) {
      if (!deltaTiming) { deltaSince = now; deltaTiming = true; }
      if (uint32_t(now - deltaSince) >= cfg::DELTA_HOLD_MS) state.delta = true;
    } else {
      deltaTiming = false;
      if (delta <= 2) state.delta = false;
    }
  } else { deltaTiming = false; state.delta = false; }
  state.doorAlarm = d.open && d.openSeconds >= cfg::DOOR_ALARM_MS / 1000;
  state.sensorFaults = 0;
  // 上电后给传感器 30 秒初始化/预热时间；之后各故障位可叠加。
  if (now >= 30000) bootGraceDone = true;
  if (bootGraceDone) {
    if (!m.airValid) state.sensorFaults |= 1;
    if (!m.grainValid) state.sensorFaults |= 2;
    if (!m.lightValid) state.sensorFaults |= 4;
    if (!m.gasOnline || (!m.gasValid && !m.gasWarming)) state.sensorFaults |= 8;
  }
  // 任一环境异常或传感器故障触发共用的声光报警。
  state.alarm = state.humid || state.hot || state.delta || m.tvocTrend || state.doorAlarm || state.sensorFaults;
}
