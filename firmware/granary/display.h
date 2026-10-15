#pragma once
#include "model.h"
void outputsBegin();
void outputsTick(const ControlState &state, const NetworkState &network, uint32_t now);
void displayBegin();
void displayTick(const Snapshot &data, const NetworkState &network);
