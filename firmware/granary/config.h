#pragma once
#include <stdint.h>

// 可新建 config_local.h 填写私有参数，该文件已在 .gitignore 中忽略。
#if __has_include("config_local.h")
#include "config_local.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "granary01"
#endif
static_assert(sizeof(DEVICE_ID) > 1 && sizeof(DEVICE_ID) <= 33, "DEVICE_ID must contain 1-32 characters");

namespace cfg {
// GPIO 分配以当前实物接线为准；MC-38 门磁是干接点，依靠内部上拉读取。
constexpr uint8_t SDA_PIN = 8, SCL_PIN = 9, GRAIN_PIN = 4, DOOR_PIN = 5;
constexpr uint8_t BUZZER_PIN = 7, GREEN_PIN = 15, RED_PIN = 16;
// 实物蜂鸣器为低电平触发；空闲时保持高电平。
constexpr bool BUZZER_ACTIVE_HIGH = false;
constexpr bool DOOR_CLOSED_LOW = true;
constexpr bool OLED_ENABLED = true, SECOND_OLED_ENABLED = false;
constexpr uint8_t OLED_ADDRESS = 0x3C, SECOND_OLED_ADDRESS = 0x3D;
constexpr uint8_t OLED_WIDTH = 128, OLED_HEIGHT = 64;
constexpr uint8_t SHT_ADDRESS = 0x44, LIGHT_ADDRESS = 0x23;
constexpr uint32_t SAMPLE_MS = 5000, UPLOAD_MS = 5000;
constexpr uint32_t DELTA_HOLD_MS = 600000;
constexpr uint32_t DOOR_ALARM_MS = 30000;
}
