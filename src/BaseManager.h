#pragma once

#include "Configuration.h"

class CBaseManager {
public:
  virtual void loop() {};
  virtual void powerDown() {};
  virtual void powerUp() {};
  virtual const bool isRebootNeeded() { return false; }
  virtual const bool isJobDone() { return false; }
  virtual const bool isError() { return false; }
};
