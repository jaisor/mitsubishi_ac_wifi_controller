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
  Serial.begin(19200);  while (!Serial); delay(200);
  randomSeed(analogRead(0));

  Log.begin(LOG_LEVEL, &Serial);
  Log.noticeln("Initializing...");  

  pinMode(INTERNAL_LED_PIN, OUTPUT);
  intLEDOn();

  if (EEPROM_initAndCheckFactoryReset() >= 3) {
    Log.warningln("Factory reset conditions met!");
    EEPROM_wipe();  
  }

  tsSmoothBoot = millis();
  smoothBoot = false;

  EEPROM_loadConfig();

  device = new CDevice();
  rf24Manager = new CRF24Manager();
  wifiManager = new CWifiManager(device);

  if (wifiManager->isError()) {
    Log.errorln("wifiManager->isError()=%i", rf24Manager->isError(), wifiManager->isError());
    while(true) {
      intLEDBlink(250);
      delay(250);
    }
  }

  Log.infoln("Initialized");
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