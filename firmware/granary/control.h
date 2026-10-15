#pragma once
#include "model.h"
#include "config.h"

class Controller {
 public:
  Thresholds thresholds;
  ControlState state;
  void command(const Command &cmd);
  void update(const Measurements &m, const DoorState &d, uint32_t now);
 private:
  bool deltaTiming = false, bootGraceDone = false;
  uint32_t deltaSince = 0;
};
