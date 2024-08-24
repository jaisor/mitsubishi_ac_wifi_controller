#pragma once

#include "Configuration.h"
#include <ArduinoJson.h>

class ISensorProvider {
public:
  virtual float getTemperature(bool *current) { if (current != NULL) { *current = false; } return 0; };
  virtual float getHumidity(bool *current) { if (current != NULL) { *current = false; } return 0; };
  virtual float getBaroPressure(bool *current) { if (current != NULL) { *current = false; } return 0; };
  virtual float getBatteryVoltage(bool *current) { if (current != NULL) { *current = false; } return 0; };
  virtual uint32_t getDeviceId() { return CONFIG_getDeviceId(); };
  virtual uint32_t getUptime() { return CONFIG_getUpTime(); };
  virtual bool isSensorReady() { return false; };
  virtual JsonDocument& getACSettings();
  virtual bool setACSettings(JsonDocument ac);
};
