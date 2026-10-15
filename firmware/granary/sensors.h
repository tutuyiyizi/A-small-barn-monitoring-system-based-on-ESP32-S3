#pragma once
#include "model.h"
void sensorsBegin();
void sensorsTick(uint32_t now, Measurements &values);
void doorBegin();
void doorTick(uint32_t now, const Measurements &values, DoorState &door);
