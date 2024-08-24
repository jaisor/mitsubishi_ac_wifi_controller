#pragma once

#if defined(ESP32)
  #include <WiFi.h>
  #include <AsyncTCP.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Print.h>
#include <vector>

#include "BaseManager.h"
#include "wifi/SensorProvider.h"

typedef enum {
  WF_CONNECTING = 0,
  WF_LISTENING = 1
} wifi_status;

class CWifiManager: public CBaseManager {

private:
  ISensorProvider *sensorProvider;
  
  bool rebootNeeded;
  uint8_t wifiRetries;
  unsigned long tMillis;
  wifi_status status;
  char softAP_SSID[32];
  char SSID[32];
  char mqttSubcribeTopicConfig[255];
  unsigned long tsAPReboot;
  
  AsyncWebServer* server;
  PubSubClient mqtt;

  JsonDocument sensorJson;
  JsonDocument configJson;

  void connect();
  void listen();

  void handleRoot(AsyncWebServerRequest *request);
  void handleWifi(AsyncWebServerRequest *request);
  void handleDevice(AsyncWebServerRequest *request);
  void handleFactoryReset(AsyncWebServerRequest *request);
  void handleHeatPump(AsyncWebServerRequest *request);
  void handleStyleCSS(AsyncWebServerRequest *request);
  void handleRestAPI(AsyncWebServerRequest *request);

  void printHTMLTop(Print *p);
  void printHTMLBottom(Print *p);
  void printHTMLHeatPump(Print *p);

  void postSensorUpdate();
  bool isApMode();

  void mqttCallback(char *topic, uint8_t *payload, unsigned int);
  bool ensureMQTTConnected();

public:
	CWifiManager(ISensorProvider *sensorProvider);
  virtual void loop();

  virtual const bool isRebootNeeded() { return rebootNeeded; }
};
