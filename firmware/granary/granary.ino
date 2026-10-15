#include "config.h"
#include "model.h"
#include "sensors.h"
#include "control.h"
#include "display.h"
#include "mqtt_link.h"

// 主循环负责采样、报警判断和声光输出；Wi-Fi/MQTT 在独立任务中运行。
// 两个任务只通过队列交换快照和阈值命令，断网时本地报警仍继续工作。
Measurements measurements;
DoorState door;
Controller controller;
NetworkState network;
uint32_t lastSnapshot = 0, lastLog = 0;

void setup() {
  // 先把蜂鸣器置为关闭电平，再初始化传感器和网络任务。
  outputsBegin();
  Serial.begin(115200);
  Serial.println("\nGranary ESP32-S3 / Arduino / local MQTT");
  doorBegin(); sensorsBegin(); displayBegin();
  if (!networkBegin()) Serial.println("[ERROR] network task unavailable; local control continues");
}
void loop() {
  // 采样函数按各自周期工作；DS18B20 转换期间不阻塞门磁检测。
  sensorsTick(millis(), measurements);
  uint32_t now = millis();
  doorTick(now, measurements, door);
  Command cmd;
  // MQTT 回调只投递命令，真正修改控制阈值发生在主循环。
  while (networkCommand(cmd)) controller.command(cmd);
  controller.update(measurements, door, now);
  networkState(network);
  outputsTick(controller.state, network, now);
  Snapshot data{measurements, door, controller.state, controller.thresholds, now};
  // 网络队列只保留最新快照，避免断网或重连后积压旧数据。
  if (uint32_t(now - lastSnapshot) >= 200) { lastSnapshot = now; networkSnapshot(data); }
  displayTick(data, network);
  if (uint32_t(now - lastLog) >= 5000) {
    lastLog = now;
    Serial.printf("[DATA] air=%.2f RH=%.2f grain=%.2f TVOC=%.0f eCO2=%.0f lux=%.1f door=%d alarm=%d faults=%u wifi=%d mqtt=%d\n",
      measurements.airC, measurements.humidity, measurements.grainC, measurements.tvoc, measurements.eco2,
      measurements.lux, door.open, controller.state.alarm, controller.state.sensorFaults, network.wifi, network.mqtt);
  }
  delay(5); // 让出调度时间；传感器与控制不等待网络。
}
