#include <Arduino.h>
#include <functional>
#include <ArduinoLog.h>

#include "Device.h"

#ifdef TEMP_SENSOR_DS18B20
#include <Wire.h>
#endif

#if defined(ESP8266)
  #include <SoftwareSerial.h> // ESP8266 uses software UART because of USB conflict with its single full hardware UART
#endif

#if defined(ESP32)
  #define HP_RX GPIO_NUM_16
  #define HP_TX GPIO_NUM_17
#elif defined(ESP8266)
  #define HP_RX -1 // default
  #define HP_TX -1 // default
#elif defined(SEEED_XIAO_M0)
  #define HP_RX D7
  #define HP_TX D6
#else
  #error Unsupported platform
#endif

unsigned long tsHPSettingsUpdated = 0;
unsigned long tsHPStatusUpdated = 0;
heatpumpSettings hpSettings;
heatpumpStatus hpStatus;
HeatPump hp;

void hpSettingsChanged() {
  hpSettings = hp.getSettings();
  tsHPSettingsUpdated = millis();
}

void hpStatusChanged(heatpumpStatus currentStatus) {
  hpStatus = currentStatus;
  tsHPStatusUpdated = millis();
}

CDevice::CDevice() {

  tMillisUp = millis();
  tMillisTemp = millis();
  sensorReady = false;

  tLastReading = 0;
#ifdef TEMP_SENSOR_DS18B20
  pinMode(TEMP_SENSOR_PIN, INPUT_PULLUP);
  oneWire = new OneWire(TEMP_SENSOR_PIN);
  DeviceAddress da;
  ds18b20 = new DS18B20(oneWire);
  ds18b20->setConfig(DS18B20_CRC);
  ds18b20->begin();

  ds18b20->getAddress(da);
  String addr = "";
  for (uint8_t i = 0; i < 8; i++) {
    if (da[i] < 16) {
      addr += String("o");
    }
    addr += String(da[i], HEX);
  }
  Log.noticeln(F("DS18B20 sensor at address: %s"), addr.c_str());
  
  ds18b20->setResolution(12);
  ds18b20->requestTemperatures();

  sensorReady = true;
  tMillisTemp = 0;
#endif
#ifdef TEMP_SENSOR_BME280
  _bme = new Adafruit_BME280();
  if (!_bme->begin(BME_I2C_ID)) {
    Log.errorln(F("BME280 sensor initialization failed with ID %x"), BME_I2C_ID);
    sensorReady = false;
  } else {
    sensorReady = true;
    tMillisTemp = 0;
  }
#endif
#ifdef TEMP_SENSOR_DHT
  _dht = new DHT_Unified(TEMP_SENSOR_PIN, TEMP_SENSOR_DHT_TYPE);
  _dht->begin();
  sensor_t sensor;
  _dht->temperature().getSensor(&sensor);
  Log.noticeln(F("DHT temperature sensor name(%s) v(%u) id(%u) range(%F - %F) res(%F)"),
    sensor.name, sensor.version, sensor.sensor_id, 
    sensor.min_value, sensor.max_value, sensor.resolution);
  _dht->humidity().getSensor(&sensor);
  Log.noticeln(F("DHT humidity sensor name(%s) v(%u) id(%u) range(%F - %F) res(%F)"),
    sensor.name, sensor.version, sensor.sensor_id, 
    sensor.min_value, sensor.max_value, sensor.resolution);
  minDelayMs = sensor.min_delay / 1000;
  Log.noticeln(F("DHT sensor min delay %i"), minDelayMs);
#endif
#ifdef BATTERY_SENSOR
  #if SEEED_XIAO_M0
    analogReadResolution(12);
  #endif
  pinMode(BATTERY_SENSOR_ADC_PIN, INPUT);
#endif

  bool hpConnected = false;
  hp.enableExternalUpdate();
  hp.setSettingsChangedCallback(hpSettingsChanged);
  hp.setStatusChangedCallback(hpStatusChanged);

#if defined(ESP32)
  #ifdef CONFIG_IDF_TARGET_ESP32C3
    hpConnected = hp.connect(&Serial1);
  #else
    hpConnected = hp.connect(&Serial2, HP_RX, HP_TX);
  #endif
#elif defined(ESP8266)
  #ifdef DISABLE_LOGGING
    hpConnected = hp.connect(&Serial);
  #endif
#elif defined(SEEED_XIAO_M0)
  hpConnected = hp.connect(&Serial1, HP_RX, HP_TX);
#endif
  
  if (hpConnected) {
    Log.infoln("Heat pump UART connected");
  } else {
    Log.errorln("Failed to connect heat pump UART");
  }

  jsonHPSettings["connected"] = hpConnected;

  /*
  hp.setSettings({ //set some default settings
    "ON",  // ON/OFF 
    "FAN", // HEAT/COOL/FAN/DRY/AUTO 
    26,    // Between 16 and 31 
    "4",   // Fan speed: 1-4, AUTO, or QUIET 
    "3",   // Air direction (vertical): 1-5, SWING, or AUTO 
    "|"    // Air direction (horizontal): <<, <, |, >, >>, <>, or SWING 
  });
  */

  Log.infoln(F("Device initialized"));
}

CDevice::~CDevice() { 
#ifdef TEMP_SENSOR_DS18B20
  delete ds18b20;
#endif
#ifdef TEMP_SENSOR_BME280
  delete _bme;
#endif
#ifdef TEMP_SENSOR_DHT
  delete _dht;
#endif
  Log.noticeln(F("Device destroyed"));
}

void CDevice::loop() {

  uint32_t delay = 1000;
  #ifdef TEMP_SENSOR_DHT
    delay = minDelayMs;
  #endif

  if (!sensorReady && millis() - tMillisTemp > delay) {
    sensorReady = true;
  }

  if (sensorReady && millis() - tMillisTemp > delay) {
    #ifdef TEMP_SENSOR_DS18B20
      if (ds18b20->isConversionComplete()) {
        _temperature = ds18b20->getTempC();
        ds18b20->setResolution(12);
        ds18b20->requestTemperatures();
        tLastReading = millis();
        Log.traceln(F("DS18B20 temp: %FC %FF"), _temperature, _temperature*1.8+32);
      } else {
        //Log.infoln(F("DS18B20 conversion not complete"));
      }
    #endif
    #ifdef TEMP_SENSOR_BME280
      _temperature = _bme->readTemperature();
      _humidity = _bme->readHumidity();
      _baro_pressure = _bme->readPressure();
      tLastReading = millis();
    #endif
    #ifdef TEMP_SENSOR_DHT
      if (millis() - tLastReading > minDelayMs) {
        sensors_event_t event;
        // temperature
        _dht->temperature().getEvent(&event);
        if (isnan(event.temperature)) {
          //Log.warningln(F("Error reading DHT temperature!"));
        } else {
          _temperature = event.temperature;
          Log.traceln(F("DHT temp: %FC %FF"), _temperature, _temperature*1.8+32);
        }
        // humidity
        _dht->humidity().getEvent(&event);
        if (isnan(event.relative_humidity)) {
          //Log.warningln(F("Error reading DHT humidity!"));
        }
        else {
          _humidity = event.relative_humidity;
          Log.traceln(F("DHT humidity: %F%%"), _humidity);
        }
        
        tLastReading = millis();
      }
    #endif
  }

  #if !defined(ESP8266) || (defined(ESP8266) && defined(DISABLE_LOGGING))
  hp.sync();
  #endif

}

#if defined(TEMP_SENSOR_DS18B20) || defined(TEMP_SENSOR_DHT) || defined(TEMP_SENSOR_BME280)
float CDevice::getTemperature(bool *current) {
  if (current != NULL) { 
    *current = millis() - tLastReading < STALE_READING_AGE_MS; 
  }
  return _temperature;
}
#else
float CDevice::getTemperature(bool *current) {
  if (tsHPStatusUpdated > 0) {
    return hpStatus.roomTemperature;
  }
  return 0;
}
#endif

#if defined(TEMP_SENSOR_DHT) || defined(TEMP_SENSOR_BME280)
float CDevice::getHumidity(bool *current) {
  if (current != NULL) { 
    *current = millis() - tLastReading < STALE_READING_AGE_MS; 
  }
  return _humidity;
}
#endif

#if defined(TEMP_SENSOR_BME280)
float CDevice::getBaroPressure(bool *current) {
  if (current != NULL) { 
    *current = millis() - tLastReading < STALE_READING_AGE_MS; 
  }
  return _baro_pressure;
}
#endif

#ifdef BATTERY_SENSOR
float CDevice::getBatteryVoltage(bool *current) {  
  if (current != NULL) { *current = true; } 
  int vi = analogRead(BATTERY_SENSOR_ADC_PIN);
  float v = (float)vi/configuration.battVoltsDivider;
  Log.verboseln(F("Battery voltage raw: %i volts: %D"), vi, v);
  return v; 
}
#endif

JsonDocument& CDevice::getACSettings() {

  jsonHPSettings["ts"] = millis();
  jsonHPSettings["tsHPSettingsUpdated"] = tsHPSettingsUpdated;
  jsonHPSettings["tsHPStatusUpdated"] = tsHPStatusUpdated;

  if (tsHPSettingsUpdated > 0) {
    jsonHPSettings["connected"] = hpSettings.connected;
    jsonHPSettings["power"] = hpSettings.power;
    jsonHPSettings["mode"] = hpSettings.mode;
    jsonHPSettings["temperature"] = hpSettings.temperature;
    jsonHPSettings["fan"] = hpSettings.fan;
    jsonHPSettings["vane"] = hpSettings.vane;
    jsonHPSettings["wideVane"] = hpSettings.wideVane;
  }

  if (tsHPStatusUpdated > 0) {
    jsonHPSettings["roomTemperature"] = hpStatus.roomTemperature;
    jsonHPSettings["operating"] = hpStatus.operating;
    jsonHPSettings["compressorFrequency"] = hpStatus.compressorFrequency;
  }

  #ifdef DEBUG_MOCK_HP
    jsonHPSettings["tsHPSettingsUpdated"] = millis();
    jsonHPSettings["tsHPStatusUpdated"] = millis();
    jsonHPSettings["connected"] = true;
    jsonHPSettings["power"] = "ON";
    jsonHPSettings["mode"] = "COOL";
    jsonHPSettings["temperature"] = 23.32;
    jsonHPSettings["fan"] = "AUTO";
    jsonHPSettings["vane"] = "AUTO";;
    jsonHPSettings["wideVane"] = "|";
    jsonHPSettings["roomTemperature"] = 32.23;
    jsonHPSettings["operating"] = true;
    jsonHPSettings["compressorFrequency"] = 0;
  #endif

  return jsonHPSettings;
}
