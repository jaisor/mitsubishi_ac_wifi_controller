#if !( defined(ESP32) ) && !( defined(ESP8266) )
  #error This code is intended to run on ESP8266 or ESP32 platform!
#endif

#include <Arduino.h>
#include <WiFiClient.h>
#include <Time.h>
#include <ezTime.h>
#include <ElegantOTA.h>
#include <StreamUtils.h>
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

  sensorJson["gw_name"] = configuration.name;
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
  sensorJson["gw_deviceId"] = deviceId;
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
  server->on("/connect", HTTP_POST, std::bind(&CWifiManager::handleConnect, this, std::placeholders::_1));
  server->on("/config", HTTP_POST, std::bind(&CWifiManager::handleConfig, this, std::placeholders::_1));
  server->on("/factory_reset", HTTP_POST, std::bind(&CWifiManager::handleFactoryReset, this, std::placeholders::_1));

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
    
    if (!isApMode() && strlen(configuration.mqttServer) && strlen(configuration.mqttTopic) && mqtt.connected()) {
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

  Log.infoln("handleRoot");
  intLEDOn();

  AsyncResponseStream *response = request->beginResponseStream("text/html");
  printHTMLTop(response);

  if (isApMode()) {
    response->printf(htmlWifiApConnectForm.c_str());
  } else {
    response->printf("<p>Connected to '%s'</p>", SSID);
  }

  printHTMLHeatPump(response);

  char tempUnit[256];
  snprintf(tempUnit, 256, "<option %s value='0'>Celsius</option>\
    <option %s value='1'>Fahrenheit</option>", 
    configuration.tempUnit == TEMP_UNIT_CELSIUS ? "selected" : "", 
    configuration.tempUnit == TEMP_UNIT_FAHRENHEIT ? "selected" : "");

#ifdef BATTERY_SENSOR
  float bvd = configuration.battVoltsDivider;
#else
  float bvd = 0;
#endif

  response->printf(htmlDeviceConfigs.c_str(), configuration.name, tempUnit,
    configuration.mqttServer, configuration.mqttPort, configuration.mqttTopic,
    bvd);

  printHTMLBottom(response);
  request->send(response);

  intLEDOff();
}

void CWifiManager::handleConnect(AsyncWebServerRequest *request) {

  Log.infoln("handleConnect");

  String ssid = request->arg("ssid");
  String password = request->arg("password");
  
  AsyncResponseStream *response = request->beginResponseStream("text/html");
  
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
}

void CWifiManager::handleConfig(AsyncWebServerRequest *request) {

  Log.infoln("handleConfig");

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

#ifdef BATTERY_SENSOR
  float battVoltsDivider = atof(request->arg("battVoltsDivider").c_str());
  configuration.battVoltsDivider = battVoltsDivider;
  Log.infoln("battVoltsDivider: %D", battVoltsDivider);
#endif

#ifdef TEMP_SENSOR
  uint16_t tempUnit = atoi(request->arg("tempUnit").c_str());
  configuration.tempUnit = tempUnit;
  Log.infoln("Temperature unit: %u", tempUnit);
#endif

  EEPROM_saveConfig();
  
  request->redirect("/");
  tMillis = millis();
  rebootNeeded = true;
}

void CWifiManager::handleFactoryReset(AsyncWebServerRequest *request) {
  Log.infoln("handleFactoryReset");
  
  AsyncResponseStream *response = request->beginResponseStream("text/html");
  response->setCode(200);
  response->printf("OK");

  EEPROM_wipe();
  tMillis = millis();
  rebootNeeded = true;
  
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
  float v; int iv;

  iv = dBmtoPercentage(WiFi.RSSI());
  sensorJson["gw_wifi_percent"] = iv;
  sensorJson["gw_wifi_rssi"] = WiFi.RSSI();

  time_t now; 
  time(&now);
  unsigned long uptimeMillis = CONFIG_getUpTime();

  sensorJson["gw_uptime_millis"] = uptimeMillis;
  // Convert to ISO8601 for JSON
  char buf[sizeof "2011-10-08T07:07:09Z"];
  strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
  sensorJson["timestamp_iso8601"] = String(buf);

  sensorJson["mqttConfigTopic"] = mqttSubcribeTopicConfig;

#ifdef TEMP_SENSOR_PIN
  bool sensorReady = sensorProvider->isSensorReady();

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
  p->printf(htmlTop.c_str(), configuration.name, configuration.name);
}

void CWifiManager::printHTMLBottom(Print *p) {
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  char mqttStat[255];
  snprintf_P(mqttStat, 255, PSTR("state: %i / connected: %i"), mqtt.state(), mqtt.connected());

  float t = sensorProvider->getTemperature(NULL);
  p->printf(htmlBottom.c_str(), DEVICE_NAME, hr, min % 60, sec % 60, 
    dBmtoPercentage(WiFi.RSSI()),
    configuration.tempUnit == TEMP_UNIT_CELSIUS ? t : t * 1.8 + 32, configuration.tempUnit == TEMP_UNIT_CELSIUS ? "C" : "F",
    sensorProvider->getBatteryVoltage(NULL), mqttStat);
}

void CWifiManager::printHTMLHeatPump(Print *p) {

  JsonDocument ac = sensorProvider->getACSettings();
  
  String jsonStr;
  serializeJson(ac, jsonStr);
  Log.verboseln("hpSettings: '%s'", jsonStr.c_str());

  float rt = ac.containsKey("roomTemperature") ? ac["roomTemperature"] : 0;
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
      <option %s value='5'>5</option>\
      <option %s value='SWING'>SWING</option>\
      "), 
      strcmp(ac["fan"], "AUTO") == 0 ? "selected" : "", 
      strcmp(ac["fan"], "QUIET") == 0 ? "selected" : "",
      strcmp(ac["fan"], "1") == 0 ? "selected" : "",
      strcmp(ac["fan"], "2") == 0 ? "selected" : "",
      strcmp(ac["fan"], "3") == 0 ? "selected" : "",
      strcmp(ac["fan"], "4") == 0 ? "selected" : "",
      strcmp(ac["fan"], "5") == 0 ? "selected" : "",
      strcmp(ac["fan"], "SWING") == 0 ? "selected" : ""
    );
  }

  // {"AUTO", "QUIET", "1", "2", "3", "4"};
  // {"AUTO", "1", "2", "3", "4", "5", "SWING"};
  // {"<<", "<",  "|",  ">",  ">>", "<>", "SWING"};

  p->printf(htmlHeatPump.c_str(), 
    ac.containsKey("connected") && ac["connected"] ? PSTR("✅") : PSTR("❌"),
    ac.containsKey("operating") && ac["operating"] ? "<div style='color: green; font-weight: bold;'>OPERATING</div><br>" : "",
    configuration.tempUnit == TEMP_UNIT_CELSIUS ? rt : rt * 1.8 + 32, configuration.tempUnit == TEMP_UNIT_CELSIUS ? "C" : "F",
    selectPower,
    selectMode,
    configuration.tempUnit == TEMP_UNIT_CELSIUS ? t : t * 1.8 + 32, configuration.tempUnit == TEMP_UNIT_CELSIUS ? "C" : "F",
    selectFan, // fan
    "", // vane
    "", // wideVane
    jsonStr.c_str()
  );
}

bool CWifiManager::ensureMQTTConnected() {
  if (!mqtt.connected()) {
    if (mqtt.state() != MQTT_CONNECTED 
      && strlen(configuration.mqttServer) && strlen(configuration.mqttTopic)) { // Reconnectable
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
    if (!mqtt.connected()) {
      Log.noticeln("MQTT not connected %i", mqtt.state());
      return false;
    }
  }
  return true;
}
