#include <assert.h>
#include <stdio.h>
#include "control.h"

static Measurements healthy() {
  Measurements m;
  m.airC = 25; m.humidity = 50; m.grainC = 25;
  m.airValid = m.grainValid = m.lightValid = m.gasValid = m.gasOnline = true;
  m.gasWarming = false;
  return m;
}

int main() {
  DoorState door;
  {
    Controller c; auto m = healthy();
    m.humidity = 70; c.update(m, door, 1000); assert(c.state.humid && c.state.alarm);
    m.humidity = 62; c.update(m, door, 2000); assert(c.state.humid && c.state.alarm);
    m.humidity = 60; c.update(m, door, 3000); assert(!c.state.humid && !c.state.alarm);
    Command cmd; cmd.hasThresholds = true; cmd.thresholds = {75, 65, 32, 29};
    c.command(cmd); c.update(m, door, 4000); assert(!c.state.humid);
    m.humidity = 76; c.update(m, door, 5000); assert(c.state.humid && c.state.alarm);
    m.airValid = false; c.update(m, door, 6000); assert(!c.state.humid && !c.state.alarm);
  }
  {
    Controller c; auto m = healthy();
    m.grainC = 31; c.update(m, door, 1000); assert(c.state.hot && c.state.alarm);
    m.grainC = 29; c.update(m, door, 2000); assert(c.state.hot);
    m.grainC = 28; c.update(m, door, 3000); assert(!c.state.hot && !c.state.alarm);
    m.tvocTrend = true; c.update(m, door, 4000); assert(c.state.alarm);
  }
  {
    Controller c; auto m = healthy();
    m.airC = 20; m.grainC = 24;
    c.update(m, door, 1000); c.update(m, door, 600999); assert(!c.state.delta);
    c.update(m, door, 601000); assert(c.state.delta && c.state.alarm);
    m.grainC = 22; c.update(m, door, 602000); assert(!c.state.delta && !c.state.alarm);
  }
  {
    Controller c; auto m = healthy();
    m.airC = 20; m.grainC = 24; c.update(m, door, 1000);
    m.airValid = false; c.update(m, door, 500000);
    m.airValid = true; c.update(m, door, 601000); assert(!c.state.delta);
    m.grainValid = false; m.grainC = NAN; c.update(m, door, 700000);
    assert(!c.state.hot && (c.state.sensorFaults & 2) && c.state.alarm);
  }
  {
    Controller c; auto m = healthy(); door.open = true;
    door.openSeconds = 29; c.update(m, door, 1000); assert(!c.state.doorAlarm);
    door.openSeconds = 30; c.update(m, door, 2000); assert(c.state.doorAlarm && c.state.alarm);
    door.open = false; c.update(m, door, 3000); assert(!c.state.doorAlarm && !c.state.alarm);
  }
  {
    Controller c; auto m = healthy(); m.airC = 20; m.grainC = 24;
    const uint32_t start = 0xfffffff0U;
    c.update(m, door, start); c.update(m, door, start + 599999U); assert(!c.state.delta);
    c.update(m, door, start + 600000U); assert(c.state.delta); // millis 回绕
  }
  {
    Controller c; auto m = healthy(); m.grainValid = false;
    c.update(m, door, 0xffffff00U); assert(c.state.sensorFaults & 2);
    c.update(m, door, 1000); assert(c.state.sensorFaults & 2); // 启动宽限不能在回绕后重启
  }
  puts("PASS: alert thresholds, hysteresis, delta, faults, door, rollover");
}
