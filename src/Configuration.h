#pragma once

#include <Arduino.h>
#include <functional>
#include <ArduinoLog.h>

//#define DEBUG_MOCK_HP
#define DISABLE_LOGGING
#ifndef DISABLE_LOGGING
  #define LOG_LEVEL LOG_LEVEL_VERBOSE
#endif

#define WIFI    // 2.4Ghz wifi access point

#define EEPROM_FACTORY_RESET 0       // Byte to be used for factory reset device fails to start or is rebooted within 1 sec 3 consequitive times
#define EEPROM_CONFIGURATION_START 1   // First EEPROM byte to be used for storing the configuration

#define FACTORY_RESET_CLEAR_TIMER_MS 2000   // Clear factory reset counter when elapsed, considered smooth boot

#if defined(ESP32)
  #define DEVICE_NAME "ESP32MHVAC"
#elif defined(ESP8266)
  #define DEVICE_NAME "ESP8266MHVAC"
#endif

#ifdef WIFI
  #define WIFI_SSID DEVICE_NAME
  #define WIFI_PASS "password123"

  // If unable to connect, it will create a soft accesspoint
  #define WIFI_FALLBACK_SSID DEVICE_NAME // device chip id will be suffixed
  #define WIFI_FALLBACK_PASS "password123"

  #define NTP_SERVER "pool.ntp.org"
  #define NTP_GMT_OFFSET_SEC -25200
  #define NTP_DAYLIGHT_OFFSET_SEC 0

  // Web server
  #define WEB_SERVER_PORT 80
#endif

// #define BATTERY_SENSOR  // ADC A0 using 0-3.3v voltage divider
#ifdef BATTERY_SENSOR
  #define BATTERY_VOLTS_DIVIDER 57.2 // 162.3 - 4.2v max; 57.2 - 14.8v max;
  #if defined(ESP32)
    #define BATTERY_SENSOR_ADC_PIN  A0
  #elif defined(ESP8266)
    #define BATTERY_SENSOR_ADC_PIN  A0
  #elif defined(SEEED_XIAO_M0)
    #define BATTERY_SENSOR_ADC_PIN  D1
  #else
    #define BATTERY_SENSOR_ADC_PIN  0
  #endif
#endif

// #define TEMP_SENSOR
#ifdef TEMP_SENSOR
  #define TEMP_UNIT_CELSIUS     0
  #define TEMP_UNIT_FAHRENHEIT  1
  //#define TEMP_SENSOR_DS18B20
  //#define TEMP_SENSOR_BME280
  #define TEMP_SENSOR_DHT
  #ifdef TEMP_SENSOR_DHT
    #define TEMP_SENSOR_DHT_TYPE   DHT22
  #endif
  #ifdef TEMP_SENSOR_BME280
    #define BME_SEALEVELPRESSURE_HPA (1013.25)
    #define BME_I2C_ID 0x76
  #endif
  #if defined(ESP32)
    #define TEMP_SENSOR_PIN 0
  #elif defined(ESP8266)
    #define TEMP_SENSOR_PIN D3
  #elif defined(SEEED_XIAO_M0)
    #define TEMP_SENSOR_PIN D4
  #endif
#endif

#define HVAC_CTRL
#ifdef HVAC_CTRL
  #define TEMP_UNIT_CELSIUS     0
  #define TEMP_UNIT_FAHRENHEIT  1
#endif

#define INTERNAL_LED_PIN LED_BUILTIN

struct configuration_t {

  #ifdef WIFI
    char wifiSsid[32];
    char wifiPassword[63];
    
    // ntp
    char ntpServer[128];
    long gmtOffset_sec;
    int daylightOffset_sec;

    // mqtt
    char mqttServer[128];
    uint16_t mqttPort;
    char mqttTopic[128];
  #endif

  char name[128];
  #ifdef BATTERY_SENSOR
    float battVoltsDivider;
  #endif
  #if defined(TEMP_SENSOR) || defined(HVAC_CTRL)
    uint8_t tempUnit;
  #endif

  char _loaded[7]; // used to check if EEPROM was correctly set
};

extern configuration_t configuration;

uint8_t EEPROM_initAndCheckFactoryReset();
void EEPROM_clearFactoryReset();

void EEPROM_saveConfig();
void EEPROM_loadConfig();
void EEPROM_wipe();

uint32_t CONFIG_getDeviceId();
unsigned long CONFIG_getUpTime();

void intLEDOn();
void intLEDOff();
void intLEDBlink(uint16_t ms);