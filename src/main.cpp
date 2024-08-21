#include <Arduino.h>
#include <ArduinoLog.h>

#if !( defined(ESP32) ) && !( defined(ESP8266) )
  #error This code is intended to run on ESP8266 platform! Please check your Tools->Board setting.
#endif

#include "Configuration.h"
#include "wifi/WifiManager.h"
#include "Device.h"

#ifdef ESP32
#elif ESP8266
  ADC_MODE(ADC_TOUT);
#endif

CWifiManager *wifiManager;
CDevice *device;

unsigned long tsSmoothBoot;
bool smoothBoot;
unsigned long tsMillisBooted;

void setup() {

  randomSeed(analogRead(0));
  
  pinMode(INTERNAL_LED_PIN, OUTPUT);
  intLEDOn();

  #ifndef DISABLE_LOGGING
  Serial.begin(19200); while (!Serial); delay(100);
  Log.begin(LOG_LEVEL, &Serial);
  Log.infoln(F("Initializing..."));
  #endif

  if (EEPROM_initAndCheckFactoryReset() >= 3) {
    Log.warningln("Factory reset conditions met!");
    EEPROM_wipe();  
  }

  tsSmoothBoot = millis();
  smoothBoot = false;

  EEPROM_loadConfig();

  Log.infoln("Configuration loaded");

  device = new CDevice();
  wifiManager = new CWifiManager(device);

  if (wifiManager->isError()) {
    Log.errorln("wifiManager->isError()=%i", wifiManager->isError());
    while(true) {
      intLEDBlink(250);
      delay(250);
    }
  }

  Log.infoln("Initialized");
  intLEDOff();
}

void loop() {
  
  if (!smoothBoot && millis() - tsSmoothBoot > FACTORY_RESET_CLEAR_TIMER_MS) {
    smoothBoot = true;
    EEPROM_clearFactoryReset();
    tsMillisBooted = millis();
    intLEDOff();
    Log.noticeln("Device booted smoothly!");
  }

  device->loop();
  wifiManager->loop();

  if (wifiManager->isRebootNeeded()) {
    return;
  }
 
  yield();
}