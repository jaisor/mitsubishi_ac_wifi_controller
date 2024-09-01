#if !( defined(ESP32) ) && !( defined(ESP8266) )
  #error This code is intended to run on ESP8266 or ESP32 platform!
#endif

#include <Arduino.h>
#include <WiFiClient.h>
#include <Time.h>
#include <ezTime.h>
#include <ElegantOTA.h>
#include <StreamUtils.h>
#include <AsyncJson.h>
#include "Configuration.h"
#include "wifi/WifiManager.h"
#include "wifi/HTMLAssets.h"

#define MAX_CONNECT_TIMEOUT_MS 15000 // 10 seconds to connect before creating its own AP
#define POST_UPDATE_INTERVAL 300000 // Every 5 min

const int RSSI_MAX =-50;// define maximum straighten of signal in dBm
const int RSSI_MIN =-100;// define minimum strength of signal in dBm

WiFiClient espClient;

int dBmtoPercentage(int dBm) {
  int quality;
  if(dBm <= RSSI_MIN) {
    quality = 0;
  } else if(dBm >= RSSI_MAX) {  
    quality = 100;
  } else {
    quality = 2 * (dBm + 100);
  }
  return quality;
}

CWifiManager::CWifiManager(ISensorProvider *sensorProvider)
:sensorProvider(sensorProvider), rebootNeeded(false), wifiRetries(0) {  

  sensorJson["dev_name"] = configuration.name;
  #ifdef BATTERY_SENSOR
  sensorJson["battVoltsDivider"] = configuration.battVoltsDivider;
  #endif

  strcpy(SSID, configuration.wifiSsid);
  server = new AsyncWebServer(WEB_SERVER_PORT);
  mqtt.setClient(espClient);
  connect();
}

void CWifiManager::connect() {

  status = WF_CONNECTING;
  strcpy(softAP_SSID, "");
  tMillis = millis();

  uint32_t deviceId = CONFIG_getDeviceId();
  sensorJson["device_id"] = deviceId;
  Log.infoln("Device ID: '%i'", deviceId);

  if (strlen(SSID)) {

    // Join AP from Config
    Log.infoln("Connecting to WiFi: '%s'", SSID);
    WiFi.begin(SSID, configuration.wifiPassword);
    wifiRetries = 0;

  } else {

    // Create AP using fallback and chip ID
    sprintf_P(softAP_SSID, "%s_%i", WIFI_FALLBACK_SSID, deviceId);
    Log.infoln("Creating WiFi: '%s' / '%s'", softAP_SSID, WIFI_FALLBACK_PASS);

    if (WiFi.softAP(softAP_SSID, WIFI_FALLBACK_PASS)) {
      wifiRetries = 0;
      tsAPReboot = millis();
      Log.infoln("Wifi AP '%s' created, listening on '%s'", softAP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
      Log.errorln("Wifi AP faliled");
    };

  }
  
}

void CWifiManager::listen() {

  status = WF_LISTENING;

  // Web
  server->on("/", std::bind(&CWifiManager::handleRoot, this, std::placeholders::_1));
  server->on("/style.css", HTTP_GET, std::bind(&CWifiManager::handleStyleCSS, this, std::placeholders::_1));
  server->on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(404); });
  //
  server->on("/wifi", HTTP_GET | HTTP_POST, std::bind(&CWifiManager::handleWifi, this, std::placeholders::_1));
  server->on("/device", HTTP_GET | HTTP_POST, std::bind(&CWifiManager::handleDevice, this, std::placeholders::_1));
  server->on("/hp", HTTP_POST, std::bind(&CWifiManager::handleHeatPump, this, std::placeholders::_1));
  //
  server->on("/factory_reset", HTTP_POST, std::bind(&CWifiManager::handleFactoryReset, this, std::placeholders::_1));
  server->on("/reboot", HTTP_POST, std::bind(&CWifiManager::handleReboot, this, std::placeholders::_1));
  server->on("/mqtt_reconnect", HTTP_POST, std::bind(&CWifiManager::handleFixMQTT, this, std::placeholders::_1));
#ifdef WEB_LOGGING
  server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request){ 
    Log.traceln("handleLog");
    intLEDOn();
    AsyncResponseStream *response = request->beginResponseStream("text/plain; charset=UTF-8");
    response->println(logStream.str().c_str());
    request->send(response);
    intLEDOff();
  });
#endif
  // API - TODO: Only GET works
  server->on("/api", HTTP_GET, std::bind(&CWifiManager::handleRestAPI, this, std::placeholders::_1));
  AsyncCallbackJsonWebHandler* handler = new AsyncCallbackJsonWebHandler("/api", [this](AsyncWebServerRequest *request, JsonVariant &json) {
    JsonObject&& jsonObj = json.as<JsonObject>();
    
    JsonDocument ac = sensorProvider->getACSettings();

    if (jsonObj.containsKey("power")) { ac["power"] = jsonObj["power"]; }
    if (jsonObj.containsKey("mode")) { ac["mode"] = jsonObj["mode"]; }
    
    if (jsonObj.containsKey("temperature")) {
      int tu = jsonObj["temperature"];
      ac["temperature"] = roundf(configuration.tempUnit == TEMP_UNIT_CELSIUS ? tu : (((float)tu - 32.0) / 1.8));
    }

    if (jsonObj.containsKey("fan")) { ac["fan"] = jsonObj["fan"]; }
    if (jsonObj.containsKey("vane")) { ac["vane"] = jsonObj["vane"]; }
    if (jsonObj.containsKey("wideVane")) { ac["wideVane"] = jsonObj["wideVane"]; }
    
    String jsonStr;
    serializeJson(ac, jsonStr);
    Log.verboseln("new hpSettings: '%s'", jsonStr.c_str());
    
    bool success = sensorProvider->setACSettings(ac);
    
    AsyncResponseStream *response = request->beginResponseStream("text/plain; charset=UTF-8");
    if (success) {
      response->print("OK");
      response->setCode(200);
    } else {
      response->print("ERROR");
      response->setCode(500);
    }
    
  });
  server->addHandler(handler);


  server->begin();
  Log.infoln("Web server listening on %s port %i", WiFi.localIP().toString().c_str(), WEB_SERVER_PORT);
  
  sensorJson["ip"] = WiFi.localIP().toString();

  // NTP
  Log.infoln("Configuring time from %s at %i (%i)", configuration.ntpServer, configuration.gmtOffset_sec, configuration.daylightOffset_sec);
  configTime(configuration.gmtOffset_sec, configuration.daylightOffset_sec, configuration.ntpServer);
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    Log.noticeln("The time is %i:%i", timeinfo.tm_hour,timeinfo.tm_min);
  }

  // OTA
  ElegantOTA.begin(server);

  // MQTT
  mqtt.setServer(configuration.mqttServer, configuration.mqttPort);

  using std::placeholders::_1;
  using std::placeholders::_2;
  using std::placeholders::_3;
  mqtt.setCallback(std::bind( &CWifiManager::mqttCallback, this, _1,_2,_3));

  if (strlen(configuration.mqttServer) && strlen(configuration.mqttTopic) && !mqtt.connected()) {
    Log.noticeln("Attempting MQTT connection to '%s:%i' ...", configuration.mqttServer, configuration.mqttPort);
    if (mqtt.connect(String(CONFIG_getDeviceId()).c_str())) {
      Log.noticeln("MQTT connected");
      
      sprintf_P(mqttSubcribeTopicConfig, "%s/%u/config", configuration.mqttTopic, CONFIG_getDeviceId());
      bool r = mqtt.subscribe(mqttSubcribeTopicConfig);
      Log.noticeln("Subscribed for config changes to MQTT topic '%s' success = %T", mqttSubcribeTopicConfig, r);

      postSensorUpdate();
    } else {
      Log.warningln("MQTT connect failed, rc=%i", mqtt.state());
    }
  }
}

void CWifiManager::loop() {

  ElegantOTA.loop();

  if (rebootNeeded && millis() - tMillis > 300) {
    Log.noticeln("Rebooting...");
  #if defined(ESP32)
    ESP.restart();
  #elif defined(ESP8266)
    ESP.reset();
  #endif
    return;
  }

  if (WiFi.status() == WL_CONNECTED || isApMode() ) {
    // WiFi is connected

    if (status != WF_LISTENING) {  
      // Start listening for requests
      listen();
      return;
    }

    mqtt.loop();
    
    if (!isApMode() && strlen(configuration.mqttServer) && strlen(configuration.mqttTopic)) {
      if (millis() - tMillis > POST_UPDATE_INTERVAL) {
        tMillis = millis();
        postSensorUpdate();
      }
    }

    if (isApMode() && strlen(configuration.wifiSsid)) {
      if (WiFi.softAPgetStationNum() > 0)  {
        tsAPReboot = millis();
      } else if (millis() - tsAPReboot > 60000) {
        // Reboot if in AP mode and no connected clients, in hope of connecting to real AP
        Log.infoln(F("Rebooting after a minute in AP with no connections"));
        rebootNeeded = true;
      }
    }

  } else if (WiFi.status() == WL_NO_SSID_AVAIL && !isApMode()) {
    // Can't find desired AP
    if (millis() - tMillis > MAX_CONNECT_TIMEOUT_MS) {
      tMillis = millis();
      if (++wifiRetries > 1) {
        Log.warningln("Failed to find previous AP (wifi status %i) after %l ms, create an AP instead", WiFi.status(), (millis() - tMillis));
        strcpy(SSID, "");
        WiFi.disconnect(false, true);
        connect();
      } else {
        Log.warningln("Can't find previous AP (wifi status %i) trying again attempt: %i", WiFi.status(), wifiRetries);
      }
      //Log.infoln("WifiMode == %i", WiFi.getMode());
    }
  } else {
    // WiFi is down
    switch (status) {
      case WF_LISTENING: {
      Log.infoln("Disconnecting %i", status);
      server->end();
      status = WF_CONNECTING;
      connect();
      } break;
      case WF_CONNECTING: {
        if (millis() - tMillis > MAX_CONNECT_TIMEOUT_MS) {
          tMillis = millis();
          if (++wifiRetries > 3) {
            Log.warningln("Connecting failed (wifi status %i) after %l ms, create an AP instead", WiFi.status(), (millis() - tMillis));
            strcpy(SSID, "");
          }
          connect();
        }
      } break;
    } // switch
  }
  
}

void CWifiManager::handleRoot(AsyncWebServerRequest *request) {
  Log.traceln("handleRoot");
  intLEDOn();

  AsyncResponseStream *response = request->beginResponseStream("text/html; charset=UTF-8");
  printHTMLTop(response);
  printHTMLHeatPump(response);
  printHTMLBottom(response);
  request->send(response);

  intLEDOff();
}

void CWifiManager::handleWifi(AsyncWebServerRequest *request) {
  Log.traceln("handleWifi: %s", request->methodToString());
  intLEDOn();

  if (request->method() == HTTP_POST) {
    String ssid = request->arg("ssid");
    String password = request->arg("password");
    
    AsyncResponseStream *response = request->beginResponseStream("text/html; charset=UTF-8");
    
    printHTMLTop(response);
    response->printf("<p>Connecting to '%s' ... see you on the other side!</p>", ssid.c_str());
    printHTMLBottom(response);

    request->send(response);

    ssid.toCharArray(configuration.wifiSsid, sizeof(configuration.wifiSsid));
    password.toCharArray(configuration.wifiPassword, sizeof(configuration.wifiPassword));

    Log.noticeln("Saved config SSID: '%s'", configuration.wifiSsid);

    EEPROM_saveConfig();

    strcpy(SSID, configuration.wifiSsid);
    WiFi.disconnect(true, true);
    tMillis = millis();
    rebootNeeded = true;
  } else {
    AsyncResponseStream *response = request->beginResponseStream("text/html; charset=UTF-8");
    printHTMLTop(response);
    response->printf_P(htmlWifi);
    printHTMLBottom(response);
    request->send(response);
  }

  intLEDOff();
}

void CWifiManager::handleDevice(AsyncWebServerRequest *request) {
  Log.traceln("handleDevice: %s", request->methodToString());
  intLEDOn();

  if (request->method() == HTTP_POST) {
    configuration.ledEnabled = request->hasArg("ledEnabled");

    String deviceName = request->arg("deviceName");
    deviceName.toCharArray(configuration.name, sizeof(configuration.name));
    Log.infoln("Device req name: %s", deviceName);
    Log.infoln("Device size %i name: %s", sizeof(configuration.name), configuration.name);

    String mqttServer = request->arg("mqttServer");
    mqttServer.toCharArray(configuration.mqttServer, sizeof(configuration.mqttServer));
    Log.infoln("MQTT Server: %s", mqttServer);

    uint16_t mqttPort = atoi(request->arg("mqttPort").c_str());
    configuration.mqttPort = mqttPort;
    Log.infoln("MQTT Port: %u", mqttPort);

    String mqttTopic = request->arg("mqttTopic");
    mqttTopic.toCharArray(configuration.mqttTopic, sizeof(configuration.mqttTopic));
    Log.infoln("MQTT Topic: %s", mqttTopic);

    uint16_t tempUnit = atoi(request->arg("tempUnit").c_str());
    configuration.tempUnit = tempUnit;
    Log.infoln("Temperature unit: %u", tempUnit);

    EEPROM_saveConfig();
    
    request->redirect("/");
    tMillis = millis();
    rebootNeeded = true;
  } else {

    char tempUnit[256];
    snprintf(tempUnit, 256, "<option %s value='0'>Celsius</option>\
      <option %s value='1'>Fahrenheit</option>", 
      configuration.tempUnit == TEMP_UNIT_CELSIUS ? "selected" : "", 
      configuration.tempUnit == TEMP_UNIT_FAHRENHEIT ? "selected" : "");

    AsyncResponseStream *response = request->beginResponseStream("text/html; charset=UTF-8");
    printHTMLTop(response);
    response->printf_P(htmlDevice, configuration.ledEnabled ? "checked" : "",
      configuration.name, tempUnit,
      configuration.mqttServer, configuration.mqttPort, configuration.mqttTopic);
    printHTMLBottom(response);
    request->send(response);
  }
  intLEDOff();
}

void CWifiManager::handleHeatPump(AsyncWebServerRequest *request) {
  Log.traceln("handleHeatPump: %s", request->methodToString());
  intLEDOn();

  JsonDocument ac = sensorProvider->getACSettings();

  ac["power"] = request->arg("power");
  ac["mode"] = request->arg("mode");
  
  int tu = atoi(request->arg("temperature").c_str());
  ac["temperature"] = roundf(configuration.tempUnit == TEMP_UNIT_CELSIUS ? tu : (((float)tu - 32.0) / 1.8));
  
  ac["fan"] = request->arg("fan");
  ac["vane"] = request->arg("vane");
  ac["wideVane"] = request->arg("wideVane");

  String jsonStr;
  serializeJson(ac, jsonStr);
  Log.verboseln("new hpSettings: '%s'", jsonStr.c_str());
  
  bool success = sensorProvider->setACSettings(ac);
  
  AsyncResponseStream *response = request->beginResponseStream("text/plain; charset=UTF-8");
  if (success) {
    response->print("OK");
    response->setCode(200);
  } else {
    response->print("ERROR");
    response->setCode(500);
  }
  
  request->send(response);

  intLEDOff();
}

void CWifiManager::handleFactoryReset(AsyncWebServerRequest *request) {
  Log.traceln("handleFactoryReset");
  intLEDOn();
  
  AsyncResponseStream *response = request->beginResponseStream("text/plain; charset=UTF-8");
  response->setCode(200);
  response->print("OK");

  EEPROM_wipe();
  tMillis = millis();
  rebootNeeded = true;
  
  request->send(response);
  intLEDOff();
}

void CWifiManager::handleReboot(AsyncWebServerRequest *request) {
  Log.traceln("handleReboot");
  intLEDOn();
  
  AsyncResponseStream *response = request->beginResponseStream("text/plain; charset=UTF-8");
  response->setCode(200);
  response->print("OK");

  tMillis = millis();
  rebootNeeded = true;
  
  request->send(response);
  intLEDOff();
}

void CWifiManager::handleFixMQTT(AsyncWebServerRequest *request) {
  Log.traceln("handleReboot");
  intLEDOn();
  
  ensureMQTTConnected();

  request->redirect("/");
  intLEDOff();
}

void CWifiManager::handleRestAPI(AsyncWebServerRequest *request) {
  Log.traceln("handleRestAPI: %s", request->methodToString());
  intLEDOn();
  
  JsonDocument ac = sensorProvider->getACSettings();
  
  String jsonStr;
  serializeJson(ac, jsonStr);
  Log.verboseln("hpSettings: '%s'", jsonStr.c_str());

  AsyncResponseStream *response = request->beginResponseStream("application/json; charset=UTF-8");
  response->print(jsonStr);
  response->setCode(200);
  request->send(response);

  intLEDOff();
}

void CWifiManager::handleStyleCSS(AsyncWebServerRequest *request) {
  Log.traceln("handleStyleCSS");
  static uint32_t dataLen = strlen_P(cssPico);
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/css; charset=UTF-8", [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
    size_t len = (dataLen>maxLen)?maxLen:dataLen;
    if (len > 0) {
      memcpy_P(buffer, cssPico + index, len);
      dataLen -= len;
    } else {
      dataLen = strlen_P(cssPico);
    }
    return len;
  });
  request->send(response);
}

void CWifiManager::postSensorUpdate() {

  if (!strlen(configuration.mqttTopic)) {
    Log.warningln("Blank MQTT topic");
    return;
  }

  if (!ensureMQTTConnected()) {
    Log.errorln("Unable to post sensor update due to MQTT connection issues");
    return;
  }

  intLEDOn();

  char topic[255];
  int iv;

  iv = dBmtoPercentage(WiFi.RSSI());
  sensorJson["wifi_percent"] = iv;
  sensorJson["wifi_rssi"] = WiFi.RSSI();

  time_t now; 
  time(&now);
  unsigned long uptimeMillis = CONFIG_getUpTime();

  sensorJson["uptime_millis"] = uptimeMillis;
  // Convert to ISO8601 for JSON
  char buf[sizeof "2011-10-08T07:07:09Z"];
  strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
  sensorJson["timestamp_iso8601"] = String(buf);

  sensorJson["mqttConfigTopic"] = mqttSubcribeTopicConfig;

#ifdef TEMP_SENSOR_PIN
  bool sensorReady = sensorProvider->isSensorReady();
  float v;

  if (sensorReady) {
    bool current = false;
    v = sensorProvider->getTemperature(&current);
    if (configuration.tempUnit == TEMP_UNIT_FAHRENHEIT) {
      v = v * 1.8 + 32;
    }
    char tunit[32];
    snprintf(tunit, 32, (configuration.tempUnit == TEMP_UNIT_CELSIUS ? "Celsius" : (configuration.tempUnit == TEMP_UNIT_FAHRENHEIT ? "Fahrenheit" : "" )));
    
    if (current) {
      sensorJson["temperature"] = v;
      sensorJson["temperature_unit"] = tunit;
    }

    v = sensorProvider->getHumidity(&current);
    if (current) {
      sensorJson["humidity"] = v;
      sensorJson["humidit_unit"] = "percent";
    }
  }
#endif
#ifdef BATTERY_SENSOR
  if (configuration.battVoltsDivider > 0) {
    sensorJson["battery_v"] = sensorProvider->getBatteryVoltage(NULL);
    iv = analogRead(BATTERY_SENSOR_ADC_PIN);
    sensorJson["adc_raw"] = iv;
  }
#endif
#ifdef RADIO_RF24
  sensorJson["rf24_channel"] = configuration.rf24_channel;
  sensorJson["rf24_data_rate"] = configuration.rf24_data_rate;
  sensorJson["rf24_pa_level"] = configuration.rf24_pa_level;
  sensorJson["rf24_pipe_suffix"] = configuration.rf24_pipe_suffix;
  sensorJson["rf_msq_queue_size"] = messageQueue->getQueue()->size();
#endif

  sensorJson["ac"] = sensorProvider->getACSettings();

  // sensor Json
  sprintf_P(topic, "%s/json", configuration.mqttTopic);
  mqtt.beginPublish(topic, measureJson(sensorJson), false);
  BufferingPrint bufferedClient(mqtt, 32);
  serializeJson(sensorJson, bufferedClient);
  bufferedClient.flush();
  mqtt.endPublish();

  String jsonStr;
  serializeJson(sensorJson, jsonStr);
  Log.noticeln("Sent '%s' json to MQTT topic '%s'", jsonStr.c_str(), topic);

  intLEDOff();
}

bool CWifiManager::isApMode() { 
#if defined(ESP32)
  return WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_MODE_APSTA; 
#elif defined(ESP8266)
  return WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA; 
#endif

}

void CWifiManager::mqttCallback(char *topic, uint8_t *payload, unsigned int length) {

  if (length == 0) {
    return;
  }

  Log.noticeln("Received %u bytes message on MQTT topic '%s'", length, topic);
  if (!strcmp(topic, mqttSubcribeTopicConfig)) {
    deserializeJson(configJson, (const byte*)payload, length);

    String jsonStr;
    serializeJson(configJson, jsonStr);
    Log.noticeln("Received configuration over MQTT with json: '%s'", jsonStr.c_str());

    if (configJson.containsKey("name")) {
      strncpy(configuration.name, configJson["name"], 128);
    }

    if (configJson.containsKey("mqttTopic")) {
      strncpy(configuration.mqttTopic, configJson["mqttTopic"], 64);
    }

    // Delete the config message in case it was retained
    mqtt.publish(mqttSubcribeTopicConfig, NULL, 0, true);
    Log.noticeln("Deleted config message");

    EEPROM_saveConfig();
    postSensorUpdate();
  }
  
}

void CWifiManager::printHTMLTop(Print *p) {
  float t = sensorProvider->getTemperature(NULL);
  p->printf_P(htmlTop, 
    configuration.name, 
    isApMode() ? softAP_SSID : SSID, dBmtoPercentage(WiFi.RSSI()),
    configuration.tempUnit == TEMP_UNIT_CELSIUS ? t : t * 1.8 + 32, configuration.tempUnit == TEMP_UNIT_CELSIUS ? "C" : "F",
    configuration.name
    
  );
}

void CWifiManager::printHTMLBottom(Print *p) {
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  char mqttStat[255];
  if (mqtt.state() == MQTT_CONNECTED) {
    snprintf_P(mqttStat, 255, PSTR("✅"));
  } else {
    snprintf_P(mqttStat, 255, PSTR("<a href='mqtt_reconnect'>❌[%i]</a>"), mqtt.state());
  }

  JsonDocument ac = sensorProvider->getACSettings();
  String jsonStr;
  serializeJson(ac, jsonStr);
  Log.verboseln("hpSettings: '%s'", jsonStr.c_str());

  p->printf_P(htmlBottom, hr, min % 60, sec % 60, mqttStat, jsonStr.c_str());
}

void CWifiManager::printHTMLHeatPump(Print *p) {

  JsonDocument ac = sensorProvider->getACSettings();
  float t = ac.containsKey("temperature") ? ac["temperature"] : 0;

  char selectPower[512] = "";
  if (ac.containsKey("power")) {
    snprintf_P(selectPower, 512, PSTR("\
      <option %s value='OFF'>OFF</option>\
      <option %s value='ON'>ON</option>"
      ), 
      strcmp(ac["power"], "OFF") == 0 ? "selected" : "", 
      strcmp(ac["power"], "ON") == 0 ? "selected" : ""
    );
  }

  char selectMode[512] = "";
  if (ac.containsKey("mode")) {
    snprintf_P(selectMode, 512, PSTR("\
      <option %s value='HEAT'>HEAT</option>\
      <option %s value='DRY'>DRY</option>\
      <option %s value='COOL'>COOL</option>\
      <option %s value='FAN'>FAN</option>\
      <option %s value='AUTO'>AUTO</option>\
      "), 
      strcmp(ac["mode"], "HEAT") == 0 ? "selected" : "", 
      strcmp(ac["mode"], "DRY") == 0 ? "selected" : "",
      strcmp(ac["mode"], "COOL") == 0 ? "selected" : "",
      strcmp(ac["mode"], "FAN") == 0 ? "selected" : "",
      strcmp(ac["mode"], "AUTO") == 0 ? "selected" : ""
    );
  }

  char selectFan[512] = "";
  if (ac.containsKey("fan")) {
    snprintf_P(selectFan, 512, PSTR("\
      <option %s value='AUTO'>AUTO</option>\
      <option %s value='QUIET'>QUIET</option>\
      <option %s value='1'>1</option>\
      <option %s value='2'>2</option>\
      <option %s value='3'>3</option>\
      <option %s value='4'>4</option>\
      "), 
      strcmp(ac["fan"], "AUTO") == 0 ? "selected" : "", 
      strcmp(ac["fan"], "QUIET") == 0 ? "selected" : "",
      strcmp(ac["fan"], "1") == 0 ? "selected" : "",
      strcmp(ac["fan"], "2") == 0 ? "selected" : "",
      strcmp(ac["fan"], "3") == 0 ? "selected" : "",
      strcmp(ac["fan"], "4") == 0 ? "selected" : ""
    );
  }

  uint8_t tu = (uint8_t)lroundf(configuration.tempUnit == TEMP_UNIT_CELSIUS ? t : t * 1.8 + 32);
  uint8_t tminu = configuration.tempUnit == TEMP_UNIT_CELSIUS ? 16 : 60;
  uint8_t tmaxu = configuration.tempUnit == TEMP_UNIT_CELSIUS ? 31 : 88;

  char selectVane[512] = "";
  if (ac.containsKey("vane")) {
    snprintf_P(selectVane, 512, PSTR("\
      <option %s value='AUTO'>AUTO</option>\
      <option %s value='1'>1</option>\
      <option %s value='2'>2</option>\
      <option %s value='3'>3</option>\
      <option %s value='4'>4</option>\
      <option %s value='5'>5</option>\
      <option %s value='SWING'>SWING</option>\
      "), 
      strcmp(ac["vane"], "AUTO") == 0 ? "selected" : "", 
      strcmp(ac["vane"], "1") == 0 ? "selected" : "",
      strcmp(ac["vane"], "2") == 0 ? "selected" : "",
      strcmp(ac["vane"], "3") == 0 ? "selected" : "",
      strcmp(ac["vane"], "4") == 0 ? "selected" : "",
      strcmp(ac["vane"], "5") == 0 ? "selected" : "",
      strcmp(ac["vane"], "SWING") == 0 ? "selected" : ""
    );
  }

  char selectWideVane[512] = "";
  if (ac.containsKey("vane")) {
    snprintf_P(selectWideVane, 512, PSTR("\
      <option %s value='&lt;&lt;'>&lt;&lt;</option>\
      <option %s value='&lt;'>&lt;</option>\
      <option %s value='|'>|</option>\
      <option %s value='&gt;'>&gt;</option>\
      <option %s value='&gt;&gt;'>&gt;&gt;</option>\
      <option %s value='&lt;&gt;'>&lt;&gt;</option>\
      <option %s value='SWING'>SWING</option>\
      "), 
      strcmp(ac["wideVane"], "<<") == 0 ? "selected" : "",
      strcmp(ac["wideVane"], "<") == 0 ? "selected" : "",
      strcmp(ac["wideVane"], "|") == 0 ? "selected" : "",
      strcmp(ac["wideVane"], ">") == 0 ? "selected" : "",
      strcmp(ac["wideVane"], ">>") == 0 ? "selected" : "",
      strcmp(ac["wideVane"], "<>") == 0 ? "selected" : "",
      strcmp(ac["wideVane"], "SWING") == 0 ? "selected" : ""
    );
  }

  p->printf_P(htmlHeatPump, 
    ac.containsKey("connected") && ac["connected"] ? PSTR("✅") : PSTR("❌"),
    ac.containsKey("operating") && ac["operating"] ? "⚡" : "",
    selectPower,
    selectMode,
    //
    tu, configuration.tempUnit == TEMP_UNIT_CELSIUS ? "C" : "F", tu, tminu, tmaxu,
    //
    selectFan, // fan
    selectVane, // vane
    selectWideVane // wideVane
  );
}

bool CWifiManager::ensureMQTTConnected() {
  if (!mqtt.connected() || mqtt.state() != MQTT_CONNECTED) {
    //Log.noticeln("Disconnecting MQTT...");
    //mqtt.disconnect();
    if (strlen(configuration.mqttServer) && strlen(configuration.mqttTopic)) { // Reconnectable
      Log.noticeln("Attempting to reconnect from MQTT state %i at '%s:%i' ...", mqtt.state(), configuration.mqttServer, configuration.mqttPort);
      if (mqtt.connect(String(CONFIG_getDeviceId()).c_str())) {
        Log.noticeln("MQTT reconnected");
        sprintf_P(mqttSubcribeTopicConfig, "%s/%u/config", configuration.mqttTopic, CONFIG_getDeviceId());
        bool r = mqtt.subscribe(mqttSubcribeTopicConfig);
        Log.noticeln("Subscribed for config changes to MQTT topic '%s' success = %T", mqttSubcribeTopicConfig, r);
      } else {
        Log.warningln("MQTT reconnect failed, rc=%i", mqtt.state());
      }
    }
    if (!mqtt.connected() || mqtt.state() != MQTT_CONNECTED) {
      Log.noticeln("MQTT not connected %i", mqtt.state());
      return false;
    }
  }
  return true;
}
